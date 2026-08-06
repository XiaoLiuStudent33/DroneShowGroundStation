#include "CommandManager.h"
#include "DroneManager.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "MavlinkDiscoveryService.h"
#include "MavlinkReadOnlyIngress.h"
#include "MavlinkUdpReceiver.h"
#include "SafetyConfig.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cstdint>
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

namespace
{
    void Expect(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::vector<std::uint8_t> MakeHeartbeatDatagram(
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
        const std::uint16_t sourcePort = 10005)
    {
        const std::vector<std::uint8_t> bytes = SerializeMessage(message);
        return ingress.SubmitDatagram(
            bytes.data(), bytes.size(), sourceIp, sourcePort);
    }

    MavlinkDiscoveryResult SendHeartbeat(
        MavlinkDiscoveryService& discovery,
        const std::uint8_t systemId,
        const std::string& sourceIp,
        const std::uint16_t sourcePort = 10005,
        const std::uint32_t customMode = 5)
    {
        const std::vector<std::uint8_t> bytes =
            MakeHeartbeatDatagram(systemId, MAV_COMP_ID_AUTOPILOT1, customMode);
        return discovery.IngestDatagram(
            bytes.data(),
            bytes.size(),
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
            receiver.Start("127.0.0.1", 0, error),
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

        closesocket(sender);
        Expect(result == MavlinkUdpReceiveResult::Received, "loopback receive failed");
        Expect(received.sourceIp == "127.0.0.1", "source IP extraction failed");
        Expect(received.sourcePort != 0, "source port extraction failed");
        Expect(received.bytes == heartbeat, "received datagram bytes changed");
        std::cout << "UDP receiver loopback source endpoint extraction passed\n";
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
        constexpr std::uint16_t sharedPort = 10005;
        MavlinkUdpReceiver endpoint;
        std::string error;
        Expect(
            endpoint.Start("127.0.0.1", sharedPort, error),
            "shared endpoint failed to bind 127.0.0.1:10005: " + error);

        DroneSessionConfig sessionDefaults;
        sessionDefaults.localIp = "127.0.0.1";
        sessionDefaults.localPort = sharedPort;
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
        request.source = "Phase7.1MultiTargetEndpointTest";
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
                "GCS heartbeat source port was not 10005");

            std::uint16_t commandSourcePort = 0;
            Expect(
                drone->WaitForMessage(
                    MAVLINK_MSG_ID_COMMAND_LONG,
                    commandSourcePort),
                "simulated drone did not receive COMMAND_LONG");
            Expect(
                commandSourcePort == sharedPort,
                "COMMAND_LONG source port was not 10005");

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
            << "Shared endpoint PASS: three registrations, telemetry routes, ACK routes; "
            << "one multi-target ARM expanded to three Sessions; "
            << "GCS heartbeat and COMMAND_LONG source port=10005\n";
    }
}

int main()
{
    try
    {
        if (!SafetyConfig::SAFE_TEST_MODE ||
            SafetyConfig::REAL_COMMAND_ENABLED)
        {
            throw std::runtime_error(
                "Discovery tests require SAFE_TEST_MODE and real commands disabled");
        }

        VerifyUdpReceiverLoopback();
        VerifySharedMavlinkEndpoint();

        DroneSessionConfig sessionDefaults;
        sessionDefaults.localIp = "127.0.0.1";
        sessionDefaults.localPort = 0;
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
            drone001->GetConfig().targetIp == "192.168.1.145" &&
            drone002->GetConfig().targetIp == "192.168.1.231" &&
            drone003->GetConfig().targetIp == "192.168.1.232",
            "same SYSID endpoints were not preserved as independent Sessions");
        ExpectOnline(manager, "Drone001", true);
        ExpectOnline(manager, "Drone002", true);
        ExpectOnline(manager, "Drone003", true);
        std::cout << "Drone001 online\nDrone002 online\nDrone003 online\n";

        mavlink_message_t position{};
        mavlink_msg_global_position_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &position,
            1000,
            319324710,
            1186385310,
            123450,
            5000,
            100,
            200,
            0,
            9000);
        Expect(
            SubmitMessage(ingress, position, "192.168.1.145")
                .telemetryApplied == 1,
            "GLOBAL_POSITION_INT was not routed to Drone001");

        mavlink_message_t gps{};
        mavlink_msg_gps_raw_int_pack(
            1,
            MAV_COMP_ID_AUTOPILOT1,
            &gps,
            1000000,
            3,
            319324710,
            1186385310,
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

        const FlightData telemetry = drone001->GetFlightData();
        Expect(telemetry.gpsValid, "Drone001 GPS did not become valid");
        Expect(telemetry.altitudeValid, "Drone001 altitude did not become valid");
        Expect(
            telemetry.latitude > 31.9324 && telemetry.latitude < 31.9325,
            "Drone001 latitude conversion failed");
        Expect(
            telemetry.longitude > 118.6385 && telemetry.longitude < 118.6386,
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
                    10005,
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
            productionProvider.Start(publisherConfig, transportError),
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
