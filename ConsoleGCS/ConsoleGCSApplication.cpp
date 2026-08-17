/**
 * @brief 实现 ConsoleGCS 启动校验和主服务循环。
 *
 * 分类：Production；当前配置只进入多机 MAVLink Production 路径。
 * 输入：gcs-config.ini；输出：MAVLink 收发、DSP-GCS 快照/命令处理和 ConsoleUI 摘要。
 * 调用链：main -> Run -> RunProductionMavlink。
 */
#define WIN32_LEAN_AND_MEAN

#include "ConsoleGCSApplication.h"

#include "AppConfig.h"
#include "CommandManager.h"
#include "ConsoleUI.h"
#include "DroneManager.h"
#include "DSPGCSFrozenAssignmentReceiver.h"
#include "DSPGCSProductionCommandReceiver.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSTransportConfigLoader.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MavlinkUdpReceiver.h"
#include "SafetyConfig.h"
#include "FrozenAssignmentStore.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    std::string FormatBytes(const int bytes)
    {
        constexpr int bytesPerMiB = 1024 * 1024;
        if (bytes > 0 && bytes % bytesPerMiB == 0)
        {
            return std::to_string(bytes / bytesPerMiB) + "MiB";
        }
        return std::to_string(bytes) + " bytes";
    }

    void LogProductionConfigSummary(
        const AppConfig& appConfig,
        const dspgcs::DSPGCSTransportConfig* transportConfig)
    {
        ConsoleUI::AddLog("[Config]");
        ConsoleUI::AddLog(
            "MAVLink: Bind=" + appConfig.mavlinkBindIP + ":" +
            std::to_string(appConfig.mavlinkBindPort) +
            " HeartbeatTimeout=" +
            std::to_string(appConfig.heartbeatTimeoutMs) + "ms");

        if (transportConfig != nullptr)
        {
            ConsoleUI::AddLog(
                std::string("DSP-GCS: Transport=") +
                dspgcs::ToString(transportConfig->type) +
                " ConsoleEndpoint=" + transportConfig->bindAddress + ":" +
                std::to_string(transportConfig->bindPort) +
                " UnrealEndpoint=" + transportConfig->remoteAddress + ":" +
                std::to_string(transportConfig->remotePort) +
                " SnapshotInterval=" +
                std::to_string(appConfig.dspgcsTelemetryIntervalMs) + "ms");
            ConsoleUI::AddLog(
                "Performance: MavlinkRxBuffer=" +
                FormatBytes(appConfig.mavlinkReceiveBufferBytes) +
                " MaxDrain=" +
                std::to_string(appConfig.maxPacketsPerDrain) +
                " DSPRxBuffer=" +
                FormatBytes(transportConfig->receiveBufferBytes) +
                " DSPPoll=" +
                std::to_string(transportConfig->receiverPollMs) + "ms" +
                " ChunkBytes=" +
                std::to_string(appConfig.snapshotDatagramBytes) +
                " ChunkDrones=" +
                std::to_string(appConfig.snapshotItemsPerChunk));
        }
        else
        {
            ConsoleUI::AddLog("DSP-GCS: Disabled");
            ConsoleUI::AddLog(
                "Performance: MavlinkRxBuffer=" +
                FormatBytes(appConfig.mavlinkReceiveBufferBytes) +
                " MaxDrain=" +
                std::to_string(appConfig.maxPacketsPerDrain));
        }
    }

    std::vector<ConsoleDroneStatus> BuildConsoleFleetStatus(
        const std::vector<DroneSession*>& sessions)
    {
        std::vector<ConsoleDroneStatus> statusRows;
        statusRows.reserve(sessions.size());

        for (const DroneSession* session : sessions)
        {
            if (session == nullptr)
            {
                continue;
            }

            statusRows.push_back(ConsoleDroneStatus{
                session->GetDroneId(),
                session->GetFlightData().online });
        }

        std::sort(
            statusRows.begin(),
            statusRows.end(),
            [](const ConsoleDroneStatus& left, const ConsoleDroneStatus& right)
            {
                return left.droneId < right.droneId;
            });

        return statusRows;
    }
}

int ConsoleGCSApplication::Run(int argc, char* argv[])
{
    const std::string configPath =
        argc > 1 ? argv[1] : "gcs-config.ini";

    AppConfig appConfig;
    std::string configError;

    if (!AppConfig::Load(configPath, appConfig, configError))
    {
        std::cerr
            << configError
            << "\nCopy gcs-config.ini.example to gcs-config.ini "
               "and review the network settings before running.\n";
        return -1;
    }

    SafetyConfig::SetSafeTestMode(appConfig.safeTestMode);
    SafetyConfig::SetRealCommandEnabled(appConfig.realCommandEnabled);
    SafetyConfig::SetFlightAltitudeLimits(
        appConfig.takeoffAltitudeLimitMeters,
        appConfig.hoverAltitudeLimitMeters);

    // Production mode enables real commands only when all explicit runtime
    // gates below are enabled.
    const bool mavlinkControlEnabled =
        SafetyConfig::CanEnableRealCommands(
            appConfig.mavlinkNetworkEnabled,
            appConfig.dspgcsCommandEnabled,
            appConfig.mavlinkBindIP,
            appConfig.mavlinkBindPort);

    ConsoleUI::Init();
    ConsoleUI::AddLog("Drone GCS Started");
    ConsoleUI::AddLog(
        std::string("SAFE_TEST_MODE=") +
        (SafetyConfig::SAFE_TEST_MODE ? "true" : "false"));
    ConsoleUI::AddLog(
        std::string("MAVLinkControlEnabled=") +
        (mavlinkControlEnabled ? "true" : "false"));
    ConsoleUI::AddLog(
        std::string("REAL_COMMAND_ENABLED=") +
        (appConfig.realCommandEnabled ? "true" : "false"));
    ConsoleUI::AddLog(
        std::string("MODE: ") +
        (appConfig.realCommandEnabled
            ? "REAL MULTI VEHICLE TEST"
            : "SIMULATION"));
    ConsoleUI::AddLog("Target limit: selected vehicles (per-Session dispatch)");
    ConsoleUI::AddLog(
        "TAKEOFF altitude limit: " +
        std::to_string(SafetyConfig::TAKEOFF_ALTITUDE_LIMIT_METERS) + "m");
    ConsoleUI::AddLog(
        "HOVER_TEST altitude limit: " +
        std::to_string(SafetyConfig::HOVER_ALTITUDE_LIMIT_METERS) + "m");
    ConsoleUI::AddLog(
        std::string("MAVLinkTelemetryEnabled=") +
        (appConfig.mavlinkNetworkEnabled || mavlinkControlEnabled
            ? "true"
            : "false"));
    ConsoleUI::UpdateFleet({});

    if (!appConfig.mavlinkNetworkEnabled)
    {
        ConsoleUI::AddLog(
            "Startup blocked: mavlink_network_enabled=false; "
            "Legacy Single Session was removed in Cleanup Phase 3C");
        return -2;
    }

    return RunProductionMavlink(appConfig);
}

int ConsoleGCSApplication::RunProductionMavlink(
    const AppConfig& appConfig)
{
    if (appConfig.realCommandEnabled &&
        !SafetyConfig::CanEnableRealCommands(
            appConfig.mavlinkNetworkEnabled,
            appConfig.dspgcsCommandEnabled,
            appConfig.mavlinkBindIP,
            appConfig.mavlinkBindPort))
    {
        ConsoleUI::AddLog(
            "Startup blocked: real commands require explicit MAVLink/DSP "
            "gates and a concrete non-wildcard MAVLink bind endpoint");
        return -2;
    }

    const bool dspgcsEnabled =
        appConfig.dspgcsTelemetryEnabled || appConfig.dspgcsCommandEnabled;
    dspgcs::DSPGCSTransportConfig transportConfig =
        dspgcs::MakeConsoleDSPGCSDefaults();
    if (dspgcsEnabled)
    {
        std::string transportError;
        if (!LoadDSPGCSTransportConfig(
                appConfig.dspgcsTransportConfigPath,
                transportConfig,
                transportError))
        {
            ConsoleUI::AddLog(
                "DSP-GCS production configuration failed: " +
                transportError);
            return -4;
        }
    }
    LogProductionConfigSummary(
        appConfig,
        dspgcsEnabled ? &transportConfig : nullptr);

    MavlinkUdpReceiver receiver;
    std::string receiverError;
    if (!receiver.Start(
        appConfig.mavlinkBindIP,
        static_cast<std::uint16_t>(appConfig.mavlinkBindPort),
        appConfig.mavlinkReceiveBufferBytes,
        receiverError))
    {
        ConsoleUI::AddLog(receiverError);
        return -2;
    }

    DroneManager droneManager;
    CommandManager commandManager(droneManager);
    DSPGCSProductionCommandReceiver commandReceiver;
    DSPGCSFrozenAssignmentReceiver frozenAssignmentReceiver;
    FrozenAssignmentStore frozenAssignmentStore;
    commandReceiver.SetRealCommandEnabled(appConfig.realCommandEnabled);
    DroneSessionConfig discoveredSessionDefaults;
    // Discovery Sessions share the same bound endpoint for receive, heartbeat,
    // commands, and ACK routing. They never create an ephemeral control socket.
    discoveredSessionDefaults.sharedMavlinkEndpoint = &receiver;
    discoveredSessionDefaults.heartbeatTimeoutMs =
        appConfig.heartbeatTimeoutMs;
    discoveredSessionDefaults.receiveOnly =
        !appConfig.dspgcsCommandEnabled || !appConfig.realCommandEnabled;
    discoveredSessionDefaults.realCommandEnabled =
        appConfig.realCommandEnabled;

    MavlinkDiscoveryService discovery(
        droneManager,
        discoveredSessionDefaults);
    MavlinkReadOnlyIngress ingress(droneManager, discovery);
    std::unordered_set<std::string> announcedDroneIds;
    DSPGCSProductionTelemetryProvider telemetryProvider;
    if (dspgcsEnabled)
    {
        telemetryProvider.SetReceiveHandler(
            [&commandReceiver,
             &frozenAssignmentReceiver,
             commandEnabled = appConfig.dspgcsCommandEnabled](
                dspgcs::TransportFrame frame)
            {
                dspgcs::MessageEnvelope envelope;
                std::vector<std::uint8_t> payload;
                if (!dspgcs::DecodeEnvelope(frame.bytes, envelope, payload))
                {
                    return;
                }
                if (envelope.messageType ==
                        dspgcs::MessageType::CommandRequest)
                {
                    if (commandEnabled)
                    {
                        commandReceiver.EnqueueFrame(std::move(frame));
                    }
                    return;
                }
                if (DSPGCSFrozenAssignmentReceiver::
                        IsFrozenAssignmentMessageType(envelope.messageType))
                {
                    frozenAssignmentReceiver.EnqueueFrame(std::move(frame));
                }
            });
    }
    if (dspgcsEnabled)
    {
        std::string transportError;
        if (!telemetryProvider.Start(
                transportConfig,
                static_cast<std::size_t>(appConfig.snapshotDatagramBytes),
                static_cast<std::size_t>(appConfig.snapshotItemsPerChunk),
                transportError))
        {
            ConsoleUI::AddLog("DSP-GCS production channel start failed: " + transportError);
            return -4;
        }
        ConsoleUI::AddLog(
            "DSP-GCS production channel enabled " +
            transportConfig.bindAddress + ":" +
            std::to_string(transportConfig.bindPort) + " -> " +
            transportConfig.remoteAddress + ":" +
            std::to_string(transportConfig.remotePort));
    }

    ConsoleUI::AddLog(
        "Passive registration listening on " +
        appConfig.mavlinkBindIP + ":" +
        std::to_string(receiver.GetBoundPort()));
    ConsoleUI::AddLog(
        appConfig.realCommandEnabled
            ? "DSP-GCS REAL COMMANDS ENABLED: multi-target, per-Session ACK, configured altitude limits active"
            : appConfig.dspgcsCommandEnabled
            ? "DSP-GCS commands enabled: SAFE_TEST_MODE simulation only"
            : "Read-only telemetry mode: command input and MAVLink sends disabled");

    auto lastRefresh = std::chrono::steady_clock::now();
    auto lastTelemetryPublish = std::chrono::steady_clock::time_point{};
    while (true)
    {
        // Drain a full synchronized thousand-aircraft telemetry burst before
        // sleeping. The bound prevents malformed traffic from starving the
        // rest of the service loop.
        for (int datagramIndex = 0;
             datagramIndex < appConfig.maxPacketsPerDrain;
             ++datagramIndex)
        {
            MavlinkReceivedDatagram datagram;
            std::string receiveError;
            const MavlinkUdpReceiveResult receiveResult =
                receiver.Receive(datagram, receiveError);
            if (receiveResult == MavlinkUdpReceiveResult::WouldBlock)
            {
                break;
            }
            if (receiveResult == MavlinkUdpReceiveResult::Error)
            {
                ConsoleUI::AddLog(receiveError);
                return -3;
            }

            const MavlinkIngressStats stats = ingress.SubmitDatagram(
                datagram.bytes.data(),
                datagram.bytes.size(),
                datagram.sourceIp,
                datagram.sourcePort);
            if (stats.identityConflicts > 0)
            {
                ConsoleUI::AddLog(
                    "DroneIdentityConflict source=" + datagram.sourceIp +
                    ":" + std::to_string(datagram.sourcePort));
            }
        }

        const auto now = std::chrono::steady_clock::now();
        discovery.Update(now);
        droneManager.TickAll(now);

        if (appConfig.dspgcsCommandEnabled)
        {
            commandReceiver.ProcessPending(
                commandManager,
                [&telemetryProvider](
                    const dspgcs::CommandResultBatchDTO& resultBatch)
                {
                    return telemetryProvider.SendCommandResult(resultBatch);
                });
        }

        if (telemetryProvider.IsRunning())
        {
            const DSPGCSFrozenAssignmentProcessStats assignmentStats =
                frozenAssignmentReceiver.ProcessPending(
                    telemetryProvider.GetCurrentFleetSessionID(),
                    droneManager,
                    frozenAssignmentStore,
                    [&telemetryProvider](
                        const dspgcs::FrozenAssignmentAckDTO& acknowledgement)
                    {
                        return telemetryProvider.SendFrozenAssignmentAck(
                            acknowledgement);
                    },
                    now);
            if (assignmentStats.transactionsAccepted > 0 ||
                assignmentStats.transactionsRejected > 0)
            {
                ConsoleUI::AddLog(
                    "Frozen Assignment transactions accepted=" +
                    std::to_string(assignmentStats.transactionsAccepted) +
                    " rejected=" +
                    std::to_string(assignmentStats.transactionsRejected));
            }
        }

        if (telemetryProvider.IsRunning() &&
            (lastTelemetryPublish == std::chrono::steady_clock::time_point{} ||
             now - lastTelemetryPublish >=
                std::chrono::milliseconds(appConfig.dspgcsTelemetryIntervalMs)))
        {
            if (!telemetryProvider.PublishSnapshot(droneManager))
            {
                ConsoleUI::AddLog("DSP-GCS telemetry publish failed");
            }
            lastTelemetryPublish = now;
        }

        for (DroneSession* session : droneManager.GetAllDrones())
        {
            if (announcedDroneIds.insert(session->GetDroneId()).second)
            {
                const DroneSessionConfig& config = session->GetConfig();
                ConsoleUI::AddLog(
                    session->GetDroneId() +
                    " registered SYSID=" +
                    std::to_string(session->GetSysId()) +
                    " source=" + config.remoteIp + ":" +
                    std::to_string(config.remotePort));
            }
        }

        if (now - lastRefresh >= std::chrono::seconds(1))
        {
            const std::vector<DroneSession*> sessions =
                droneManager.GetAllDrones();
            ConsoleUI::UpdateFleet(BuildConsoleFleetStatus(sessions));
            lastRefresh = now;
        }

        Sleep(10);
    }
}
