#include "../AppConfig.h"
#include "../CommandManager.h"
#include "../ConsoleDSPGCSLoopbackTransport.h"
#include "../DSPGCSProductionCommandReceiver.h"
#include "../DSPGCSProductionTelemetryProvider.h"
#include "../DroneManager.h"
#include "../SafetyConfig.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::int64_t UtcNowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    bool Check(const bool condition, const char* message)
    {
        if (!condition) std::cerr << "FAILED: " << message << '\n';
        return condition;
    }

    std::vector<std::string> MakeDroneIds(const std::size_t count)
    {
        std::vector<std::string> result;
        result.reserve(count);
        for (std::size_t index = 1; index <= count; ++index)
        {
            std::string suffix = std::to_string(index);
            suffix.insert(0, 3 - suffix.size(), '0');
            result.push_back("Drone" + suffix);
        }
        return result;
    }

    dspgcs::TransportFrame MakeCommandFrame(
        const std::string& commandId,
        const dspgcs::CommandType type,
        std::vector<std::string> targets,
        const float hoverAltitude = 0.0f,
        const float hoverDuration = 0.0f,
        const float takeoffAltitude = 0.0f,
        const std::int32_t customMode = -1,
        const bool start = true)
    {
        dspgcs::CommandRequestDTO request;
        request.commandId = commandId;
        request.commandType = type;
        request.targetDroneIds = std::move(targets);
        request.parameters.hoverAltitudeMeters = hoverAltitude;
        request.parameters.hoverDurationSeconds = hoverDuration;
        request.parameters.takeoffAltitudeMeters = takeoffAltitude;
        request.parameters.customMode = customMode;
        request.parameters.start = start;
        request.source.sourceType = "UI";
        request.source.sourceId = "UnrealGCS-Test";
        request.source.stationId = "GCS-TEST";
        request.timestampUtcMs = UtcNowMs();
        request.safeTestMode = true;

        dspgcs::MessageEnvelope envelope;
        envelope.messageType = dspgcs::MessageType::CommandRequest;
        envelope.sourceId = "UnrealGCS-ProductionCommand-Test";
        envelope.sessionId = "UE-TEST";
        envelope.timestampUtcMs = request.timestampUtcMs;
        envelope.correlationId = commandId;
        return { dspgcs::EncodeCommand(envelope, request) };
    }
}

int RunDSPCommandTests()
{
    if (!SafetyConfig::SAFE_TEST_MODE)
    {
        std::cerr << "FAILED: SAFE_TEST_MODE must be true\n";
        return 2;
    }

    DroneManager droneManager;
    const std::vector<std::string> allDroneIds = MakeDroneIds(8);
    for (const std::string& droneId : allDroneIds)
    {
        DroneSessionConfig config;
        config.droneId = droneId;
        config.remoteIp = "127.0.0.1";
        config.remotePort = 0;
        config.receiveOnly = false;
        if (!droneManager.AddDrone(config) ||
            !droneManager.StartDrone(config.droneId))
        {
            std::cerr << "FAILED: unable to create simulated Session\n";
            return 3;
        }
    }

    CommandManager commandManager(droneManager);
    DSPGCSProductionCommandReceiver commandReceiver;
    DSPGCSProductionTelemetryProvider productionChannel;
    productionChannel.SetReceiveHandler(
        [&commandReceiver](dspgcs::TransportFrame frame)
        {
            commandReceiver.EnqueueFrame(std::move(frame));
        });

    dspgcs::DSPGCSTransportConfig consoleConfig;
    consoleConfig.type = dspgcs::TransportType::Loopback;
    consoleConfig.bindAddress = "127.0.0.1";
    consoleConfig.bindPort = 12531;
    consoleConfig.remoteAddress = "127.0.0.1";
    consoleConfig.remotePort = 12532;
    std::string error;
    if (!productionChannel.Start(
            consoleConfig,
            AppConfigDefaults::SnapshotDatagramBytes,
            AppConfigDefaults::SnapshotItemsPerChunk,
            error))
    {
        std::cerr << "FAILED: production channel start: " << error << '\n';
        return 4;
    }

    dspgcs::DSPGCSTransportConfig ueConfig = consoleConfig;
    ueConfig.bindPort = consoleConfig.remotePort;
    ueConfig.remotePort = consoleConfig.bindPort;
    auto ueTransport =
        dspgcs::DSPGCSTransportFactory::CreateTransport(ueConfig, error);
    if (!ueTransport)
    {
        std::cerr << "FAILED: UE transport factory: " << error << '\n';
        return 5;
    }

    std::mutex resultMutex;
    std::deque<dspgcs::CommandResultBatchDTO> results;
    ueTransport->Receive([&](dspgcs::TransportFrame frame)
    {
        dspgcs::MessageEnvelope envelope;
        std::vector<std::uint8_t> payload;
        dspgcs::CommandResultBatchDTO batch;
        if (dspgcs::DecodeEnvelope(frame.bytes, envelope, payload) &&
            envelope.messageType == dspgcs::MessageType::CommandResultBatch &&
            dspgcs::DecodeCommandResult(payload, batch))
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            results.push_back(std::move(batch));
        }
    });
    if (!ueTransport->Start())
    {
        std::cerr << "FAILED: UE transport start\n";
        return 6;
    }

    bool passed = true;

    // Pure safety-policy checks: no transport is started by these calls and no
    // DroneSession or MAVLink sender is reached.
    const std::vector<std::string> threeTargets(
        allDroneIds.begin(), allDroneIds.begin() + 3);
    const std::vector<std::string> fiveTargets(
        allDroneIds.begin(), allDroneIds.begin() + 5);
    const std::vector<std::string> eightTargets = allDroneIds;

    dspgcs::CommandRequestDTO emptyTargetArm;
    emptyTargetArm.commandType = dspgcs::CommandType::Arm;
    const DSPGCSRealCommandValidation emptyTargetValidation =
        DSPGCSProductionCommandReceiver::ValidateRealCommand(emptyTargetArm);
    passed &= Check(
        !emptyTargetValidation.allowed &&
        emptyTargetValidation.errorCode == "REAL_COMMAND_TARGET_REQUIRED",
        "real command with no target rejected");

    dspgcs::CommandRequestDTO threeDroneArm;
    threeDroneArm.commandType = dspgcs::CommandType::Arm;
    threeDroneArm.targetDroneIds = threeTargets;
    passed &= Check(
        DSPGCSProductionCommandReceiver::ValidateRealCommand(threeDroneArm).allowed,
        "real three-drone ARM accepted by safety policy");

    dspgcs::CommandRequestDTO overLimitTakeoff;
    overLimitTakeoff.commandType = dspgcs::CommandType::Takeoff;
    overLimitTakeoff.targetDroneIds = eightTargets;
    overLimitTakeoff.parameters.takeoffAltitudeMeters = 5.1f;
    const DSPGCSRealCommandValidation overLimitValidation =
        DSPGCSProductionCommandReceiver::ValidateRealCommand(
            overLimitTakeoff);
    passed &= Check(
        !overLimitValidation.allowed &&
        overLimitValidation.errorCode == "REAL_COMMAND_ALTITUDE_LIMIT",
        "real eight-drone TAKEOFF above configured limit rejected");

    dspgcs::CommandRequestDTO fiveMeterTakeoff;
    fiveMeterTakeoff.commandType = dspgcs::CommandType::Takeoff;
    fiveMeterTakeoff.targetDroneIds = fiveTargets;
    fiveMeterTakeoff.parameters.takeoffAltitudeMeters = 5.0f;
    const DSPGCSRealCommandValidation fiveMeterValidation =
        DSPGCSProductionCommandReceiver::ValidateRealCommand(
            fiveMeterTakeoff);
    passed &= Check(
        fiveMeterValidation.allowed,
        "real five-drone 5m TAKEOFF accepted by safety policy");

    dspgcs::CommandRequestDTO multiDroneHover;
    multiDroneHover.commandType = dspgcs::CommandType::HoverTest;
    multiDroneHover.targetDroneIds = eightTargets;
    multiDroneHover.parameters.hoverAltitudeMeters = 5.0f;
    passed &= Check(
        DSPGCSProductionCommandReceiver::ValidateRealCommand(multiDroneHover).allowed,
        "real eight-drone 5m HOVER_TEST accepted by safety policy");

    for (const int customMode :
        { ArduCopterCustomMode::Stabilize,
          ArduCopterCustomMode::Auto,
          ArduCopterCustomMode::Guided })
    {
        dspgcs::CommandRequestDTO modeRequest;
        modeRequest.commandType = dspgcs::CommandType::SetMode;
        modeRequest.targetDroneIds = fiveTargets;
        modeRequest.parameters.customMode = customMode;
        const std::string validationMessage =
            std::string("real five-drone ") +
            ArduCopterCustomMode::Name(customMode) +
            " accepted by safety policy";
        passed &= Check(
            DSPGCSProductionCommandReceiver::ValidateRealCommand(modeRequest).allowed,
            validationMessage.c_str());
    }

    dspgcs::CommandRequestDTO invalidModeRequest;
    invalidModeRequest.commandType = dspgcs::CommandType::SetMode;
    invalidModeRequest.targetDroneIds = fiveTargets;
    invalidModeRequest.parameters.customMode = 42;
    passed &= Check(
        !DSPGCSProductionCommandReceiver::ValidateRealCommand(invalidModeRequest).allowed,
        "unsupported Copter custom mode rejected by safety policy");

    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-ARM-5",
            dspgcs::CommandType::Arm,
            fiveTargets)),
        "multi ARM frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-DISARM-3",
            dspgcs::CommandType::Disarm,
            threeTargets)),
        "multi DISARM frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-GUIDED-8",
            dspgcs::CommandType::SetMode,
            eightTargets,
            0.0f,
            0.0f,
            0.0f,
            4)),
        "multi GUIDED frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-STABILIZE-5",
            dspgcs::CommandType::SetMode,
            fiveTargets,
            0.0f,
            0.0f,
            0.0f,
            0)),
        "multi STABILIZE frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-AUTO-3",
            dspgcs::CommandType::SetMode,
            threeTargets,
            0.0f,
            0.0f,
            0.0f,
            3)),
        "multi AUTO frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-TAKEOFF-5",
            dspgcs::CommandType::Takeoff,
            fiveTargets,
            0.0f,
            0.0f,
            5.0f)),
        "multi TAKEOFF frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-RTL-8",
            dspgcs::CommandType::RTL,
            eightTargets)),
        "multi RTL frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-STOP-3",
            dspgcs::CommandType::HoverTest,
            threeTargets,
            0.0f,
            0.0f,
            0.0f,
            -1,
            false)),
        "multi STOP frame sent");
    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-HOVER-5",
            dspgcs::CommandType::HoverTest,
            fiveTargets,
            5.0f)),
        "multi HOVER_TEST frame sent");

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        commandReceiver.ProcessPending(
            commandManager,
            [&productionChannel](const dspgcs::CommandResultBatchDTO& batch)
            {
                return productionChannel.SendCommandResult(batch);
            });
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            if (results.size() >= 9) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::deque<dspgcs::CommandResultBatchDTO> captured;
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        captured.swap(results);
    }

    struct ExpectedBatch
    {
        std::string commandId;
        std::vector<std::string> targets;
        bool simulated = true;
    };
    const std::vector<ExpectedBatch> expectedBatches =
    {
        { "UE-TEST-ARM-5", fiveTargets },
        { "UE-TEST-DISARM-3", threeTargets },
        { "UE-TEST-GUIDED-8", eightTargets },
        { "UE-TEST-STABILIZE-5", fiveTargets },
        { "UE-TEST-AUTO-3", threeTargets },
        { "UE-TEST-TAKEOFF-5", fiveTargets },
        { "UE-TEST-RTL-8", eightTargets },
        { "UE-TEST-STOP-3", threeTargets, false },
        { "UE-TEST-HOVER-5", fiveTargets }
    };
    for (const ExpectedBatch& expected : expectedBatches)
    {
        const auto batch = std::find_if(
            captured.begin(),
            captured.end(),
            [&expected](const dspgcs::CommandResultBatchDTO& candidate)
            {
                return candidate.commandId == expected.commandId;
            });
        passed &= Check(batch != captured.end(), "multi-command result returned");
        if (batch == captured.end())
        {
            continue;
        }

        passed &= Check(
            batch->results.size() == expected.targets.size(),
            "multi-command result count equals TargetDroneIDs count");
        for (std::size_t index = 0; index < batch->results.size(); ++index)
        {
            passed &= Check(
                index < expected.targets.size() &&
                batch->results[index].droneId == expected.targets[index],
                "multi-command result retained target DroneID");
            const bool expectedState = expected.simulated
                ? batch->results[index].ackState == dspgcs::AckState::Simulated
                : batch->results[index].ackState == dspgcs::AckState::Accepted;
            passed &= Check(expectedState, "per-drone result state is correct");
        }
    }

    std::size_t waitingCommandCount = 0;
    for (const std::string& droneId : fiveTargets)
    {
        const DroneSession* session = droneManager.GetDrone(droneId);
        passed &= Check(session != nullptr, "Hover target Session exists");
        if (!session)
        {
            continue;
        }

        passed &= Check(
            session->GetHoverTargetAltitudeMeters() == 5.0f,
            "Hover altitude propagated to each Session");
        if (session->GetHoverMissionState() == HoverMissionState::WaitingCommand)
        {
            ++waitingCommandCount;
        }
    }
    passed &= Check(
        waitingCommandCount == fiveTargets.size(),
        "all Hover missions remain WaitingCommand");

    passed &= Check(
        ueTransport->Send(MakeCommandFrame(
            "UE-TEST-LAND-5",
            dspgcs::CommandType::Land,
            fiveTargets)),
        "manual multi LAND frame sent after WaitingCommand");

    const auto landDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < landDeadline)
    {
        commandReceiver.ProcessPending(
            commandManager,
            [&productionChannel](const dspgcs::CommandResultBatchDTO& batch)
            {
                return productionChannel.SendCommandResult(batch);
            });
        {
            std::lock_guard<std::mutex> lock(resultMutex);
            if (!results.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    dspgcs::CommandResultBatchDTO landBatch;
    {
        std::lock_guard<std::mutex> lock(resultMutex);
        if (!results.empty())
        {
            landBatch = std::move(results.front());
            results.pop_front();
        }
    }
    passed &= Check(
        landBatch.commandId == "UE-TEST-LAND-5" &&
        landBatch.results.size() == fiveTargets.size(),
        "manual LAND returned one result for every selected drone");
    for (const std::string& droneId : fiveTargets)
    {
        const DroneSession* session = droneManager.GetDrone(droneId);
        passed &= Check(
            session != nullptr &&
            session->GetHoverMissionState() == HoverMissionState::Idle,
            "manual LAND ended the selected Session hover state");
    }
    passed &= Check(
        commandReceiver.GetReceivedCommandCount() == 10,
        "HoverTest generated no automatic LAND/RTL command");

    ueTransport->Stop();
    productionChannel.Stop();
    droneManager.StopAll();

    if (!passed) return 7;
    std::cout
        << "DSP-GCS COMMAND PASS\n"
        << "targetSizes=N3,N5,N8 dynamicTaskExpansion=PASS\n"
        << "multiArm=5 multiDisarm=3 multiStabilize=5 multiAuto=3 multiGuided=8 multiTakeoff=5\n"
        << "multiLand=5 multiRTL=8 multiStop=3 multiHover=5\n"
        << "commandBatches=10 resultsMatchTargetCount=PASS\n"
        << "N5 Hovering=5 WaitingCommand=5 manualLand=5 "
           "automaticLand=0 automaticRTL=0\n"
        << "realMultiTarget=ALLOWED realTakeoff5m=ALLOWED_BY_POLICY "
           "realTakeoff5.1m=REJECTED realHover5m=ALLOWED_BY_POLICY\n"
        << "CommandID=present Source=UI:UnrealGCS-Test Result=SIMULATED\n";
    return 0;
}
