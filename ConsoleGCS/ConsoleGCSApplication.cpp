/**
 * @brief 实现 ConsoleGCS 启动校验和主服务循环。
 *
 * 分类：Production；当前配置只进入多机 MAVLink Production 路径。
 * 输入：gcs-config.ini；输出：MAVLink 收发、DSP-GCS 快照/命令处理和 ConsoleUI 摘要。
 * 调用链：main -> Run -> RunProductionMavlink。
 */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "ConsoleGCSApplication.h"

#include "AppConfig.h"
#include "CommandManager.h"
#include "ConsoleArtifactConsumerV2.h"
#include "ConsoleUploadJobAuthority.h"
#include "ConsoleUI.h"
#include "DroneManager.h"
#include "DSPGCSFrozenAssignmentReceiver.h"
#include "DSPGCSProductionCommandReceiver.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSUploadJobReceiver.h"
#include "DSPGCSTransportConfigLoader.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MavlinkUdpReceiver.h"
#include "MAVFTPMissionHardwareTest.h"
#include "MissionProfileProbe.h"
#include "RouteUploadManager.h"
#include "SafetyConfig.h"
#include "FrozenAssignmentStore.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    class ScopedConsolePresentation
    {
    public:
        ScopedConsolePresentation()
        {
            ConsoleUI::Init();
        }

        ~ScopedConsolePresentation()
        {
            ConsoleUI::Shutdown();
        }

        ScopedConsolePresentation(const ScopedConsolePresentation&) = delete;
        ScopedConsolePresentation& operator=(
            const ScopedConsolePresentation&) = delete;
    };

    template <typename Integer>
    bool ParseInteger(const std::string& text, Integer& output)
    {
        if (text.empty()) return false;
        const char* const begin = text.data();
        const char* const end = begin + text.size();
        const auto parsed = std::from_chars(begin, end, output);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    }

    bool ReadOptionValue(
        const char* const option,
        int& index,
        const int argc,
        char* argv[],
        std::string& value)
    {
        if (index + 1 >= argc || argv[index + 1] == nullptr ||
            std::string(argv[index + 1]).empty())
        {
            std::cerr << option << " requires one value.\n";
            return false;
        }
        value = argv[++index];
        return true;
    }

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
        ConsoleUI::AddLog(
            "MAVFTP: PerClientWriteWindow=" +
            std::to_string(appConfig.mavftpWriteWindow) +
            " ConfiguredGlobalWriteOutstandingCap=" +
            std::to_string(appConfig.mavftpGlobalWriteOutstandingCap));

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
    std::string configPath = "gcs-config.ini";
    bool configPathSet = false;
    MissionProfileProbeOptions probeOptions;
    MAVFTPMissionHardwareTestOptions hardwareTestOptions;
    bool quickOnlyOptionSeen = false;
    bool quickRouteSlotSeen = false;
    bool legacyHardwareEntrySeen = false;
    bool segmentItemsProvidedByCli = false;
    bool terminateTimeoutProvidedByCli = false;
    bool writeWindowProvidedByCli = false;
    std::optional<int> globalWriteOutstandingCapCli;
    for (int index = 1; index < argc; ++index)
    {
        const std::string argument = argv[index] ? argv[index] : "";
        if (argument == "--mission-profile-probe")
        {
            if (index + 1 >= argc || argv[index + 1] == nullptr ||
                std::string(argv[index + 1]).empty())
            {
                std::cerr << "--mission-profile-probe requires one explicit DroneID.\n";
                return -1;
            }
            probeOptions.enabled = true;
            probeOptions.droneId = argv[++index];
        }
        else if (argument == "--allow-mission-write-probe")
        {
            probeOptions.allowWriteProbe = true;
        }
        else if (argument == "--mavftp-test")
        {
            quickOnlyOptionSeen = true;
            hardwareTestOptions.enabled = true;
            hardwareTestOptions.quickTest = true;
            if (!ReadOptionValue("--mavftp-test", index, argc, argv,
                hardwareTestOptions.droneId))
                return -1;
            hardwareTestOptions.confirmedDroneId = hardwareTestOptions.droneId;
        }
        else if (argument == "--route-slot")
        {
            quickOnlyOptionSeen = true;
            quickRouteSlotSeen = true;
            std::string value;
            if (!ReadOptionValue("--route-slot", index, argc, argv, value) ||
                !ParseInteger(value, hardwareTestOptions.routeSlotIndex))
            {
                std::cerr << "--route-slot requires an int32 value.\n";
                return -1;
            }
        }
        else if (argument == "--items")
        {
            quickOnlyOptionSeen = true;
            std::string value;
            if (!ReadOptionValue("--items", index, argc, argv, value) ||
                !ParseInteger(value, hardwareTestOptions.artifactItemLimit))
            {
                std::cerr << "--items requires a uint32 value.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-hardware-test")
        {
            legacyHardwareEntrySeen = true;
            hardwareTestOptions.enabled = true;
            if (!ReadOptionValue("--mavftp-hardware-test", index, argc, argv,
                hardwareTestOptions.droneId))
                return -1;
        }
        else if (argument == "--mavftp-hardware-confirm")
        {
            if (!ReadOptionValue("--mavftp-hardware-confirm", index, argc, argv,
                hardwareTestOptions.confirmedDroneId))
                return -1;
        }
        else if (argument == "--mavftp-artifact-set-id")
        {
            if (!ReadOptionValue("--mavftp-artifact-set-id", index, argc, argv,
                hardwareTestOptions.artifactSetId))
                return -1;
        }
        else if (argument == "--mavftp-artifact-set-digest")
        {
            std::string value;
            if (!ReadOptionValue("--mavftp-artifact-set-digest", index, argc,
                argv, value))
                return -1;
            dronegcs::route_artifact::Error digestError;
            if (!dronegcs::route_artifact::ParseLowerHexDigest(
                value, hardwareTestOptions.artifactSetDigest, digestError))
            {
                std::cerr << "--mavftp-artifact-set-digest must be 64 lowercase hex characters.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-route-slot")
        {
            std::string value;
            if (!ReadOptionValue("--mavftp-route-slot", index, argc, argv, value) ||
                !ParseInteger(value, hardwareTestOptions.routeSlotIndex))
            {
                std::cerr << "--mavftp-route-slot requires an int32 value.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-artifact-items")
        {
            std::string value;
            if (!ReadOptionValue("--mavftp-artifact-items", index, argc, argv,
                value) || !ParseInteger(value, hardwareTestOptions.artifactItemLimit))
            {
                std::cerr << "--mavftp-artifact-items requires a uint32 value.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-segment-items")
        {
            segmentItemsProvidedByCli = true;
            std::string value;
            if (!ReadOptionValue("--mavftp-segment-items", index, argc, argv,
                value) || !ParseInteger(value,
                    hardwareTestOptions.segmentItemCapacity))
            {
                std::cerr << "--mavftp-segment-items requires a uint32 value.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-write-window")
        {
            writeWindowProvidedByCli = true;
            std::string value;
            if (!ReadOptionValue("--mavftp-write-window", index, argc, argv,
                value) || !ParseInteger(value, hardwareTestOptions.writeWindow))
            {
                std::cerr << "--mavftp-write-window requires a uint32 value.\n";
                return -1;
            }
        }
        else if (argument == "--mavftp-global-write-outstanding-cap")
        {
            std::string value;
            int parsed = 0;
            if (!ReadOptionValue("--mavftp-global-write-outstanding-cap",
                    index, argc, argv, value) || !ParseInteger(value, parsed))
            {
                std::cerr << "--mavftp-global-write-outstanding-cap requires an integer value.\n";
                return -1;
            }
            globalWriteOutstandingCapCli = parsed;
        }
        else if (argument == "--mavftp-terminate-timeout-ms")
        {
            terminateTimeoutProvidedByCli = true;
            std::string value;
            if (!ReadOptionValue("--mavftp-terminate-timeout-ms", index,
                argc, argv, value) || !ParseInteger(value,
                    hardwareTestOptions.terminateTimeoutMs))
            {
                std::cerr << "--mavftp-terminate-timeout-ms requires an int32 value.\n";
                return -1;
            }
        }
        else if (!argument.empty() && argument[0] == '-')
        {
            std::cerr << "Unknown command-line option.\n";
            return -1;
        }
        else if (!configPathSet)
        {
            configPath = argument;
            configPathSet = true;
        }
        else
        {
            std::cerr << "Only one configuration path is accepted.\n";
            return -1;
        }
    }
    if (probeOptions.allowWriteProbe && !probeOptions.enabled)
    {
        std::cerr << "--allow-mission-write-probe requires --mission-profile-probe <DroneID>.\n";
        return -1;
    }
    if (probeOptions.enabled && hardwareTestOptions.enabled)
    {
        std::cerr << "MissionProfileProbe and MAVFTP hardware test are mutually exclusive.\n";
        return -1;
    }
    if ((quickOnlyOptionSeen && !hardwareTestOptions.quickTest) ||
        (hardwareTestOptions.quickTest && legacyHardwareEntrySeen))
    {
        std::cerr
            << "Quick MAVFTP options require exactly one --mavftp-test entry "
               "and cannot be combined with the legacy hardware entry.\n";
        return -1;
    }
    if (hardwareTestOptions.quickTest && !quickRouteSlotSeen)
        hardwareTestOptions.routeSlotIndex = 0;
    if (hardwareTestOptions.quickTest && !configPathSet)
        configPath = "gcs-config-upload.ini";
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
    if (!ApplyMavFtpGlobalWriteOutstandingCapCliOverride(
        appConfig, globalWriteOutstandingCapCli, configError))
    {
        std::cerr << configError << '\n';
        return -1;
    }
    std::string hardwareOptionsError;
    if (!ResolveMAVFTPMissionTransportOptions(
            appConfig,
            segmentItemsProvidedByCli,
            terminateTimeoutProvidedByCli,
            writeWindowProvidedByCli,
            hardwareTestOptions,
            hardwareOptionsError) ||
        !ResolveMAVFTPQuickTestOptionsFromCurrent(
            appConfig.artifactWorkspaceRoot,
            hardwareTestOptions,
            hardwareOptionsError) ||
        !ValidateMAVFTPMissionHardwareTestOptions(
            hardwareTestOptions, hardwareOptionsError))
    {
        std::cerr << hardwareOptionsError << '\n';
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

    ScopedConsolePresentation consolePresentation;
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
        std::string("ROUTE_UPLOAD_ENABLED=") +
        (appConfig.routeUploadEnabled ? "true" : "false"));
    ConsoleUI::AddLog(
        "MISSION_TRANSPORT=MAVFTP (frozen production transport)");
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

    if (hardwareTestOptions.enabled &&
        (!appConfig.safeTestMode || !appConfig.realCommandEnabled ||
            !appConfig.routeUploadEnabled))
    {
        ConsoleUI::AddErrorLog(
            "MAVFTP hardware validation requires the existing safe-test, "
            "real-command, and route-upload safety gates; configuration was not changed.");
        return -6;
    }

    return RunProductionMavlink(
        appConfig, probeOptions, hardwareTestOptions);
}

int ConsoleGCSApplication::RunProductionMavlink(
    const AppConfig& appConfig,
    const MissionProfileProbeOptions& probeOptions,
    const MAVFTPMissionHardwareTestOptions& hardwareTestOptions)
{
    if (probeOptions.enabled && appConfig.realCommandEnabled)
    {
        ConsoleUI::AddErrorLog(
            "MissionProfileProbe requires real_command_enabled=false so no "
            "unrelated vehicle command can share the probe process.");
        return -5;
    }
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
    ConsoleArtifactConsumerV2 artifactConsumer(appConfig.artifactWorkspaceRoot);
    ConsoleMavFtpBootConfig mavFtpBootConfig;
    mavFtpBootConfig.globalWriteOutstandingCap =
        appConfig.mavftpGlobalWriteOutstandingCap;
    mavFtpBootConfig.writeWindow = appConfig.mavftpWriteWindow;
    mavFtpBootConfig.segmentItems = appConfig.mavftpSegmentItems;
    mavFtpBootConfig.requestTimeoutMs = static_cast<std::uint32_t>(
        appConfig.mavftpRequestTimeoutMs);
    mavFtpBootConfig.terminateTimeoutMs = static_cast<std::uint32_t>(
        appConfig.mavftpTerminateTimeoutMs);
    mavFtpBootConfig.retryCount = appConfig.mavftpRequestRetryCount;
    ConsoleUploadJobAuthority uploadJobAuthority(
        appConfig.artifactWorkspaceRoot,
        &artifactConsumer,
        &frozenAssignmentStore,
        mavFtpBootConfig);
    RouteUploadManagerConfig routeUploadConfig;
    routeUploadConfig.routeUploadEnabled = appConfig.routeUploadEnabled;
    routeUploadConfig.mavftpSegmentItems = appConfig.mavftpSegmentItems;
    routeUploadConfig.mavftpRequestTimeoutMs = appConfig.mavftpRequestTimeoutMs;
    routeUploadConfig.mavftpTerminateTimeoutMs = appConfig.mavftpTerminateTimeoutMs;
    routeUploadConfig.mavftpRequestRetryCount = appConfig.mavftpRequestRetryCount;
    routeUploadConfig.mavftpWriteWindow = appConfig.mavftpWriteWindow;
    routeUploadConfig.mavftpGlobalWriteOutstandingCap =
        appConfig.mavftpGlobalWriteOutstandingCap;
    RouteUploadManager routeUploadManager(
        uploadJobAuthority, droneManager, routeUploadConfig);
    DSPGCSUploadJobReceiver uploadJobReceiver;
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
    discoveredSessionDefaults.mavFtpServiceEnabled =
        probeOptions.enabled || hardwareTestOptions.enabled ||
        appConfig.routeUploadEnabled;
    discoveredSessionDefaults.mavFtpRequestTimeoutMs =
        appConfig.mavftpRequestTimeoutMs;
    discoveredSessionDefaults.mavFtpTerminateTimeoutMs =
        hardwareTestOptions.enabled
        ? hardwareTestOptions.terminateTimeoutMs
        : appConfig.mavftpTerminateTimeoutMs;
    discoveredSessionDefaults.mavFtpRequestRetryCount =
        appConfig.mavftpRequestRetryCount;
    discoveredSessionDefaults.mavFtpWriteWindow = hardwareTestOptions.enabled
        ? hardwareTestOptions.writeWindow
        : appConfig.mavftpWriteWindow;
    discoveredSessionDefaults.mavFtpWritePermitPool =
        routeUploadManager.GetWritePermitPool();
    discoveredSessionDefaults.missionUploadServiceEnabled =
        appConfig.routeUploadEnabled;

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
             &uploadJobReceiver,
             commandEnabled = appConfig.dspgcsCommandEnabled &&
                !hardwareTestOptions.enabled](
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
                    return;
                }
                if (DSPGCSUploadJobReceiver::IsIngressMessageType(
                        envelope.messageType))
                {
                    uploadJobReceiver.EnqueueFrame(std::move(frame));
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
    const auto probeDiscoveryDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    std::unique_ptr<MissionProfileProbe> missionProfileProbe;
    std::unique_ptr<MAVFTPMissionHardwareTest> hardwareTest;
    if (hardwareTestOptions.enabled)
    {
        hardwareTest = std::make_unique<MAVFTPMissionHardwareTest>(
            artifactConsumer, droneManager, hardwareTestOptions);
        std::string hardwareTestError;
        if (!hardwareTest->Start(
            std::chrono::steady_clock::now(), hardwareTestError))
        {
            ConsoleUI::AddErrorLog(
                "MAVFTP hardware test rejected: " + hardwareTestError);
            return -6;
        }
    }
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

        if (probeOptions.enabled && !missionProfileProbe)
        {
            DroneSession* requested = droneManager.FindDrone(probeOptions.droneId);
            if (requested != nullptr && requested->GetFlightData().online)
            {
                std::string probeError;
                missionProfileProbe = CreateMissionProfileProbe(
                    droneManager, probeOptions, probeError);
                if (!missionProfileProbe)
                {
                    ConsoleUI::AddErrorLog(
                        "MissionProfileProbe rejected: " + probeError);
                    return -5;
                }
                ConsoleUI::AddLog(
                    "MissionProfileProbe started DroneID=" + probeOptions.droneId +
                    (probeOptions.allowWriteProbe
                        ? " WRITE OPT-IN ACTIVE"
                        : " READ-ONLY"));
                ConsoleUI::AddLog(
                    missionProfileProbe->GetReport().OpenFileRequestSafeSummary());
            }
            else if (now >= probeDiscoveryDeadline)
            {
                ConsoleUI::AddErrorLog(
                    "MissionProfileProbe failed: explicitly requested DroneID was not online before the bounded discovery deadline.");
                return -5;
            }
        }
        if (missionProfileProbe)
        {
            missionProfileProbe->Tick(now);
            if (missionProfileProbe->IsComplete())
            {
                const MissionProfileProbeReport& report =
                    missionProfileProbe->GetReport();
                ConsoleUI::AddTerminalLog(
                    report.ToMachineJson() + "\n" + report.ToHumanSummary());
                return report.succeeded ? 0 : -5;
            }
        }

        if (hardwareTest)
        {
            hardwareTest->Tick(now);
            if (hardwareTest->IsComplete())
            {
                const MAVFTPMissionHardwareTestReport& report =
                    hardwareTest->GetReport();
                ConsoleUI::AddTerminalLog(report.ToHumanSummary());
                return report.succeeded ? 0 : -6;
            }
        }

        if (appConfig.dspgcsCommandEnabled && !hardwareTestOptions.enabled)
        {
            commandReceiver.ProcessPending(
                commandManager,
                [&telemetryProvider](
                    const dspgcs::CommandResultBatchDTO& resultBatch)
                {
                    return telemetryProvider.SendCommandResult(resultBatch);
                });
        }

        if (telemetryProvider.IsRunning() && !hardwareTestOptions.enabled)
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

            uploadJobAuthority.Tick();
            uploadJobReceiver.ProcessPending(
                uploadJobAuthority,
                [&telemetryProvider](
                    const dspgcs::UploadJobCommandAckDTO& acknowledgement)
                {
                    return telemetryProvider.SendUploadJobCommandAck(
                        acknowledgement);
                },
                [&telemetryProvider](
                    const dspgcs::UploadJobSnapshotDTO& snapshot)
                {
                    return telemetryProvider.SendUploadJobSnapshot(snapshot);
                },
                [&telemetryProvider](
                    const dspgcs::UploadJobRouteStatePageDTO& page)
                {
                    return telemetryProvider.SendUploadJobRouteStatePage(page);
                },
                [&telemetryProvider](
                    const dspgcs::MavFtpRuntimeConfigAckDTO& acknowledgement)
                {
                    return telemetryProvider.SendMavFtpRuntimeConfigAck(
                        acknowledgement);
                },
                [&telemetryProvider](
                    const dspgcs::MavFtpRuntimeConfigSnapshotDTO& snapshot)
                {
                    return telemetryProvider.SendMavFtpRuntimeConfigSnapshot(snapshot);
                });
            routeUploadManager.Tick(now);
            for (const auto& delta : uploadJobAuthority.DrainRouteDeltas())
                telemetryProvider.SendUploadJobRouteStateDelta(delta);
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
