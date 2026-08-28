#include "AppConfig.h"
#include "CommandManager.h"
#include "DroneManager.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "DSPGCSTransportConfigLoader.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MavlinkUdpReceiver.h"
#include "SafetyConfig.h"
#include "RouteArtifactReadLease.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C"
{
#include "ardupilotmega/mavlink.h"
}

int RunCommandTests();
int RunDSPCommandTests();
int RunDSPFrozenAssignmentProtocolTests();
int RunDSPFrozenAssignmentReceiverTests();
int RunDSPFrozenAssignmentE2EServer(int argc, char** argv);
int RunConsoleArtifactConsumerV2Tests();
int RunConsoleArtifactConsumerV2Scale10K();
int RunRouteArtifactV2ContractTests();
int RunRouteMissionCodecTests();
int RunMissionFtpCompilerTests();
int RunRouteMissionBenchmarks(int argc, char** argv);
int RunDSPUploadJobProtocolTests();
int RunMavFtpRuntimeConfigTests();
int RunMavFtpProbeTests();
int RunMAVFTPMissionWriterTests();
int RunMAVFTPWritePermitConcurrencyTests();
int RunRouteUploadConcurrencySchedulerTests();
int RunMAVFTPMissionHardwareTestTests();
int RunMissionUploadTests();
int RunDroneLogicalIdTests();
int RunConsoleRenderTests();

namespace
{
    void Expect(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    class TemporaryConfigFile
    {
    public:
        TemporaryConfigFile(
            const std::string& name,
            const std::string& contents)
        {
            static int counter = 0;
            path = std::filesystem::temp_directory_path() /
                ("DroneGCS-" + name + "-" +
                 std::to_string(++counter) + ".ini");
            std::ofstream output(path, std::ios::binary);
            Expect(output.good(), "unable to create temporary config file");
            output << contents;
            Expect(output.good(), "unable to write temporary config file");
        }

        ~TemporaryConfigFile()
        {
            std::error_code error;
            std::filesystem::remove(path, error);
        }

        std::string GetPath() const
        {
            return path.string();
        }

    private:
        std::filesystem::path path;
    };

    void VerifyProductionConfigurationLoading()
    {
        TemporaryConfigFile configured("production", R"(
safe_test_mode=true
mavlink_network_enabled=true
real_command_enabled=false
route_upload_enabled=true
mavlink_bind_ip=10.20.30.40
mavlink_bind_port=14550
heartbeat_timeout_ms=4500
dspgcs_telemetry_enabled=true
dspgcs_command_enabled=true
dspgcs_transport_config=config/DSPGCSTransport.ini
dspgcs_telemetry_interval_ms=500
 artifact_workspace_root=C:\DroneGCSArtifactWorkspace
 [performance]
mavlink_receive_buffer_bytes=8388608
max_packets_per_drain=8192
snapshot_datagram_bytes=60000
snapshot_items_per_chunk=256
 [mavftp]
 segment_items=4000
 request_timeout_ms=750
 terminate_timeout_ms=2500
 request_retry_count=3
write_window=10
global_write_outstanding_cap=123
)" );

        AppConfig config;
        std::string error;
        Expect(
            AppConfig::Load(configured.GetPath(), config, error),
            "production config failed to load: " + error);
        Expect(
            config.mavlinkBindIP == "10.20.30.40" &&
            config.mavlinkBindPort == 14550 &&
            config.heartbeatTimeoutMs == 4500,
            "MAVLink deployment values were not loaded");
        Expect(config.routeUploadEnabled && !config.realCommandEnabled,
            "Route upload gate was not independent from real command enablement");
        Expect(
            config.mavlinkReceiveBufferBytes == 8388608 &&
            config.maxPacketsPerDrain == 8192 &&
            config.snapshotDatagramBytes == 60000 &&
            config.snapshotItemsPerChunk == 256,
            "Production performance values were not loaded");
        Expect(
             config.mavftpSegmentItems == 4000U &&
             config.mavftpRequestTimeoutMs == 750 &&
             config.mavftpTerminateTimeoutMs == 2500 &&
             config.mavftpRequestRetryCount == 3U &&
            config.mavftpWriteWindow == 10U &&
            config.mavftpGlobalWriteOutstandingCap == 123U,
            "legal MAVFTP configuration values were not loaded");
        Expect(
            config.artifactWorkspaceRoot == "C:\\DroneGCSArtifactWorkspace",
            "Artifact Workspace root was not loaded");

        TemporaryConfigFile normalizedWorkspace("normalized-workspace", R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
artifact_workspace_root=C:\DroneGCSArtifactWorkspace\temporary\..\CurrentWorkspace
)" );
        AppConfig normalizedWorkspaceConfig;
        error.clear();
        Expect(
            AppConfig::Load(
                normalizedWorkspace.GetPath(), normalizedWorkspaceConfig, error),
            "normalized Artifact Workspace config failed to load: " + error);
        Expect(
            normalizedWorkspaceConfig.artifactWorkspaceRoot ==
                std::filesystem::u8path(
                    "C:\\DroneGCSArtifactWorkspace\\temporary\\..\\CurrentWorkspace")
                    .lexically_normal().u8string(),
            "Artifact Workspace root retained noncanonical dot segments");
        Expect(
            dronegcs::route_artifact::ArtifactReadLease::BuildMutexName(
                std::filesystem::u8path(
                    normalizedWorkspaceConfig.artifactWorkspaceRoot).wstring()) ==
            dronegcs::route_artifact::ArtifactReadLease::BuildMutexName(
                std::filesystem::u8path(
                    "C:\\DroneGCSArtifactWorkspace\\CurrentWorkspace").wstring()),
            "ArtifactReadLease did not receive the canonical Workspace identity");

        TemporaryConfigFile fallback("fallback", R"(
safe_test_mode=true
mavlink_network_enabled=true
real_command_enabled=false
mavlink_bind_ip=192.168.1.254
mavlink_bind_port=10001
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
)" );
        AppConfig fallbackConfig;
        error.clear();
        Expect(
            AppConfig::Load(fallback.GetPath(), fallbackConfig, error),
            "fallback config failed to load: " + error);
        Expect(
            fallbackConfig.mavlinkReceiveBufferBytes ==
                AppConfigDefaults::MavlinkReceiveBufferBytes &&
            fallbackConfig.maxPacketsPerDrain ==
                AppConfigDefaults::MaxPacketsPerDrain &&
            fallbackConfig.snapshotDatagramBytes ==
                AppConfigDefaults::SnapshotDatagramBytes &&
            fallbackConfig.snapshotItemsPerChunk ==
                AppConfigDefaults::SnapshotItemsPerChunk &&
            fallbackConfig.mavftpSegmentItems ==
                AppConfigDefaults::MavftpSegmentItems &&
             fallbackConfig.mavftpRequestTimeoutMs ==
                 AppConfigDefaults::MavftpRequestTimeoutMs &&
             fallbackConfig.mavftpTerminateTimeoutMs ==
                 AppConfigDefaults::MavftpTerminateTimeoutMs &&
            fallbackConfig.mavftpRequestRetryCount ==
                AppConfigDefaults::MavftpRequestRetryCount &&
            fallbackConfig.mavftpWriteWindow ==
                AppConfigDefaults::MavftpWriteWindow &&
            fallbackConfig.mavftpGlobalWriteOutstandingCap ==
                AppConfigDefaults::MavftpGlobalWriteOutstandingCap,
            "missing optional keys did not preserve code defaults");
        Expect(!fallbackConfig.routeUploadEnabled,
            "route_upload_enabled did not default fail-closed");

        const auto expectRemovedTransport = [](const std::string& value)
        {
            TemporaryConfigFile removedTransport(
                "removed-transport-" + value, std::string(R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
[route_upload]
transport=)") + value + "\n");
            AppConfig removedTransportConfig;
            std::string removedError;
            Expect(!AppConfig::Load(removedTransport.GetPath(),
                    removedTransportConfig, removedError) &&
                removedError.find("route_upload.transport was removed") !=
                    std::string::npos,
                "removed Route Upload transport key was accepted");
        };
        expectRemovedTransport("mavftp");

        const std::filesystem::path shippingConfig =
            std::filesystem::path(__FILE__).parent_path()
            .parent_path() / "gcs-config-upload.ini";
        AppConfig shipping;
        error.clear();
        Expect(AppConfig::Load(shippingConfig.string(), shipping, error) &&
            shipping.mavftpSegmentItems == 4000U &&
            shipping.mavftpWriteWindow == 16U &&
            shipping.mavftpRequestTimeoutMs == 750 &&
            shipping.mavftpTerminateTimeoutMs == 2500 &&
            shipping.mavftpRequestRetryCount == 3U &&
            shipping.mavftpGlobalWriteOutstandingCap == 16U,
            "shipping upload INI does not freeze the production MAVFTP profile");

        const auto expectInvalidMavftp = [](
            const std::string& name,
            const std::string& setting)
        {
            TemporaryConfigFile invalid(name, std::string(R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
[mavftp]
)") + setting + "\n");
            AppConfig invalidConfig;
            std::string invalidError;
            Expect(!AppConfig::Load(
                invalid.GetPath(), invalidConfig, invalidError),
                "invalid MAVFTP setting was accepted: " + setting);
            Expect(invalidError.find("mavftp.") != std::string::npos,
                "invalid MAVFTP setting produced an unrelated diagnostic");
        };
        expectInvalidMavftp("segment-zero", "segment_items=0");
        expectInvalidMavftp("segment-overflow", "segment_items=65536");
         expectInvalidMavftp("timeout-low", "request_timeout_ms=99");
         expectInvalidMavftp("timeout-high", "request_timeout_ms=60001");
         expectInvalidMavftp("terminate-timeout-zero", "terminate_timeout_ms=0");
         expectInvalidMavftp("terminate-timeout-negative", "terminate_timeout_ms=-1");
         expectInvalidMavftp("terminate-timeout-low", "terminate_timeout_ms=99");
         expectInvalidMavftp("terminate-timeout-high", "terminate_timeout_ms=60001");
        expectInvalidMavftp("retry-negative", "request_retry_count=-1");
        expectInvalidMavftp("retry-high", "request_retry_count=11");
        expectInvalidMavftp("window-zero", "write_window=0");
        expectInvalidMavftp("window-negative", "write_window=-1");
        expectInvalidMavftp("window-high", "write_window=17");
        expectInvalidMavftp("global-cap-zero", "global_write_outstanding_cap=0");
        expectInvalidMavftp("global-cap-negative", "global_write_outstanding_cap=-1");
        expectInvalidMavftp("global-cap-high", "global_write_outstanding_cap=801");

        for (const int cap : { 1, 16, 800 })
        {
            TemporaryConfigFile legalCap(
                "global-cap-" + std::to_string(cap), std::string(R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
[mavftp]
global_write_outstanding_cap=)") + std::to_string(cap) + "\n");
            AppConfig legalConfig;
            std::string legalError;
            Expect(AppConfig::Load(legalCap.GetPath(), legalConfig, legalError) &&
                legalConfig.mavftpGlobalWriteOutstandingCap ==
                    static_cast<std::uint32_t>(cap),
                "legal MAVFTP global Write cap was rejected");
        }

        AppConfig cliOverride = config;
        std::string cliError;
        Expect(ApplyMavFtpGlobalWriteOutstandingCapCliOverride(
                cliOverride, 800, cliError) &&
            cliOverride.mavftpGlobalWriteOutstandingCap == 800U,
            "CLI did not override INI global Write cap");
        Expect(!ApplyMavFtpGlobalWriteOutstandingCapCliOverride(
                cliOverride, 0, cliError) &&
            cliOverride.mavftpGlobalWriteOutstandingCap == 800U,
            "explicit CLI zero was accepted or changed the previous valid cap");

         for (const int window : { 1, 2, 4, 8, 10, 16 })
        {
            TemporaryConfigFile legalWindow(
                "window-" + std::to_string(window), std::string(R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
[mavftp]
write_window=)") + std::to_string(window) + "\n");
            AppConfig legalConfig;
            std::string legalError;
            Expect(AppConfig::Load(legalWindow.GetPath(), legalConfig, legalError) &&
                legalConfig.mavftpWriteWindow == static_cast<std::uint32_t>(window),
                "legal MAVFTP write window was rejected");
         }

         for (const int timeout : { 100, 2500, 60000 })
         {
             TemporaryConfigFile legalTerminateTimeout(
                 "terminate-timeout-" + std::to_string(timeout), std::string(R"(
 safe_test_mode=true
 mavlink_network_enabled=false
 real_command_enabled=false
 heartbeat_timeout_ms=3000
 dspgcs_telemetry_enabled=false
 dspgcs_command_enabled=false
 [mavftp]
 terminate_timeout_ms=)") + std::to_string(timeout) + "\n");
             AppConfig legalConfig;
             std::string legalError;
             Expect(AppConfig::Load(legalTerminateTimeout.GetPath(),
                 legalConfig, legalError) &&
                 legalConfig.mavftpTerminateTimeoutMs == timeout,
                 "legal MAVFTP terminate timeout was rejected");
         }

        TemporaryConfigFile invalidRouteGate("invalid-route-gate", R"(
safe_test_mode=true
mavlink_network_enabled=false
real_command_enabled=false
route_upload_enabled=true
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=true
dspgcs_command_enabled=true
)" );
        AppConfig invalidRouteGateConfig;
        error.clear();
        Expect(!AppConfig::Load(invalidRouteGate.GetPath(),
            invalidRouteGateConfig, error) &&
            error.find("route_upload_enabled") != std::string::npos,
            "Route upload gate did not require the MAVLink network");

        TemporaryConfigFile disabledNetwork("disabled-network", R"(
 safe_test_mode=true
 mavlink_network_enabled=false
 real_command_enabled=false
heartbeat_timeout_ms=3000
dspgcs_telemetry_enabled=false
dspgcs_command_enabled=false
 )" );
        AppConfig disabledNetworkConfig;
        error.clear();
        Expect(
            AppConfig::Load(
                disabledNetwork.GetPath(),
                disabledNetworkConfig,
                error),
            "disabled-network config unexpectedly required a fixed target: " +
                error);
        Expect(
            !disabledNetworkConfig.mavlinkNetworkEnabled,
            "disabled-network flag was not preserved");
        std::cout
            << "Production/fallback config loading passed; fixed target fields are not required\n";
    }

    void VerifyDSPTransportConfigurationLoading()
    {
        TemporaryConfigFile configured("dsp", R"(
TransportType=Loopback
ConsoleAddress=127.0.0.1
ConsolePort=13001
UnrealAddress=127.0.0.1
UnrealPort=13002
ReceiveBufferBytes=8388608
ReceiverPollMs=7
)" );
        dspgcs::DSPGCSTransportConfig config;
        std::string error;
        Expect(
            LoadDSPGCSTransportConfig(configured.GetPath(), config, error),
            "DSP role config failed to load: " + error);
        Expect(
            config.bindAddress == "127.0.0.1" &&
            config.bindPort == 13001 &&
            config.remoteAddress == "127.0.0.1" &&
            config.remotePort == 13002,
            "ConsoleEndpoint/UnrealEndpoint mapping changed");
        Expect(
            config.receiveBufferBytes == 8388608 &&
            config.receiverPollMs == 7,
            "DSP performance values were not loaded");

        TemporaryConfigFile fallback("dsp-fallback", R"(
TransportType=Loopback
ConsoleAddress=127.0.0.1
ConsolePort=12001
UnrealAddress=127.0.0.1
UnrealPort=12002
)" );
        error.clear();
        Expect(
            LoadDSPGCSTransportConfig(fallback.GetPath(), config, error),
            "DSP fallback config failed to load: " + error);
        Expect(
            config.receiveBufferBytes ==
                dspgcs::DSPGCSDefaults::SocketBufferBytes &&
            config.receiverPollMs ==
                dspgcs::DSPGCSDefaults::ConsoleReceiverPollMs,
            "DSP missing performance keys did not preserve fallbacks");

        TemporaryConfigFile compatibility("dsp-compatibility", R"(
TransportType=Loopback
BindAddress=127.0.0.1
BindPort=12001
RemoteAddress=127.0.0.1
RemotePort=12002
)" );
        error.clear();
        Expect(
            LoadDSPGCSTransportConfig(
                compatibility.GetPath(), config, error),
            "legacy DSP key compatibility failed: " + error);
        Expect(
            config.bindPort == 12001 && config.remotePort == 12002,
            "legacy DSP endpoint keys mapped incorrectly");
        std::cout << "DSP role endpoints and performance fallbacks passed\n";
    }

    void VerifySafetyUsesDeploymentConfig()
    {
        SafetyConfig::SetRealCommandEnabled(true);
        Expect(
            SafetyConfig::CanEnableRealCommands(
                true, true, "10.20.30.40", 14550),
            "Safety rejected an explicit configured deployment endpoint");
        Expect(
            !SafetyConfig::CanEnableRealCommands(
                true, true, "0.0.0.0", 14550),
            "Safety accepted a wildcard real-command bind");
        Expect(
            !SafetyConfig::CanEnableRealCommands(
                true, false, "10.20.30.40", 14550),
            "Safety ignored the DSP command gate");
        SafetyConfig::SetRealCommandEnabled(false);
        std::cout << "Safety policy uses deployment config without endpoint constants\n";
    }

    mavlink_message_t MakeHeartbeatMessage(
        const std::uint8_t systemId,
        const std::uint8_t componentId = MAV_COMP_ID_AUTOPILOT1,
        const std::uint32_t customMode = 5)
    {
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(
            systemId,
            componentId,
            &message,
            MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
            customMode,
            MAV_STATE_STANDBY);

        return message;
    }

    std::vector<std::uint8_t> MakeHeartbeatDatagram(
        const std::uint8_t systemId,
        const std::uint8_t componentId = MAV_COMP_ID_AUTOPILOT1,
        const std::uint32_t customMode = 5)
    {
        const mavlink_message_t message = MakeHeartbeatMessage(
            systemId,
            componentId,
            customMode);

        std::vector<std::uint8_t> bytes(MAVLINK_MAX_PACKET_LEN);
        const std::uint16_t byteCount =
            mavlink_msg_to_send_buffer(bytes.data(), &message);
        bytes.resize(byteCount);
        return bytes;
    }

    std::vector<std::uint8_t> SerializeMessage(
        const mavlink_message_t& message)
    {
        std::vector<std::uint8_t> bytes(MAVLINK_MAX_PACKET_LEN);
        const std::uint16_t byteCount =
            mavlink_msg_to_send_buffer(bytes.data(), &message);
        bytes.resize(byteCount);
        return bytes;
    }

    MavlinkIngressStats SubmitMessage(
        MavlinkReadOnlyIngress& ingress,
        const mavlink_message_t& message,
        const std::string& sourceIp,
        const std::uint16_t sourcePort = 10001
    )
    {
        const std::vector<std::uint8_t> bytes = SerializeMessage(message);
        return ingress.SubmitDatagram(
            bytes.data(), bytes.size(), sourceIp, sourcePort);
    }

    MavlinkDiscoveryResult SendHeartbeat(
        MavlinkDiscoveryService& discovery,
        const std::uint8_t systemId,
        const std::string& sourceIp,
        const std::uint16_t sourcePort = 10001,
        const std::uint32_t customMode = 5)
    {
        return discovery.IngestMessage(
            MakeHeartbeatMessage(
                systemId,
                MAV_COMP_ID_AUTOPILOT1,
                customMode),
            sourceIp,
            sourcePort);
    }

    void ExpectOnline(
        const DroneManager& manager,
        const std::string& droneId,
        const bool expectedOnline)
    {
        const DroneSession* session = manager.FindDrone(droneId);
        Expect(session != nullptr, droneId + " session missing");
        Expect(
            session->GetFlightData().online == expectedOnline,
            droneId + " online state mismatch");
    }

    void VerifyUdpReceiverLoopback()
    {
        MavlinkUdpReceiver receiver;
        std::string error;
        Expect(
            receiver.Start(
                "127.0.0.1",
                0,
                AppConfigDefaults::MavlinkReceiveBufferBytes,
                error),
            "loopback receiver failed to start: " + error);

        const SOCKET sender = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        Expect(sender != INVALID_SOCKET, "loopback sender socket failed");

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(receiver.GetBoundPort());
        Expect(
            inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1,
            "loopback destination address failed");

        const std::vector<std::uint8_t> heartbeat = MakeHeartbeatDatagram(42);
        const int sent = sendto(
            sender,
            reinterpret_cast<const char*>(heartbeat.data()),
            static_cast<int>(heartbeat.size()),
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        Expect(sent == static_cast<int>(heartbeat.size()), "loopback send failed");

        MavlinkReceivedDatagram received;
        MavlinkUdpReceiveResult result = MavlinkUdpReceiveResult::WouldBlock;
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            result = receiver.Receive(received, error);
            if (result != MavlinkUdpReceiveResult::WouldBlock)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        Expect(result == MavlinkUdpReceiveResult::Received, "loopback receive failed");
        Expect(received.sourceIp == "127.0.0.1", "source IP extraction failed");
        Expect(received.sourcePort != 0, "source port extraction failed");
        Expect(received.bytes == heartbeat, "received datagram bytes changed");

        constexpr int BurstDatagrams = 4096;
        for (int index = 0; index < BurstDatagrams; ++index)
        {
            const int burstSent = sendto(
                sender,
                reinterpret_cast<const char*>(heartbeat.data()),
                static_cast<int>(heartbeat.size()),
                0,
                reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination));
            Expect(
                burstSent == static_cast<int>(heartbeat.size()),
                "loopback burst send failed");
        }

        int burstReceived = 0;
        for (int attempt = 0;
             attempt < 10000 && burstReceived < BurstDatagrams;
             ++attempt)
        {
            result = receiver.Receive(received, error);
            if (result == MavlinkUdpReceiveResult::WouldBlock)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            Expect(result == MavlinkUdpReceiveResult::Received, error);
            ++burstReceived;
        }
        closesocket(sender);
        Expect(
            burstReceived == BurstDatagrams,
            "MAVLink UDP receive buffer dropped synchronized burst traffic");
        std::cout
            << "UDP receiver endpoint extraction and 4096-datagram burst passed\n";
    }

    class SimulatedDroneEndpoint
    {
    public:
        explicit SimulatedDroneEndpoint(const std::uint8_t systemIdValue)
            : systemId(systemIdValue)
        {
            handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            Expect(handle != INVALID_SOCKET, "simulated drone socket failed");

            sockaddr_in localAddress{};
            localAddress.sin_family = AF_INET;
            localAddress.sin_port = 0;
            Expect(
                inet_pton(AF_INET, "127.0.0.1", &localAddress.sin_addr) == 1,
                "simulated drone local address failed");
            Expect(
                bind(
                    handle,
                    reinterpret_cast<const sockaddr*>(&localAddress),
                    sizeof(localAddress)) != SOCKET_ERROR,
                "simulated drone bind failed");

            int addressLength = sizeof(localAddress);
            Expect(
                getsockname(
                    handle,
                    reinterpret_cast<sockaddr*>(&localAddress),
                    &addressLength) != SOCKET_ERROR,
                "simulated drone getsockname failed");
            port = ntohs(localAddress.sin_port);

            u_long nonBlocking = 1;
            Expect(
                ioctlsocket(handle, FIONBIO, &nonBlocking) != SOCKET_ERROR,
                "simulated drone non-blocking setup failed");
        }

        ~SimulatedDroneEndpoint()
        {
            if (handle != INVALID_SOCKET)
            {
                closesocket(handle);
            }
        }

        SimulatedDroneEndpoint(const SimulatedDroneEndpoint&) = delete;
        SimulatedDroneEndpoint& operator=(const SimulatedDroneEndpoint&) = delete;

        void SendMessage(
            const mavlink_message_t& message,
            const std::uint16_t destinationPort) const
        {
            const std::vector<std::uint8_t> bytes = SerializeMessage(message);
            sockaddr_in destination{};
            destination.sin_family = AF_INET;
            destination.sin_port = htons(destinationPort);
            Expect(
                inet_pton(AF_INET, "127.0.0.1", &destination.sin_addr) == 1,
                "simulated drone destination address failed");
            const int sent = sendto(
                handle,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()),
                0,
                reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination));
            Expect(sent == static_cast<int>(bytes.size()), "simulated drone send failed");
        }

        void SendHeartbeat(const std::uint16_t destinationPort) const
        {
            mavlink_message_t heartbeat{};
            mavlink_msg_heartbeat_pack(
                systemId,
                MAV_COMP_ID_AUTOPILOT1,
                &heartbeat,
                MAV_TYPE_QUADROTOR,
                MAV_AUTOPILOT_ARDUPILOTMEGA,
                MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
                5,
                MAV_STATE_STANDBY);
            SendMessage(heartbeat, destinationPort);
        }

        bool WaitForMessage(
            const std::uint32_t expectedMessageId,
            std::uint16_t& sourcePort) const
        {
            for (int attempt = 0; attempt < 200; ++attempt)
            {
                std::array<std::uint8_t, 2048> bytes{};
                sockaddr_in source{};
                int sourceLength = sizeof(source);
                const int received = recvfrom(
                    handle,
                    reinterpret_cast<char*>(bytes.data()),
                    static_cast<int>(bytes.size()),
                    0,
                    reinterpret_cast<sockaddr*>(&source),
                    &sourceLength);
                if (received == SOCKET_ERROR)
                {
                    const int error = WSAGetLastError();
                    if (error == WSAEWOULDBLOCK || error == WSAECONNRESET)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        continue;
                    }
                    return false;
                }

                mavlink_message_t messageBuffer{};
                mavlink_status_t parseStatus{};
                for (int index = 0; index < received; ++index)
                {
                    mavlink_message_t decoded{};
                    mavlink_status_t decodedStatus{};
                    if (mavlink_frame_char_buffer(
                        &messageBuffer,
                        &parseStatus,
                        bytes[index],
                        &decoded,
                        &decodedStatus) == MAVLINK_FRAMING_OK &&
                        decoded.msgid == expectedMessageId)
                    {
                        sourcePort = ntohs(source.sin_port);
                        return true;
                    }
                }
            }
            return false;
        }

        std::uint16_t GetPort() const
        {
            return port;
        }

        std::uint8_t GetSystemId() const
        {
            return systemId;
        }

    private:
        SOCKET handle = INVALID_SOCKET;
        std::uint16_t port = 0;
        std::uint8_t systemId = 0;
    };

    MavlinkIngressStats PumpSharedEndpoint(
        MavlinkUdpReceiver& endpoint,
        MavlinkReadOnlyIngress& ingress,
        const int expectedDatagrams)
    {
        MavlinkIngressStats combined;
        int receivedCount = 0;
        for (int attempt = 0; attempt < 500 && receivedCount < expectedDatagrams; ++attempt)
        {
            MavlinkReceivedDatagram datagram;
            std::string error;
            const MavlinkUdpReceiveResult result = endpoint.Receive(datagram, error);
            if (result == MavlinkUdpReceiveResult::WouldBlock)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            Expect(result == MavlinkUdpReceiveResult::Received, error);

            const MavlinkIngressStats stats = ingress.SubmitDatagram(
                datagram.bytes.data(),
                datagram.bytes.size(),
                datagram.sourceIp,
                datagram.sourcePort);
            combined.decodedMessages += stats.decodedMessages;
            combined.heartbeatsApplied += stats.heartbeatsApplied;
            combined.telemetryApplied += stats.telemetryApplied;
            combined.commandMessagesApplied += stats.commandMessagesApplied;
            combined.identityConflicts += stats.identityConflicts;
            combined.ignoredMessages += stats.ignoredMessages;
            ++receivedCount;
        }

        Expect(receivedCount == expectedDatagrams, "shared endpoint datagram timeout");
        return combined;
    }

    void VerifySharedMavlinkEndpoint()
    {
        constexpr std::uint16_t sharedPort = 10001;
        MavlinkUdpReceiver endpoint;
        std::string error;
        Expect(
            endpoint.Start(
                "127.0.0.1",
                sharedPort,
                AppConfigDefaults::MavlinkReceiveBufferBytes,
                error),
            "shared endpoint failed to bind 127.0.0.1:10001: " + error);

        DroneSessionConfig sessionDefaults;
        sessionDefaults.heartbeatTimeoutMs = 5000;
        sessionDefaults.receiveOnly = false;
        sessionDefaults.realCommandEnabled = true;
        sessionDefaults.sharedMavlinkEndpoint = &endpoint;

        DroneManager manager;
        MavlinkDiscoveryService discovery(manager, sessionDefaults);
        MavlinkReadOnlyIngress ingress(manager, discovery);
        // All three simulated APMs intentionally use the same MAVLink IDs.
        // Their different UDP source endpoints are their unique identities.
        SimulatedDroneEndpoint drone1(1);
        SimulatedDroneEndpoint drone2(1);
        SimulatedDroneEndpoint drone3(1);
        SimulatedDroneEndpoint* drones[] = { &drone1, &drone2, &drone3 };

        for (const SimulatedDroneEndpoint* drone : drones)
        {
            drone->SendHeartbeat(sharedPort);
        }
        const MavlinkIngressStats heartbeatStats =
            PumpSharedEndpoint(endpoint, ingress, 3);
        Expect(
            heartbeatStats.heartbeatsApplied == 3 && manager.GetDroneCount() == 3,
            "shared endpoint did not register three heartbeats");
        const DroneSession* sharedDrone1 = manager.FindDrone("Drone001");
        const DroneSession* sharedDrone2 = manager.FindDrone("Drone002");
        const DroneSession* sharedDrone3 = manager.FindDrone("Drone003");
        Expect(
            sharedDrone1 != nullptr && sharedDrone2 != nullptr &&
                sharedDrone3 != nullptr,
            "shared endpoint sessions were not assigned stable DroneIDs");
        Expect(
            drone1.GetPort() != drone2.GetPort() &&
                drone1.GetPort() != drone3.GetPort() &&
                drone2.GetPort() != drone3.GetPort() &&
                sharedDrone1->GetConfig().remoteIp == "127.0.0.1" &&
                sharedDrone2->GetConfig().remoteIp == "127.0.0.1" &&
                sharedDrone3->GetConfig().remoteIp == "127.0.0.1" &&
                sharedDrone1->GetConfig().remotePort == drone1.GetPort() &&
                sharedDrone2->GetConfig().remotePort == drone2.GetPort() &&
                sharedDrone3->GetConfig().remotePort == drone3.GetPort(),
            "same IP/different source port/same SYSID did not create three Endpoint Sessions");

        for (int droneIndex = 0; droneIndex < 3; ++droneIndex)
        {
            const SimulatedDroneEndpoint* drone = drones[droneIndex];
            mavlink_message_t status{};
            mavlink_msg_sys_status_pack(
                drone->GetSystemId(),
                MAV_COMP_ID_AUTOPILOT1,
                &status,
                0,
                0,
                0,
                0,
                14800,
                100,
                static_cast<std::int8_t>(81 + droneIndex),
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0);
            drone->SendMessage(status, sharedPort);
        }
        const MavlinkIngressStats telemetryStats =
            PumpSharedEndpoint(endpoint, ingress, 3);
        Expect(telemetryStats.telemetryApplied == 3, "three-way telemetry routing failed");
        Expect(
            manager.FindDrone("Drone001")->GetFlightData().batteryRemaining == 81 &&
            manager.FindDrone("Drone002")->GetFlightData().batteryRemaining == 82 &&
            manager.FindDrone("Drone003")->GetFlightData().batteryRemaining == 83,
            "telemetry reached the wrong Session");

        CommandManager commandManager(manager);
        DroneCommandRequest request;
        request.commandId = 6001;
        request.commandType = DroneCommandType::Arm;
        for (const DroneSession* session : manager.GetAllDrones())
        {
            request.targetDroneIds.push_back(session->GetDroneId());
        }
        request.source = "EndpointCommandRoutingTest";
        Expect(
            commandManager.Submit(request),
            "multi-target ARM tasks were not queued");
        manager.TickAll(std::chrono::steady_clock::now() + std::chrono::seconds(2));
        for (const SimulatedDroneEndpoint* drone : drones)
        {
            std::uint16_t heartbeatSourcePort = 0;
            Expect(
                drone->WaitForMessage(
                    MAVLINK_MSG_ID_HEARTBEAT,
                    heartbeatSourcePort),
                "simulated drone did not receive GCS heartbeat");
            Expect(
                heartbeatSourcePort == sharedPort,
                "GCS heartbeat source port was not 10001");

            std::uint16_t commandSourcePort = 0;
            Expect(
                drone->WaitForMessage(
                    MAVLINK_MSG_ID_COMMAND_LONG,
                    commandSourcePort),
                "simulated drone did not receive COMMAND_LONG");
            Expect(
                commandSourcePort == sharedPort,
                "COMMAND_LONG source port was not 10001");

            mavlink_message_t ack{};
            mavlink_msg_command_ack_pack(
                drone->GetSystemId(),
                MAV_COMP_ID_AUTOPILOT1,
                &ack,
                MAV_CMD_COMPONENT_ARM_DISARM,
                MAV_RESULT_ACCEPTED,
                100,
                0,
                255,
                MAV_COMP_ID_MISSIONPLANNER);
            drone->SendMessage(ack, sharedPort);
        }

        const MavlinkIngressStats ackStats = PumpSharedEndpoint(endpoint, ingress, 3);
        Expect(ackStats.commandMessagesApplied == 3, "three-way ACK routing failed");
        manager.TickAll(
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2100));
        commandManager.Tick();
        const std::vector<ManagedDroneCommand> completed =
            commandManager.ConsumeCompletedCommands();
        Expect(completed.size() == 1, "multi-target batch did not complete once");
        Expect(
            completed.front().targetResults.size() == request.targetDroneIds.size(),
            "multi-target result count did not match TargetDroneIDs");
        for (std::size_t index = 0; index < completed.front().targetResults.size(); ++index)
        {
            const std::string& droneId = request.targetDroneIds[index];
            Expect(
                completed.front().targetResults[index].droneId == droneId &&
                completed.front().targetResults[index].state ==
                    DroneCommandTargetState::Accepted &&
                manager.FindDrone(droneId)->GetAckState().valid,
                droneId + " ACK was not routed to its independent task");
        }

        std::cout
            << "Shared endpoint PASS: same IP/different source port/same SYSID; "
            << "three registrations, telemetry routes, ACK routes; "
            << "one multi-target ARM expanded to three Sessions; "
            << "GCS heartbeat and COMMAND_LONG source port=10001\n";
    }
}

int RunConsolePipelineTests()
{
    try
    {
        if (!SafetyConfig::SAFE_TEST_MODE ||
            SafetyConfig::REAL_COMMAND_ENABLED)
        {
            throw std::runtime_error(
                "Console pipeline tests require SAFE_TEST_MODE and real commands disabled");
        }

        VerifyProductionConfigurationLoading();
        VerifyDSPTransportConfigurationLoading();
        VerifySafetyUsesDeploymentConfig();
        VerifyUdpReceiverLoopback();
        VerifySharedMavlinkEndpoint();

        DroneSessionConfig sessionDefaults;
        sessionDefaults.heartbeatTimeoutMs = 40;
        sessionDefaults.receiveOnly = true;

        DroneManager manager;
        MavlinkDiscoveryService discovery(manager, sessionDefaults);
        MavlinkReadOnlyIngress ingress(manager, discovery);

        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.145") ==
                MavlinkDiscoveryResult::Registered,
            "SYSID=1 was not registered");
        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.231") ==
                MavlinkDiscoveryResult::Registered,
            "second SYSID=1 endpoint was not registered");
        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.232") ==
                MavlinkDiscoveryResult::Registered,
            "third SYSID=1 endpoint was not registered");

        const std::vector<const DroneSession*> initialSessions =
            static_cast<const DroneManager&>(manager).GetAllDrones();
        Expect(initialSessions.size() == 3, "GetAllDrones did not return 3");

        DroneSession* drone001 = manager.FindDrone("Drone001");
        DroneSession* drone002 = manager.FindDrone("Drone002");
        DroneSession* drone003 = manager.FindDrone("Drone003");
        Expect(
            drone001 != nullptr && drone002 != nullptr && drone003 != nullptr,
            "endpoint to sequential DroneID mapping failed");
        Expect(
            drone001 != drone002 && drone001 != drone003 && drone002 != drone003,
            "discovered drones do not have isolated sessions");
        Expect(
            drone001->GetConfig().discoveredSystemId == 1 &&
            drone002->GetConfig().discoveredSystemId == 1 &&
            drone003->GetConfig().discoveredSystemId == 1 &&
            drone001->GetConfig().remoteIp == "192.168.1.145" &&
            drone002->GetConfig().remoteIp == "192.168.1.231" &&
            drone003->GetConfig().remoteIp == "192.168.1.232",
            "same SYSID endpoints were not preserved as independent Sessions");
        ExpectOnline(manager, "Drone001", true);
        ExpectOnline(manager, "Drone002", true);
        ExpectOnline(manager, "Drone003", true);
        std::cout << "Drone001 online\nDrone002 online\nDrone003 online\n";

        mavlink_message_t gps{};
        mavlink_msg_gps_raw_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &gps,
            1000000,
            3,
            319327550,
            1186382469,
            123450,
            100,
            100,
            100,
            9000,
            14,
            0,
            0,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, gps, "192.168.1.145")
                .telemetryApplied == 1,
            "GPS_RAW_INT was not routed to Drone001");

        FlightData telemetry = drone001->GetFlightData();
        Expect(telemetry.gpsValid, "GPS_RAW_INT valid Fix was rejected");
        Expect(
            telemetry.latitude > 31.9327549 &&
                telemetry.latitude < 31.9327551,
            "GPS_RAW_INT latitude conversion failed");
        Expect(
            telemetry.longitude > 118.6382468 &&
                telemetry.longitude < 118.6382470,
            "GPS_RAW_INT longitude conversion failed");
        Expect(
            telemetry.altitude > 123.4f && telemetry.altitude < 123.5f &&
                telemetry.altitudeValid,
            "GPS_RAW_INT AMSL altitude conversion failed");
		const std::int64_t firstGpsReceiveUtcMs = telemetry.lastGpsReceiveUtcMs;
		const std::uint64_t firstGpsSampleSequence = telemetry.gpsSampleSequence;
		Expect(
			firstGpsReceiveUtcMs > 0 && firstGpsSampleSequence == 1 &&
			telemetry.lastGpsReceiveMonotonic !=
				std::chrono::steady_clock::time_point{},
			"GPS_RAW_INT did not establish true receive-time/sample identity");

        mavlink_message_t position{};
        mavlink_msg_global_position_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &position,
            1000,
            0,
            0,
            0,
            5000,
            300,
            400,
            0,
            9000);
        Expect(
            SubmitMessage(ingress, position, "192.168.1.145")
                .telemetryApplied == 1,
            "GLOBAL_POSITION_INT was not routed to Drone001");

        telemetry = drone001->GetFlightData();
        Expect(
            telemetry.latitude > 31.9327549 &&
                telemetry.latitude < 31.9327551 &&
                telemetry.longitude > 118.6382468 &&
                telemetry.longitude < 118.6382470,
            "GLOBAL_POSITION_INT zero coordinates overwrote GPS_RAW_INT");
        Expect(
            telemetry.altitude > 123.4f && telemetry.altitude < 123.5f &&
                telemetry.relativeAltitude > 4.9f &&
                telemetry.relativeAltitude < 5.1f &&
                telemetry.groundSpeed > 4.9f && telemetry.groundSpeed < 5.1f,
            "GLOBAL_POSITION_INT overwrote #24 AMSL or failed relative altitude/ground speed");
		Expect(
			telemetry.lastGpsReceiveUtcMs == firstGpsReceiveUtcMs &&
			telemetry.gpsSampleSequence == firstGpsSampleSequence,
			"non-GPS telemetry changed GPS sample receive identity");

        mavlink_message_t invalidGps{};
        mavlink_msg_gps_raw_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &invalidGps,
            2000000,
            1,
            0,
            0,
            0,
            100,
            100,
            0,
            0,
            5,
            0,
            0,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, invalidGps, "192.168.1.145")
                .telemetryApplied == 1,
            "invalid GPS_RAW_INT was not routed to Drone001");
        telemetry = drone001->GetFlightData();
        Expect(!telemetry.gpsValid, "GPS_RAW_INT fix_type=1 remained valid");
		Expect(
			telemetry.gpsFixType == 1 && telemetry.satellitesVisible == 5 &&
			telemetry.gpsSampleSequence == firstGpsSampleSequence + 1 &&
			telemetry.lastGpsReceiveUtcMs >= firstGpsReceiveUtcMs,
			"fix downgrade did not update raw GPS fields and sample identity");
        Expect(
            manager.FindDrone("Drone001") == drone001 &&
                manager.GetDroneCount() == 3,
            "invalid GPS_RAW_INT removed or replaced Drone001 session");

        Expect(
            SubmitMessage(ingress, gps, "192.168.1.145")
                .telemetryApplied == 1,
            "valid GPS_RAW_INT recovery was not routed to Drone001");

        mavlink_message_t positionBeforeGps{};
        mavlink_msg_global_position_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &positionBeforeGps,
            3000,
            111111111,
            222222222,
            100000,
            2000,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, positionBeforeGps, "192.168.1.231")
                .telemetryApplied == 1,
            "GLOBAL_POSITION_INT was not routed to Drone002");
        Expect(
            drone002->GetFlightData().latitude == 0.0 &&
                drone002->GetFlightData().longitude == 0.0,
            "GLOBAL_POSITION_INT unexpectedly became the primary GPS source");

        mavlink_message_t gpsAfterPosition{};
        mavlink_msg_gps_raw_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &gpsAfterPosition,
            3000000,
            3,
            319327650,
            1186382569,
            100000,
            100,
            100,
            0,
            0,
            12,
            0,
            0,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, gpsAfterPosition, "192.168.1.231")
                .telemetryApplied == 1,
            "GPS_RAW_INT was not routed to Drone002 after GLOBAL_POSITION_INT");
        Expect(
            drone002->GetFlightData().latitude > 31.9327649 &&
                drone002->GetFlightData().latitude < 31.9327651 &&
                drone002->GetFlightData().longitude > 118.6382568 &&
                drone002->GetFlightData().longitude < 118.6382570,
            "GPS_RAW_INT did not win after GLOBAL_POSITION_INT");
        Expect(
            drone002->GetFlightData().altitude > 99.9f &&
                drone002->GetFlightData().altitude < 100.1f,
            "GPS_RAW_INT AMSL did not win after GLOBAL_POSITION_INT");

        mavlink_message_t seaLevelGps{};
        mavlink_msg_gps_raw_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &seaLevelGps,
            4000000,
            3,
            319327650,
            1186382569,
            0,
            100,
            100,
            0,
            0,
            12,
            0,
            0,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, seaLevelGps, "192.168.1.231")
                .telemetryApplied == 1,
            "sea-level GPS_RAW_INT was not routed to Drone002");
        Expect(
            drone002->GetFlightData().gpsValid &&
                drone002->GetFlightData().altitudeValid &&
                drone002->GetFlightData().altitude == 0.0f,
            "valid GPS_RAW_INT alt=0 was treated as invalid");

        mavlink_message_t battery{};
        mavlink_msg_sys_status_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &battery,
            0,
            0,
            0,
            0,
            14800,
            100,
            87,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0,
            0);
        Expect(
            SubmitMessage(ingress, battery, "192.168.1.145")
                .telemetryApplied == 1,
            "SYS_STATUS was not routed to Drone001");

        telemetry = drone001->GetFlightData();
        Expect(telemetry.gpsValid, "Drone001 GPS did not become valid");
        Expect(telemetry.altitudeValid, "Drone001 altitude did not become valid");
        Expect(
            telemetry.latitude > 31.9327549 && telemetry.latitude < 31.9327551,
            "Drone001 latitude conversion failed");
        Expect(
            telemetry.longitude > 118.6382468 && telemetry.longitude < 118.6382470,
            "Drone001 longitude conversion failed");
        Expect(
            telemetry.altitude > 123.4f && telemetry.altitude < 123.5f,
            "Drone001 altitude conversion failed");
        Expect(telemetry.batteryRemaining == 87, "Drone001 battery failed");
        Expect(telemetry.modeName == "LOITER", "Drone001 mode failed");
        std::cout << "Drone001 telemetry GPS/Altitude/Battery/Mode applied\n";

        struct ExpectedHeartbeatMode
        {
            std::uint32_t customMode;
            const char* name;
        };
        const ExpectedHeartbeatMode expectedHeartbeatModes[] =
        {
            { 0, "STABILIZE" },
            { 3, "AUTO" },
            { 4, "GUIDED" }
        };
        for (const ExpectedHeartbeatMode& expected : expectedHeartbeatModes)
        {
            Expect(
                SendHeartbeat(
                    discovery,
                    1,
                    "192.168.1.145",
                    10001,
                    expected.customMode) == MavlinkDiscoveryResult::Refreshed,
                std::string(expected.name) + " heartbeat was not routed");
            const FlightData modeTelemetry = drone001->GetFlightData();
            Expect(
                modeTelemetry.mode == static_cast<int>(expected.customMode) &&
                modeTelemetry.modeName == expected.name,
                std::string(expected.name) + " heartbeat mode was not applied");
            std::cout
                << "HEARTBEAT MODE " << expected.name
                << " custom_mode=" << expected.customMode
                << " applied to Drone001\n";
        }

        dspgcs::DSPGCSTransportConfig receiverConfig;
        receiverConfig.type = dspgcs::TransportType::Loopback;
        receiverConfig.bindAddress = "127.0.0.1";
        receiverConfig.bindPort = 12522;
        receiverConfig.remoteAddress = "127.0.0.1";
        receiverConfig.remotePort = 12521;
        std::string transportError;
        auto dtoReceiver = dspgcs::DSPGCSTransportFactory::CreateTransport(
            receiverConfig,
            transportError);
        Expect(dtoReceiver != nullptr, "DTO receiver factory failed: " + transportError);

        std::mutex receivedFrameMutex;
        dspgcs::TransportFrame receivedFrame;
        dtoReceiver->Receive([&](dspgcs::TransportFrame frame)
        {
            std::lock_guard<std::mutex> lock(receivedFrameMutex);
            receivedFrame = std::move(frame);
        });
        Expect(dtoReceiver->Start(), "DTO receiver transport failed to start");

        dspgcs::DSPGCSTransportConfig publisherConfig = receiverConfig;
        publisherConfig.bindPort = 12521;
        publisherConfig.remotePort = 12522;
        DSPGCSProductionTelemetryProvider productionProvider;
        Expect(
            productionProvider.Start(
                publisherConfig,
                AppConfigDefaults::SnapshotDatagramBytes,
                AppConfigDefaults::SnapshotItemsPerChunk,
                transportError),
            "production telemetry provider failed: " + transportError);
        Expect(
            productionProvider.PublishSnapshot(manager),
            "production telemetry snapshot send failed");

        for (int attempt = 0; attempt < 200; ++attempt)
        {
            {
                std::lock_guard<std::mutex> lock(receivedFrameMutex);
                if (!receivedFrame.bytes.empty())
                {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        dspgcs::TransportFrame frameToDecode;
        {
            std::lock_guard<std::mutex> lock(receivedFrameMutex);
            frameToDecode = receivedFrame;
        }
        productionProvider.Stop();
        dtoReceiver->Stop();
        Expect(!frameToDecode.bytes.empty(), "production telemetry frame not received");

        dspgcs::MessageEnvelope envelope;
        std::vector<std::uint8_t> payload;
        dspgcs::TelemetryBatchDTO batch;
        Expect(
            dspgcs::DecodeEnvelope(frameToDecode.bytes, envelope, payload) &&
                envelope.messageType == dspgcs::MessageType::TelemetryBatch &&
                dspgcs::DecodeTelemetry(payload, batch),
            "production TelemetryDTO frame decode failed");
        Expect(batch.items.size() == 3, "production DTO batch did not contain 3 drones");
        Expect(
            batch.items.front().droneId == "Drone001" &&
                batch.items.front().sysId == 1 &&
				batch.items.front().sampleTimestampUtcMs ==
					drone001->GetFlightData().lastGpsReceiveUtcMs &&
                batch.items.front().connection.online &&
                batch.items.front().gps.valid &&
                batch.items.front().battery.remainingPercent == 87.0f,
            "production Drone001 TelemetryDTO values failed");
        Expect(
            batch.items[1].droneId == "Drone002" &&
                batch.items[1].sysId == 1 &&
                batch.items[2].droneId == "Drone003" &&
                batch.items[2].sysId == 1,
            "DSP-GCS display IDs were not independent from SYSID");
        std::cout << "Production TelemetryDTO -> DSP-GCS Transport passed for 3 drones\n";

        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.145") ==
                MavlinkDiscoveryResult::Refreshed,
            "same endpoint did not refresh its session");
        Expect(
            manager.FindDrone("Drone001") == drone001 &&
                manager.GetDroneCount() == 3,
            "refresh created a duplicate Drone001 session");

        std::cout << "Same SYSID on three endpoints registered without conflict\n";

        std::this_thread::sleep_for(std::chrono::milliseconds(55));
        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.145") ==
                MavlinkDiscoveryResult::Refreshed,
            "Drone001 refresh before timeout evaluation failed");
        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.232") ==
                MavlinkDiscoveryResult::Refreshed,
            "Drone003 refresh before timeout evaluation failed");
        discovery.Update(std::chrono::steady_clock::now());

        ExpectOnline(manager, "Drone001", true);
        ExpectOnline(manager, "Drone002", false);
        ExpectOnline(manager, "Drone003", true);
        Expect(
            drone002->GetState() == DroneSessionState::HeartbeatTimeout,
            "Drone002 did not enter HeartbeatTimeout");
        std::cout << "Drone002 offline; Drone001/Drone003 remain online\n";

        Expect(
            SendHeartbeat(discovery, 1, "192.168.1.231") ==
                MavlinkDiscoveryResult::Refreshed,
            "Drone002 recovery heartbeat did not refresh its session");
        ExpectOnline(manager, "Drone002", true);
        Expect(
            manager.FindDrone("Drone002") == drone002 &&
                manager.GetDroneCount() == 3,
            "Drone002 recovery created a duplicate session");
        std::cout << "Drone002 online again using original session\n";

        // Scale regression: a 1000-aircraft snapshot must not be emitted as
        // one oversized UDP datagram. All chunks retain stable order and the
        // same snapshot identity so the UE side can apply them atomically.
        DroneManager scaleManager;
        DroneSessionConfig scaleDefaults;
        scaleDefaults.receiveOnly = true;
        scaleDefaults.heartbeatTimeoutMs = 3000;
        MavlinkDiscoveryService scaleDiscovery(scaleManager, scaleDefaults);
        for (std::uint16_t index = 0; index < 1000; ++index)
        {
            Expect(
                SendHeartbeat(
                    scaleDiscovery,
                    1,
                    "127.0.0.1",
                    static_cast<std::uint16_t>(20000 + index)) ==
                    MavlinkDiscoveryResult::Registered,
                "1000-aircraft scale registration failed");
        }

        dspgcs::DSPGCSTransportConfig scaleReceiverConfig;
        scaleReceiverConfig.type = dspgcs::TransportType::Loopback;
        scaleReceiverConfig.bindAddress = "127.0.0.1";
        scaleReceiverConfig.bindPort = 12532;
        scaleReceiverConfig.remoteAddress = "127.0.0.1";
        scaleReceiverConfig.remotePort = 12531;
        auto scaleReceiver = dspgcs::DSPGCSTransportFactory::CreateTransport(
            scaleReceiverConfig,
            transportError);
        Expect(scaleReceiver != nullptr, "scale DTO receiver factory failed");

        std::mutex scaleFramesMutex;
        std::vector<dspgcs::TransportFrame> scaleFrames;
        scaleReceiver->Receive([&](dspgcs::TransportFrame frame)
        {
            std::lock_guard<std::mutex> lock(scaleFramesMutex);
            scaleFrames.push_back(std::move(frame));
        });
        Expect(scaleReceiver->Start(), "scale DTO receiver failed to start");

        dspgcs::DSPGCSTransportConfig scalePublisherConfig =
            scaleReceiverConfig;
        scalePublisherConfig.bindPort = 12531;
        scalePublisherConfig.remotePort = 12532;
        DSPGCSProductionTelemetryProvider scaleProvider;
        Expect(
            scaleProvider.Start(
                scalePublisherConfig,
                AppConfigDefaults::SnapshotDatagramBytes,
                AppConfigDefaults::SnapshotItemsPerChunk,
                transportError),
            "scale telemetry provider failed to start");
        Expect(
            scaleProvider.PublishSnapshot(scaleManager),
            "1000-aircraft telemetry snapshot send failed");

        std::uint32_t expectedChunkCount = 0;
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            {
                std::lock_guard<std::mutex> lock(scaleFramesMutex);
                if (!scaleFrames.empty() && expectedChunkCount == 0)
                {
                    dspgcs::MessageEnvelope firstEnvelope;
                    std::vector<std::uint8_t> firstPayload;
                    dspgcs::TelemetryBatchDTO firstBatch;
                    if (dspgcs::DecodeEnvelope(
                            scaleFrames.front().bytes,
                            firstEnvelope,
                            firstPayload) &&
                        dspgcs::DecodeTelemetry(firstPayload, firstBatch))
                    {
                        expectedChunkCount = firstBatch.chunkCount;
                    }
                }
                if (expectedChunkCount > 0 &&
                    scaleFrames.size() >= expectedChunkCount)
                {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        scaleProvider.Stop();
        scaleReceiver->Stop();
        std::vector<dspgcs::TransportFrame> framesToVerify;
        {
            std::lock_guard<std::mutex> lock(scaleFramesMutex);
            framesToVerify = std::move(scaleFrames);
        }
        Expect(expectedChunkCount > 1, "1000-aircraft snapshot was not chunked");
        Expect(
            framesToVerify.size() == expectedChunkCount,
            "1000-aircraft snapshot chunks were lost");

        std::vector<bool> chunkSeen(expectedChunkCount, false);
        std::size_t decodedDroneCount = 0;
        std::string scaleSnapshotId;
        for (const dspgcs::TransportFrame& frame : framesToVerify)
        {
            Expect(
                frame.bytes.size() <=
                    AppConfigDefaults::SnapshotDatagramBytes,
                "telemetry chunk exceeded safe UDP payload size");
            dspgcs::MessageEnvelope chunkEnvelope;
            std::vector<std::uint8_t> chunkPayload;
            dspgcs::TelemetryBatchDTO chunkBatch;
            Expect(
                dspgcs::DecodeEnvelope(
                    frame.bytes,
                    chunkEnvelope,
                    chunkPayload) &&
                dspgcs::DecodeTelemetry(chunkPayload, chunkBatch),
                "1000-aircraft telemetry chunk failed to decode");
            Expect(
                chunkBatch.chunkCount == expectedChunkCount &&
                chunkBatch.chunkIndex < expectedChunkCount &&
                !chunkSeen[chunkBatch.chunkIndex],
                "telemetry chunk metadata was inconsistent");
            chunkSeen[chunkBatch.chunkIndex] = true;
            if (scaleSnapshotId.empty())
            {
                scaleSnapshotId = chunkBatch.snapshotId;
            }
            Expect(
                chunkBatch.snapshotId == scaleSnapshotId,
                "telemetry chunks did not share a snapshot ID");
            decodedDroneCount += chunkBatch.items.size();
        }
        Expect(
            decodedDroneCount == 1000,
            "chunked telemetry snapshot did not preserve all 1000 drones");
        std::cout
            << "1000-aircraft DSP-GCS snapshot passed: chunks="
            << expectedChunkCount << " drones=" << decodedDroneCount << '\n';

        std::cout
            << "PASS: endpoint identity registered/refreshed online state; "
            << "multi-target command routing was exercised only against loopback simulators.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

int main(const int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--frozen-e2e-server")
    {
        return RunDSPFrozenAssignmentE2EServer(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "--artifact-v2-contract-tests")
    {
        return RunRouteArtifactV2ContractTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--drone-id-tests")
    {
        return RunDroneLogicalIdTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--console-render-tests")
    {
        return RunConsoleRenderTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--artifact-v2-consumer-tests")
    {
        return RunConsoleArtifactConsumerV2Tests();
    }
    if (argc > 1 && std::string(argv[1]) == "--artifact-v2-scale-10k")
    {
        return RunConsoleArtifactConsumerV2Scale10K();
    }
    if (argc > 1 && std::string(argv[1]) == "--mission-tests")
    {
        const int codecResult = RunRouteMissionCodecTests();
        const int compilerResult = RunMissionFtpCompilerTests();
        return codecResult == 0 && compilerResult == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--mission-benchmark")
    {
        return RunRouteMissionBenchmarks(argc, argv);
    }
    if (argc > 1 && std::string(argv[1]) == "--upload-job-tests")
    {
        return RunDSPUploadJobProtocolTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--runtime-config-tests")
    {
        return RunMavFtpRuntimeConfigTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--mavftp-probe-tests")
    {
        const int probeResult = RunMavFtpProbeTests();
        const int writerResult = RunMAVFTPMissionWriterTests();
        const int harnessResult = RunMAVFTPMissionHardwareTestTests();
        return probeResult == 0 && writerResult == 0 && harnessResult == 0
            ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--mission-upload-tests")
    {
        return RunMissionUploadTests();
    }
    if (argc > 1 && std::string(argv[1]) == "--mavftp-concurrency-tests")
    {
        const int permitResult = RunMAVFTPWritePermitConcurrencyTests();
        const int schedulerResult = RunRouteUploadConcurrencySchedulerTests();
        return permitResult == 0 && schedulerResult == 0 ? 0 : 1;
    }

    const int pipelineResult = RunConsolePipelineTests();
    const int commandResult = RunCommandTests();
    const int dspCommandResult = RunDSPCommandTests();
    const int frozenAssignmentResult = RunDSPFrozenAssignmentProtocolTests();
    const int frozenAssignmentReceiverResult =
        RunDSPFrozenAssignmentReceiverTests();
    const int artifactConsumerV2Result = RunConsoleArtifactConsumerV2Tests();
    const int artifactV2ContractResult = RunRouteArtifactV2ContractTests();
    const int missionCodecResult = RunRouteMissionCodecTests();
    const int missionCompilerResult = RunMissionFtpCompilerTests();
    const int uploadJobResult = RunDSPUploadJobProtocolTests();
    const int mavFtpProbeResult = RunMavFtpProbeTests();
    const int mavFtpWriterResult = RunMAVFTPMissionWriterTests();
    const int mavFtpHarnessResult = RunMAVFTPMissionHardwareTestTests();
    const int missionUploadResult = RunMissionUploadTests();
    const int droneLogicalIdResult = RunDroneLogicalIdTests();
    const int consoleRenderResult = RunConsoleRenderTests();

    if (pipelineResult == 0 && commandResult == 0 && dspCommandResult == 0 &&
        frozenAssignmentResult == 0 && frozenAssignmentReceiverResult == 0
        && artifactConsumerV2Result == 0
        && artifactV2ContractResult == 0
        && missionCodecResult == 0
        && missionCompilerResult == 0
        && uploadJobResult == 0
        && mavFtpProbeResult == 0
        && mavFtpWriterResult == 0
        && mavFtpHarnessResult == 0
        && missionUploadResult == 0
        && droneLogicalIdResult == 0
        && consoleRenderResult == 0)
    {
        std::cout << "ConsoleGCS regression suite passed\n";
        return 0;
    }

    std::cerr
        << "ConsoleGCS regression suite failed: pipeline=" << pipelineResult
        << " command=" << commandResult
        << " dspCommand=" << dspCommandResult
        << " frozenAssignment=" << frozenAssignmentResult
        << " frozenAssignmentReceiver=" << frozenAssignmentReceiverResult
        << " artifactConsumerV2=" << artifactConsumerV2Result
        << " artifactV2Contract=" << artifactV2ContractResult
        << " missionCodec=" << missionCodecResult
        << " missionCompiler=" << missionCompilerResult
        << " uploadJob=" << uploadJobResult
        << " mavFtpProbe=" << mavFtpProbeResult
        << " mavFtpWriter=" << mavFtpWriterResult
        << " mavFtpHarness=" << mavFtpHarnessResult
        << " missionUpload=" << missionUploadResult
        << '\n';
    return 1;
}
