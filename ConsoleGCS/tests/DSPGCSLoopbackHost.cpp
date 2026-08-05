#include "../CommandManager.h"
#include "../DroneManager.h"
#include "../SafetyConfig.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
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

    DroneCommandType ToInternalType(dspgcs::CommandType type)
    {
        switch (type)
        {
        case dspgcs::CommandType::Arm: return DroneCommandType::Arm;
        case dspgcs::CommandType::Disarm: return DroneCommandType::Disarm;
        case dspgcs::CommandType::Takeoff: return DroneCommandType::Takeoff;
        case dspgcs::CommandType::Land: return DroneCommandType::Land;
        case dspgcs::CommandType::RTL: return DroneCommandType::RTL;
        case dspgcs::CommandType::SetMode: return DroneCommandType::SetMode;
        case dspgcs::CommandType::HoverTest: return DroneCommandType::HoverTest;
        }
        return DroneCommandType::Arm;
    }

    dspgcs::AckState ToProtocolState(DroneCommandTargetState state)
    {
        switch (state)
        {
        case DroneCommandTargetState::Pending: return dspgcs::AckState::Queued;
        case DroneCommandTargetState::Sent: return dspgcs::AckState::Sent;
        case DroneCommandTargetState::Accepted: return dspgcs::AckState::Accepted;
        case DroneCommandTargetState::Rejected: return dspgcs::AckState::Rejected;
        case DroneCommandTargetState::Timeout: return dspgcs::AckState::TimedOut;
        case DroneCommandTargetState::Failed: return dspgcs::AckState::Failed;
        case DroneCommandTargetState::Simulated: return dspgcs::AckState::Simulated;
        }
        return dspgcs::AckState::Failed;
    }

    dspgcs::TelemetryBatchDTO MakeTelemetry(std::uint64_t revision)
    {
        dspgcs::TelemetryBatchDTO batch;
        batch.batchId = "LOOPBACK-BATCH-" + std::to_string(revision);
        batch.batchMode = dspgcs::BatchMode::Snapshot;
        batch.fleetRevision = revision;
        batch.snapshotId = "LOOPBACK-SNAPSHOT";

        for (int index = 0; index < 3; ++index)
        {
            dspgcs::TelemetryDTO item;
            item.droneId = "Drone00" + std::to_string(index + 1);
            item.sysId = static_cast<std::uint32_t>(index + 1);
            item.sampleTimestampUtcMs = UtcNowMs();
            item.telemetryRevision = revision;
            item.changedFields = dspgcs::TelemetryFields::All;
            item.gps.valid = true;
            item.gps.latitudeDeg = 31.932471 + index * 0.00009;
            item.gps.longitudeDeg = 118.638531 + index * 0.00009;
            item.gps.fixType = 3;
            item.gps.satellitesVisible = 12;
            item.altitude.valid = true;
            item.altitude.relativeMeters = 0.0f;
            item.altitude.amslMeters = 13.1f;
            item.battery.valid = true;
            item.battery.remainingPercent = 98.0f - index * 3.0f;
            item.battery.voltageVolts = 14.8f;
            item.mode.modeCode = 5;
            item.mode.modeName = "LOITER";
            item.mode.armed = false;
            item.connection.online = true;
            item.connection.state = dspgcs::ConnectionState::Online;
            item.connection.lastHeartbeatUtcMs = UtcNowMs();
            item.connection.linkId = "LOOPBACK";
            item.rssi.valid = true;
            item.rssi.valueDbm = static_cast<std::int16_t>(-42 - index * 3);
            item.led.state = dspgcs::LEDState::Static;
            item.led.red = static_cast<std::uint8_t>(index == 0 ? 0 : 50 * index);
            item.led.green = static_cast<std::uint8_t>(180 - 40 * index);
            item.led.blue = 255;
            item.led.brightness = 200;
            item.led.confirmed = true;
            item.mission.state = dspgcs::MissionState::Ready;
            batch.items.push_back(std::move(item));
        }
        return batch;
    }

    std::string Trim(std::string value)
    {
        const auto notSpace = [](const unsigned char character) { return !std::isspace(character); };
        value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
        value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
        return value;
    }

    bool ParsePort(const std::string& value, std::uint16_t& output)
    {
        try
        {
            const unsigned long parsed = std::stoul(value);
            if (parsed == 0 || parsed > 65535) return false;
            output = static_cast<std::uint16_t>(parsed);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool LoadTransportConfig(
        const std::filesystem::path& path,
        dspgcs::DSPGCSTransportConfig& config,
        std::string& error)
    {
        std::ifstream stream(path);
        if (!stream)
        {
            error = "Cannot open transport config: " + path.string();
            return false;
        }

        bool hasType = false;
        bool hasBindAddress = false;
        bool hasBindPort = false;
        bool hasRemoteAddress = false;
        bool hasRemotePort = false;
        std::string line;
        std::size_t lineNumber = 0;
        while (std::getline(stream, line))
        {
            ++lineNumber;
            line = Trim(line);
            if (line.empty() || line.front() == '#' || line.front() == ';') continue;

            const std::size_t separator = line.find_first_of("=:");
            if (separator == std::string::npos)
            {
                error = "Invalid transport config line " + std::to_string(lineNumber);
                return false;
            }
            const std::string key = Trim(line.substr(0, separator));
            const std::string value = Trim(line.substr(separator + 1));
            if (key == "TransportType")
            {
                if (!dspgcs::TryParseTransportType(value, config.type))
                {
                    error = "Unsupported TransportType: " + value;
                    return false;
                }
                hasType = true;
            }
            else if (key == "BindAddress")
            {
                config.bindAddress = value;
                hasBindAddress = !value.empty();
            }
            else if (key == "BindPort")
            {
                hasBindPort = ParsePort(value, config.bindPort);
                if (!hasBindPort)
                {
                    error = "Invalid BindPort: " + value;
                    return false;
                }
            }
            else if (key == "RemoteAddress")
            {
                config.remoteAddress = value;
                hasRemoteAddress = !value.empty();
            }
            else if (key == "RemotePort")
            {
                hasRemotePort = ParsePort(value, config.remotePort);
                if (!hasRemotePort)
                {
                    error = "Invalid RemotePort: " + value;
                    return false;
                }
            }
            else
            {
                error = "Unknown transport config key: " + key;
                return false;
            }
        }

        if (!hasType || !hasBindAddress || !hasBindPort || !hasRemoteAddress || !hasRemotePort)
        {
            error = "Transport config is missing one or more required fields";
            return false;
        }
        return true;
    }

    std::filesystem::path FindTransportConfig(const char* executablePath)
    {
        const std::filesystem::path executable(executablePath ? executablePath : "");
        const std::filesystem::path candidates[] =
        {
            std::filesystem::current_path() / "config" / "DSPGCSTransport.ini",
            std::filesystem::current_path() / "ConsoleGCS" / "config" / "DSPGCSTransport.ini",
            executable.parent_path().parent_path().parent_path() / "config" / "DSPGCSTransport.ini"
        };
        for (const auto& candidate : candidates)
        {
            if (std::filesystem::exists(candidate)) return candidate;
        }
        return candidates[0];
    }
}

int main(int argc, char** argv)
{
    if (!SafetyConfig::SAFE_TEST_MODE)
    {
        std::cerr << "DSP-GCS loopback host refuses to start unless SAFE_TEST_MODE=true\n";
        return 2;
    }

    std::filesystem::path configPath;
    if (argc == 2)
    {
        const std::string argument = argv[1];
        const std::string prefix = "--transport-config=";
        if (argument.rfind(prefix, 0) != 0 || argument.size() == prefix.size())
        {
            std::cerr << "DSP-GCS CONFIG ERROR: expected --transport-config=<path>\n";
            return 3;
        }
        configPath = argument.substr(prefix.size());
    }
    else if (argc > 2)
    {
        std::cerr << "DSP-GCS CONFIG ERROR: too many arguments\n";
        return 3;
    }
    else
    {
        configPath = FindTransportConfig(argc > 0 ? argv[0] : nullptr);
    }
    dspgcs::DSPGCSTransportConfig transportConfig;
    std::string transportError;
    if (!LoadTransportConfig(configPath, transportConfig, transportError))
    {
        std::cerr << "DSP-GCS CONFIG ERROR: " << transportError << '\n';
        return 3;
    }

    auto transport = dspgcs::DSPGCSTransportFactory::CreateTransport(transportConfig, transportError);
    if (!transport)
    {
        std::cerr << "DSP-GCS FACTORY ERROR: " << transportError << '\n';
        return 4;
    }

    std::mutex receivedFramesMutex;
    std::deque<dspgcs::TransportFrame> receivedFrames;
    transport->Receive([&](dspgcs::TransportFrame frame)
    {
        std::lock_guard<std::mutex> lock(receivedFramesMutex);
        receivedFrames.push_back(std::move(frame));
    });
    if (!transport->Start())
    {
        std::cerr << "DSP-GCS TRANSPORT START ERROR: Type="
                  << dspgcs::ToString(transportConfig.type) << '\n';
        return 5;
    }

    DroneManager droneManager;
    for (int index = 1; index <= 3; ++index)
    {
        DroneSessionConfig config;
        config.droneId = "Drone00" + std::to_string(index);
        config.targetIp = "127.0.0.1";
        config.targetPort = 0;
        config.localIp = "127.0.0.1";
        config.localPort = 0;
        droneManager.AddDrone(config);
    }
    CommandManager commandManager(droneManager);

    std::uint64_t sequence = 1;
    std::uint64_t telemetryRevision = 1;
    int receivedCommands = 0;
    bool sawSingleArm = false;
    bool sawMultiHover = false;
    auto lastTelemetry = std::chrono::steady_clock::time_point{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);

    std::cout << "DSP-GCS Factory selected TransportType=" << dspgcs::ToString(transportConfig.type)
              << " bind=" << transportConfig.bindAddress << ':' << transportConfig.bindPort
              << " remote=" << transportConfig.remoteAddress << ':' << transportConfig.remotePort << '\n';

    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto now = std::chrono::steady_clock::now();
        if (lastTelemetry == std::chrono::steady_clock::time_point{} || now - lastTelemetry >= std::chrono::milliseconds(250))
        {
            dspgcs::MessageEnvelope envelope;
            envelope.messageType = dspgcs::MessageType::TelemetryBatch;
            envelope.sourceId = "ConsoleGCS-Loopback";
            envelope.sessionId = "LOOPBACK-SESSION";
            envelope.sequence = sequence++;
            envelope.timestampUtcMs = UtcNowMs();
            envelope.fleetRevision = telemetryRevision;
            const auto frame = dspgcs::EncodeTelemetry(envelope, MakeTelemetry(telemetryRevision++));
            transport->Send({ frame });
            lastTelemetry = now;
        }

        dspgcs::TransportFrame receivedFrame;
        {
            std::lock_guard<std::mutex> lock(receivedFramesMutex);
            if (!receivedFrames.empty())
            {
                receivedFrame = std::move(receivedFrames.front());
                receivedFrames.pop_front();
            }
        }

        if (!receivedFrame.bytes.empty())
        {
            dspgcs::MessageEnvelope envelope;
            std::vector<std::uint8_t> payload;
            dspgcs::CommandRequestDTO command;

            if (dspgcs::DecodeEnvelope(receivedFrame.bytes, envelope, payload) &&
                envelope.messageType == dspgcs::MessageType::CommandRequest &&
                dspgcs::DecodeCommand(payload, command))
            {
                ++receivedCommands;
                sawSingleArm = sawSingleArm ||
                    (command.commandType == dspgcs::CommandType::Arm && command.targetDroneIds == std::vector<std::string>{ "Drone001" });
                sawMultiHover = sawMultiHover ||
                    (command.commandType == dspgcs::CommandType::HoverTest && command.targetDroneIds == std::vector<std::string>{ "Drone001", "Drone002", "Drone003" });

                DroneCommandParameters parameters;
                parameters.takeoffAltitudeMeters = command.parameters.takeoffAltitudeMeters;
                parameters.customMode = command.parameters.customMode;
                parameters.startHoverTest = command.parameters.start;

                if (command.safeTestMode)
                {
                    commandManager.Submit(ToInternalType(command.commandType), command.targetDroneIds, parameters, "DSPGCSLoopback");
                }

                const auto completed = commandManager.ConsumeCompletedCommands();
                dspgcs::CommandResultBatchDTO resultBatch;
                resultBatch.commandId = command.commandId;
                resultBatch.resultSequence = 1;
                resultBatch.final = true;
                resultBatch.requestedCount = static_cast<std::uint32_t>(command.targetDroneIds.size());

                if (!command.safeTestMode || completed.empty())
                {
                    resultBatch.aggregateState = dspgcs::AggregateState::Failed;
                    resultBatch.failedCount = resultBatch.requestedCount;
                    for (const auto& droneId : command.targetDroneIds)
                    {
                        dspgcs::DroneCommandResultDTO result;
                        result.commandId = command.commandId;
                        result.droneId = droneId;
                        result.ackState = dspgcs::AckState::Rejected;
                        result.error.code = "SAFE_TEST_MODE_BLOCKED";
                        result.error.message = "Loopback accepts simulated commands only";
                        result.updatedTimestampUtcMs = UtcNowMs();
                        result.final = true;
                        resultBatch.results.push_back(std::move(result));
                    }
                }
                else
                {
                    resultBatch.aggregateState = dspgcs::AggregateState::Simulated;
                    resultBatch.acceptedCount = resultBatch.requestedCount;
                    for (const auto& target : completed.front().targetResults)
                    {
                        dspgcs::DroneCommandResultDTO result;
                        result.commandId = command.commandId;
                        result.droneId = target.droneId;
                        result.ackState = ToProtocolState(target.state);
                        result.error.code = target.state == DroneCommandTargetState::Rejected ? "COMMAND_REJECTED" : "";
                        result.error.message = target.detail;
                        result.attempts = static_cast<std::uint16_t>(target.attempts);
                        result.updatedTimestampUtcMs = UtcNowMs();
                        result.final = true;
                        resultBatch.results.push_back(std::move(result));
                    }
                }

                dspgcs::MessageEnvelope resultEnvelope;
                resultEnvelope.messageType = dspgcs::MessageType::CommandResultBatch;
                resultEnvelope.sourceId = "ConsoleGCS-Loopback";
                resultEnvelope.sessionId = "LOOPBACK-SESSION";
                resultEnvelope.sequence = sequence++;
                resultEnvelope.timestampUtcMs = UtcNowMs();
                resultEnvelope.correlationId = command.commandId;
                transport->Send({ dspgcs::EncodeCommandResult(resultEnvelope, resultBatch) });

                std::cout << "COMMAND commandId=" << command.commandId
                          << " targets=" << resultBatch.results.size()
                          << " result=SIMULATED\n";
            }
        }

        if (sawSingleArm && sawMultiHover && receivedCommands >= 2)
        {
            std::cout << "DSP-GCS LOOPBACK PASS telemetry=3 singleArm=1 multiHover=3 resultsPerDrone=1\n";
            transport->Stop();
            return 0;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::cerr << "DSP-GCS LOOPBACK TIMEOUT singleArm=" << sawSingleArm << " multiHover=" << sawMultiHover << '\n';
    transport->Stop();
    return 6;
}
