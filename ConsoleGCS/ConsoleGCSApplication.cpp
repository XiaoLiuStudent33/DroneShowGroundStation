#define WIN32_LEAN_AND_MEAN

#include "ConsoleGCSApplication.h"

#include "AppConfig.h"
#include "CommandManager.h"
#include "ConsoleUI.h"
#include "DroneManager.h"
#include "DSPGCSProductionCommandReceiver.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSTransportConfigLoader.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MavlinkUdpReceiver.h"
#include "SafetyConfig.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <conio.h>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>

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

    // Discovery/production mode enables real commands only when all three
    // explicit runtime gates below are enabled. Legacy mode keeps its existing
    // SAFE_TEST_MODE behavior.
    const bool mavlinkControlEnabled =
        (appConfig.mavlinkNetworkEnabled &&
         appConfig.dspgcsCommandEnabled &&
         appConfig.realCommandEnabled) ||
        (!appConfig.mavlinkNetworkEnabled && !SafetyConfig::SAFE_TEST_MODE);

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
    ConsoleUI::AddLog("ConnectedDroneCount=0");

    return appConfig.mavlinkNetworkEnabled
        ? RunReadOnlyMavlink(appConfig)
        : RunLegacySingleSession(appConfig);
}

int ConsoleGCSApplication::RunReadOnlyMavlink(
    const AppConfig& appConfig)
{
    if (appConfig.realCommandEnabled &&
        (appConfig.mavlinkBindIP != SafetyConfig::MAVLINK_LOCAL_IP ||
         appConfig.mavlinkBindPort != SafetyConfig::MAVLINK_LOCAL_PORT))
    {
        ConsoleUI::AddLog(
            "Startup blocked: real commands require shared MAVLink endpoint " +
            std::string(SafetyConfig::MAVLINK_LOCAL_IP) + ":" +
            std::to_string(SafetyConfig::MAVLINK_LOCAL_PORT) +
            ", configured=" + appConfig.mavlinkBindIP + ":" +
            std::to_string(appConfig.mavlinkBindPort));
        return -2;
    }

    MavlinkUdpReceiver receiver;
    std::string receiverError;
    if (!receiver.Start(
        appConfig.mavlinkBindIP,
        static_cast<std::uint16_t>(appConfig.mavlinkBindPort),
        receiverError))
    {
        ConsoleUI::AddLog(receiverError);
        return -2;
    }

    DroneManager droneManager;
    CommandManager commandManager(droneManager);
    DSPGCSProductionCommandReceiver commandReceiver;
    commandReceiver.SetRealCommandEnabled(appConfig.realCommandEnabled);
    DroneSessionConfig discoveredSessionDefaults;
    discoveredSessionDefaults.localIp = appConfig.mavlinkBindIP;
    // Discovery Sessions share the same bound endpoint for receive, heartbeat,
    // commands, and ACK routing. They never create an ephemeral control socket.
    discoveredSessionDefaults.localPort = receiver.GetBoundPort();
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
    std::size_t lastConnectedDroneCount =
        (std::numeric_limits<std::size_t>::max)();

    DSPGCSProductionTelemetryProvider telemetryProvider;
    if (appConfig.dspgcsCommandEnabled)
    {
        telemetryProvider.SetReceiveHandler(
            [&commandReceiver](dspgcs::TransportFrame frame)
            {
                commandReceiver.EnqueueFrame(std::move(frame));
            });
    }
    if (appConfig.dspgcsTelemetryEnabled || appConfig.dspgcsCommandEnabled)
    {
        dspgcs::DSPGCSTransportConfig transportConfig;
        std::string transportError;
        if (!LoadDSPGCSTransportConfig(
                appConfig.dspgcsTransportConfigPath,
                transportConfig,
                transportError) ||
            !telemetryProvider.Start(transportConfig, transportError))
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
        for (int datagramIndex = 0; datagramIndex < 256; ++datagramIndex)
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
                    " source=" + config.targetIp + ":" +
                    std::to_string(config.targetPort));
            }
        }

        if (_kbhit())
        {
            (void)_getch();
            ConsoleUI::AddLog(
                "Control key ignored: production commands must use DSP-GCS");
        }

        if (now - lastRefresh >= std::chrono::seconds(1))
        {
            const std::vector<DroneSession*> sessions =
                droneManager.GetAllDrones();
            const std::size_t connectedDroneCount =
                static_cast<std::size_t>(std::count_if(
                    sessions.begin(),
                    sessions.end(),
                    [](const DroneSession* session)
                    {
                        return session != nullptr &&
                            session->GetFlightData().online;
                    }));
            if (connectedDroneCount != lastConnectedDroneCount)
            {
                ConsoleUI::AddLog(
                    "ConnectedDroneCount=" +
                    std::to_string(connectedDroneCount));
                lastConnectedDroneCount = connectedDroneCount;
            }
            if (!sessions.empty())
            {
                DroneSession* displayedSession = sessions.front();
                ConsoleUI::Update(
                    displayedSession->GetDroneId(),
                    displayedSession->GetFlightData(),
                    displayedSession->GetHoverTest());
            }
            lastRefresh = now;
        }

        Sleep(10);
    }
}

int ConsoleGCSApplication::RunLegacySingleSession(
    const AppConfig& appConfig)
{
    ConsoleUI::AddLog("Legacy single-session mode");

    DroneManager droneManager;
    CommandManager commandManager(droneManager);
    DroneSessionConfig sessionConfig;
    sessionConfig.droneId = "DRONE-0001";
    sessionConfig.targetIp = appConfig.targetIP;
    sessionConfig.targetPort = appConfig.targetPort;
    sessionConfig.localIp = SafetyConfig::MAVLINK_LOCAL_IP;
    sessionConfig.localPort = appConfig.localPort;
    sessionConfig.heartbeatTimeoutMs = appConfig.heartbeatTimeoutMs;
    sessionConfig.realCommandEnabled = !SafetyConfig::SAFE_TEST_MODE;

    if (!droneManager.AddDrone(sessionConfig))
    {
        ConsoleUI::AddLog("Unable to add initial drone session");
        return -2;
    }

    DroneSession* activeSession =
        droneManager.FindDrone(sessionConfig.droneId);

    if (activeSession == nullptr ||
        !droneManager.StartDrone(sessionConfig.droneId))
    {
        ConsoleUI::AddLog("Unable to start initial drone session");
        return -3;
    }

    ConsoleUI::AddLog(
        "Active DroneID=" + activeSession->GetDroneId() +
        " SYSID=" + std::to_string(activeSession->GetSysId()) +
        " Session=" + DroneSessionStateName(activeSession->GetState()));

    auto lastRefresh = std::chrono::steady_clock::now();
    std::size_t lastConnectedDroneCount =
        (std::numeric_limits<std::size_t>::max)();

    while (true)
    {
        const auto now = std::chrono::steady_clock::now();
        droneManager.TickAll(now);
        commandManager.Tick();

        if (_kbhit())
        {
            commandManager.HandleConsoleKey(
                _getch(),
                activeSession->GetDroneId());
        }

        if (now - lastRefresh >= std::chrono::seconds(1))
        {
            const std::size_t connectedDroneCount =
                activeSession->GetFlightData().online ? 1u : 0u;
            if (connectedDroneCount != lastConnectedDroneCount)
            {
                ConsoleUI::AddLog(
                    "ConnectedDroneCount=" +
                    std::to_string(connectedDroneCount));
                lastConnectedDroneCount = connectedDroneCount;
            }
            ConsoleUI::Update(
                activeSession->GetDroneId(),
                activeSession->GetFlightData(),
                activeSession->GetHoverTest());
            lastRefresh = now;
        }

        Sleep(10);
    }
}
