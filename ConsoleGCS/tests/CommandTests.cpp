#define WIN32_LEAN_AND_MEAN

#include "../CommandDispatcher.h"
#include "../CommandManager.h"
#include "../DroneManager.h"
#include "../MavlinkDatagramSender.h"
#include "../SafetyConfig.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{
    bool Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
        }

        return condition;
    }

    class CapturingDatagramSender final : public IMavlinkDatagramSender
    {
    public:
        bool SendTo(
            const std::string& targetIp,
            const std::uint16_t targetPort,
            const std::uint8_t* data,
            const int length) override
        {
            if (targetIp != "127.0.0.1" ||
                targetPort != 30001 ||
                data == nullptr ||
                length <= 0)
            {
                return false;
            }

            datagrams.emplace_back(data, data + length);
            return true;
        }

        bool ConsumeMessage(mavlink_message_t& message)
        {
            std::vector<std::uint8_t> datagram;
            if (!ConsumeDatagram(datagram))
            {
                return false;
            }
            mavlink_status_t status{};
            for (const std::uint8_t byte : datagram)
            {
                if (mavlink_parse_char(
                    MAVLINK_COMM_1,
                    byte,
                    &message,
                    &status))
                {
                    return true;
                }
            }
            return false;
        }

        bool ConsumeDatagram(std::vector<std::uint8_t>& datagram)
        {
            if (datagrams.empty())
            {
                return false;
            }
            datagram = std::move(datagrams.front());
            datagrams.erase(datagrams.begin());
            return true;
        }

    private:
        std::vector<std::vector<std::uint8_t>> datagrams;
    };
}

int RunCommandPacketTests()
{
    CapturingDatagramSender endpoint;
    UdpClient sender;

    bool passed = Check(
        sender.AttachSharedEndpoint(endpoint, "127.0.0.1", 30001),
        "shared Endpoint sender attaches");

    CommandDispatcher dispatcher(sender);
    const auto now =
        std::chrono::steady_clock::now();

    dispatcher.QueueArm(1, 1);
    dispatcher.Tick(now);

    mavlink_message_t received{};
    passed &= Check(
        endpoint.ConsumeMessage(received),
        "ARM packet is emitted");
    passed &= Check(
        received.msgid == MAVLINK_MSG_ID_COMMAND_LONG,
        "ARM uses COMMAND_LONG");

    mavlink_command_long_t command{};
    mavlink_msg_command_long_decode(
        &received,
        &command);

    passed &= Check(
        command.command == MAV_CMD_COMPONENT_ARM_DISARM,
        "ARM command id is correct");
    passed &= Check(
        std::fabs(command.param1 - 1.0f) < 0.001f,
        "ARM param1 equals 1");
    passed &= Check(
        std::fabs(command.param2) < 0.001f,
        "ARM param2 keeps safety checks enabled");

    mavlink_message_t ack{};
    mavlink_msg_command_ack_pack(
        1,
        1,
        &ack,
        MAV_CMD_COMPONENT_ARM_DISARM,
        MAV_RESULT_ACCEPTED,
        100,
        0,
        255,
        MAV_COMP_ID_MISSIONPLANNER);

    dispatcher.HandleAck(ack);

    auto results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Accepted,
        "matching ACK completes ARM");

    struct ExpectedMode
    {
        std::uint8_t customMode;
        const char* name;
    };
    const ExpectedMode expectedModes[] =
    {
        { 0, "STABILIZE" },
        { 3, "AUTO" },
        { 4, "GUIDED" }
    };
    for (std::size_t index = 0; index < std::size(expectedModes); ++index)
    {
        const ExpectedMode& expected = expectedModes[index];
        dispatcher.QueueSetMode(1, 1, expected.customMode);
        dispatcher.Tick(now + std::chrono::milliseconds(index + 1));

        passed &= Check(
            endpoint.ConsumeMessage(received),
            std::string(expected.name) + " SET_MODE packet is emitted");
        passed &= Check(
            received.msgid == MAVLINK_MSG_ID_SET_MODE,
            std::string(expected.name) + " uses MAVLink SET_MODE");

        mavlink_set_mode_t setMode{};
        mavlink_msg_set_mode_decode(&received, &setMode);
        passed &= Check(
            setMode.target_system == 1,
            std::string(expected.name) + " target_system is correct");
        passed &= Check(
            setMode.base_mode == MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
            std::string(expected.name) + " enables custom mode");
        passed &= Check(
            setMode.custom_mode == expected.customMode,
            std::string(expected.name) + " custom_mode is correct");
        std::cout
            << "SET_MODE PACKET " << expected.name
            << " target_system=" << static_cast<int>(setMode.target_system)
            << " base_mode=" << static_cast<int>(setMode.base_mode)
            << " custom_mode=" << setMode.custom_mode << '\n';

        results = dispatcher.ConsumeResults();
        passed &= Check(
            results.empty(),
            std::string(expected.name) + " SET_MODE waits for HEARTBEAT confirmation");

		mavlink_message_t modeHeartbeat{};
		mavlink_msg_heartbeat_pack(
			1,
			1,
			&modeHeartbeat,
			MAV_TYPE_QUADROTOR,
			MAV_AUTOPILOT_ARDUPILOTMEGA,
			MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
			expected.customMode,
			MAV_STATE_ACTIVE);
		dispatcher.HandleHeartbeat(modeHeartbeat);
		results = dispatcher.ConsumeResults();
		passed &= Check(
			results.size() == 1 &&
			results.front().outcome == CommandOutcome::Accepted,
			std::string(expected.name) + " SET_MODE is confirmed by HEARTBEAT");
    }

    dispatcher.QueueRTL(1, 1);
    dispatcher.Tick(now + std::chrono::milliseconds(10));
    passed &= Check(
        endpoint.ConsumeMessage(received),
        "RTL SET_MODE packet is emitted");
    passed &= Check(
        received.msgid == MAVLINK_MSG_ID_SET_MODE,
        "RTL uses MAVLink SET_MODE instead of MAV_CMD 20");
    mavlink_set_mode_t rtlMode{};
    mavlink_msg_set_mode_decode(&received, &rtlMode);
    passed &= Check(
        rtlMode.target_system == 1 &&
        rtlMode.base_mode == MAV_MODE_FLAG_CUSTOM_MODE_ENABLED &&
        rtlMode.custom_mode == 6,
        "RTL requests ArduCopter custom_mode 6");
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.empty(),
        "RTL remains pending until HEARTBEAT confirms mode 6");

    mavlink_message_t heartbeat{};
    mavlink_msg_heartbeat_pack(
        1,
        1,
        &heartbeat,
        MAV_TYPE_QUADROTOR,
        MAV_AUTOPILOT_ARDUPILOTMEGA,
        MAV_MODE_FLAG_CUSTOM_MODE_ENABLED,
        6,
        MAV_STATE_ACTIVE);
    dispatcher.HandleHeartbeat(heartbeat);
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Accepted &&
        results.front().modeConfirmationRequired &&
        results.front().expectedCustomMode == 6,
        "RTL completes only after HEARTBEAT confirms custom_mode 6");

    const auto VerifyLegacyMissionCommand =
        [&](const char* label,
            const std::uint16_t expectedCommand,
            const float expectedParam1,
            const auto& enqueue,
            const int tickOffset)
        {
            enqueue();
            dispatcher.Tick(now + std::chrono::milliseconds(tickOffset));
            const std::string prefix = std::string(label) + " ";
            passed &= Check(
                endpoint.ConsumeMessage(received),
                prefix + "packet is emitted");
            passed &= Check(
                received.msgid == MAVLINK_MSG_ID_COMMAND_LONG,
                prefix + "uses COMMAND_LONG");
            mavlink_msg_command_long_decode(&received, &command);
            passed &= Check(
                command.command == expectedCommand,
                prefix + "command id matches the legacy BGC2 contract");
            passed &= Check(
                std::fabs(command.param1 - expectedParam1) < 0.001f,
                prefix + "param1 matches the legacy BGC2 contract");

            mavlink_msg_command_ack_pack(
                1,
                1,
                &ack,
                expectedCommand,
                MAV_RESULT_ACCEPTED,
                100,
                0,
                255,
                MAV_COMP_ID_MISSIONPLANNER);
            dispatcher.HandleAck(ack);
            results = dispatcher.ConsumeResults();
            passed &= Check(
                results.size() == 1 &&
                results.front().outcome == CommandOutcome::Accepted,
                prefix + "completes on matching ACK");
        };

    VerifyLegacyMissionCommand(
        "MISSION_PAUSE",
        MAV_CMD_NAV_LOITER_UNLIM,
        1.0f,
        [&dispatcher]() { dispatcher.QueueMissionPause(1, 1); },
        11);
    VerifyLegacyMissionCommand(
        "MISSION_FORWARD",
        MAV_CMD_NAV_RETURN_TO_LAUNCH,
        0.0f,
        [&dispatcher]() { dispatcher.QueueMissionForward(1, 1); },
        12);
    VerifyLegacyMissionCommand(
        "MISSION_BACKWARD",
        MAV_CMD_NAV_RETURN_TO_LAUNCH,
        1.0f,
        [&dispatcher]() { dispatcher.QueueMissionBackward(1, 1); },
        13);

    const auto AckCommand = [&](const std::uint16_t commandId)
    {
        mavlink_msg_command_ack_pack(
            1, 1, &ack, commandId, MAV_RESULT_ACCEPTED,
            100, 0, 255, MAV_COMP_ID_MISSIONPLANNER);
        dispatcher.HandleAck(ack);
    };

    dispatcher.QueueChangeSpeed(1, 1, 3.5f);
    dispatcher.Tick(now + std::chrono::milliseconds(20));
    passed &= Check(endpoint.ConsumeMessage(received),
        "advanced speed packet is emitted");
    mavlink_msg_command_long_decode(&received, &command);
    passed &= Check(command.command == MAV_CMD_USER_2 &&
        std::fabs(command.param1 - 3.5f) < 0.001f,
        "advanced speed uses audited USER_2 contract");
    AckCommand(MAV_CMD_USER_2);
    passed &= Check(dispatcher.ConsumeResults().size() == 1,
        "advanced speed completes on ACK");

    const std::uint32_t intervalMessageIds[] = { 1, 24, 33, 30, 187 };
    dispatcher.QueueStopTelemetry(1, 1);
    for (std::size_t index = 0; index < std::size(intervalMessageIds); ++index)
    {
        dispatcher.Tick(now + std::chrono::milliseconds(30 + index));
        passed &= Check(endpoint.ConsumeMessage(received),
            "stop telemetry emits every interval command");
        mavlink_msg_command_long_decode(&received, &command);
        passed &= Check(command.command == MAV_CMD_SET_MESSAGE_INTERVAL &&
            std::fabs(command.param1 - static_cast<float>(intervalMessageIds[index])) < 0.001f &&
            std::fabs(command.param2 + 1.0f) < 0.001f,
            "stop telemetry disables only the audited message id");
        AckCommand(MAV_CMD_SET_MESSAGE_INTERVAL);
    }
    results = dispatcher.ConsumeResults();
    passed &= Check(results.size() == 1 &&
        results.front().outcome == CommandOutcome::Accepted,
        "stop telemetry completes after all five ACKs");

    dispatcher.QueueResumeTelemetry(1, 1);
    for (std::size_t index = 0; index < std::size(intervalMessageIds); ++index)
    {
        dispatcher.Tick(now + std::chrono::milliseconds(40 + index));
        passed &= Check(endpoint.ConsumeMessage(received),
            "resume telemetry emits every interval command");
        mavlink_msg_command_long_decode(&received, &command);
        passed &= Check(command.command == MAV_CMD_SET_MESSAGE_INTERVAL &&
            std::fabs(command.param1 - static_cast<float>(intervalMessageIds[index])) < 0.001f &&
            std::fabs(command.param2 - 1000000.0f) < 0.001f,
            "resume telemetry restores every audited message to 1 Hz");
        AckCommand(MAV_CMD_SET_MESSAGE_INTERVAL);
    }
    passed &= Check(dispatcher.ConsumeResults().empty(),
        "resume telemetry waits for a fresh MDHK_STATUS sample");
    dispatcher.HandleTelemetryMessage(187);
    results = dispatcher.ConsumeResults();
    passed &= Check(results.size() == 1 &&
        results.front().outcome == CommandOutcome::Accepted,
        "fresh MDHK_STATUS confirms telemetry recovery");

    dispatcher.QueueTriggerSmoke(1, 1);
    dispatcher.Tick(now + std::chrono::milliseconds(50));
    passed &= Check(endpoint.ConsumeMessage(received),
        "smoke relay-on packet is emitted");
    mavlink_msg_command_long_decode(&received, &command);
    passed &= Check(command.command == MAV_CMD_DO_SET_RELAY &&
        std::fabs(command.param1) < 0.001f &&
        std::fabs(command.param2 - 1.0f) < 0.001f,
        "smoke relay 0 is enabled");
    passed &= Check(dispatcher.ConsumeResults().empty(),
        "smoke remains pending until relay reset");
    dispatcher.Tick(now + std::chrono::milliseconds(950));
    passed &= Check(!endpoint.ConsumeMessage(received),
        "smoke relay reset waits for one second even without ON ACK");
    dispatcher.Tick(now + std::chrono::milliseconds(1150));
    passed &= Check(endpoint.ConsumeMessage(received),
        "smoke relay reset packet is emitted after pulse");
    mavlink_msg_command_long_decode(&received, &command);
    passed &= Check(command.command == MAV_CMD_DO_SET_RELAY &&
        std::fabs(command.param1) < 0.001f &&
        std::fabs(command.param2) < 0.001f,
        "smoke relay 0 is automatically reset");
    AckCommand(MAV_CMD_DO_SET_RELAY);
    passed &= Check(dispatcher.ConsumeResults().size() == 1,
        "smoke completes only after reset ACK");

    const auto VerifyAdvancedCommand =
        [&](const char* name,
            const std::uint16_t expectedCommand,
            const float expectedParam1,
            const float expectedParam2,
            const std::function<void()>& queueCommand,
            const int tickOffsetMilliseconds)
        {
            queueCommand();
            dispatcher.Tick(now + std::chrono::milliseconds(tickOffsetMilliseconds));
            passed &= Check(endpoint.ConsumeMessage(received),
                (std::string(name) + " packet is emitted").c_str());
            mavlink_msg_command_long_decode(&received, &command);
            passed &= Check(command.command == expectedCommand &&
                std::fabs(command.param1 - expectedParam1) < 0.001f &&
                std::fabs(command.param2 - expectedParam2) < 0.001f,
                (std::string(name) + " matches audited MAVLink contract").c_str());
            AckCommand(expectedCommand);
            passed &= Check(dispatcher.ConsumeResults().size() == 1,
                (std::string(name) + " completes on ACK").c_str());
        };

    VerifyAdvancedCommand("disable LED output", MAV_CMD_USER_3, 1.0f, 0.0f,
        [&dispatcher]() { dispatcher.QueueSetLedOutputDisabled(1, 1, true); }, 1200);
    VerifyAdvancedCommand("restore LED output", MAV_CMD_USER_3, 0.0f, 0.0f,
        [&dispatcher]() { dispatcher.QueueSetLedOutputDisabled(1, 1, false); }, 1210);
    VerifyAdvancedCommand("wake", MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 4.0f, 0.0f,
        [&dispatcher]() { dispatcher.QueuePowerControl(1, 1, 4, false); }, 1220);
    VerifyAdvancedCommand("standby", MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 2.0f, 0.0f,
        [&dispatcher]() { dispatcher.QueuePowerControl(1, 1, 2, true); }, 1230);
    VerifyAdvancedCommand("reboot", MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 1.0f, 0.0f,
        [&dispatcher]() { dispatcher.QueuePowerControl(1, 1, 1, true); }, 1240);
    VerifyAdvancedCommand("shutdown", MAV_CMD_DO_SET_RELAY, 1.0f, 1.0f,
        [&dispatcher]() { dispatcher.QueueShutdown(1, 1); }, 1250);

    dispatcher.QueueTakeoff(1, 1, 1.0f);
    dispatcher.QueueLand(1, 1);
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Superseded,
        "LAND supersedes queued TAKEOFF");

    dispatcher.Tick(now);
    passed &= Check(
        endpoint.ConsumeMessage(received),
        "LAND packet is emitted");
    mavlink_msg_command_long_decode(
        &received,
        &command);
    passed &= Check(
        command.command == MAV_CMD_NAV_LAND,
        "priority packet is LAND");

    mavlink_msg_command_ack_pack(
        1,
        1,
        &ack,
        MAV_CMD_NAV_LAND,
        MAV_RESULT_ACCEPTED,
        100,
        0,
        255,
        MAV_COMP_ID_MISSIONPLANNER);
    dispatcher.HandleAck(ack);
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Accepted,
        "matching ACK completes LAND");

    dispatcher.QueueSetLedColor(1, 1, 255, 48, 48);
    dispatcher.Tick(now + std::chrono::milliseconds(10));
    passed &= Check(
        endpoint.ConsumeMessage(received),
        "LED_CONTROL packet is emitted");
    passed &= Check(
        received.msgid == MAVLINK_MSG_ID_LED_CONTROL,
        "LED color uses MAVLink LED_CONTROL #186");

    mavlink_led_control_t ledControl{};
    mavlink_msg_led_control_decode(&received, &ledControl);
    passed &= Check(
        ledControl.target_system == 0 && ledControl.target_component == 0,
        "LED_CONTROL uses legacy payload target 0/0");
    passed &= Check(
        ledControl.instance == 255 && ledControl.pattern == 255,
        "LED_CONTROL instance and pattern are 255");
    passed &= Check(
        ledControl.custom_len == 3,
        "LED_CONTROL custom_len equals 3");
    passed &= Check(
        ledControl.custom_bytes[0] == 255 &&
        ledControl.custom_bytes[1] == 48 &&
        ledControl.custom_bytes[2] == 48,
        "LED_CONTROL custom bytes carry requested RGB");
    bool remainingBytesZero = true;
    for (std::size_t index = 3; index < std::size(ledControl.custom_bytes); ++index)
    {
        remainingBytesZero &= ledControl.custom_bytes[index] == 0;
    }
    passed &= Check(
        remainingBytesZero,
        "LED_CONTROL unused custom bytes are zero");
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Sent &&
        results.front().attempts == 1,
        "LED_CONTROL is fire-and-forget without ACK retry");

    dispatcher.QueueTimeControl(1, 1, 1, 1, 14, 5, 38);
    dispatcher.Tick(now + std::chrono::milliseconds(11));
    std::vector<std::uint8_t> timeControlDatagram;
    passed &= Check(
        endpoint.ConsumeDatagram(timeControlDatagram),
        "MP_TIME_CONTROL packet is emitted");
    if (!timeControlDatagram.empty())
    {
        const bool mavlink2 = timeControlDatagram.front() == MAVLINK_STX;
        const std::size_t payloadOffset = mavlink2 ? 10 : 6;
        const std::uint32_t messageId = mavlink2
            ? static_cast<std::uint32_t>(timeControlDatagram[7]) |
                (static_cast<std::uint32_t>(timeControlDatagram[8]) << 8) |
                (static_cast<std::uint32_t>(timeControlDatagram[9]) << 16)
            : static_cast<std::uint32_t>(timeControlDatagram[5]);
        passed &= Check(
            timeControlDatagram.size() >= payloadOffset + 7,
            "MP_TIME_CONTROL datagram contains 5-byte payload and checksum");
        passed &= Check(
            timeControlDatagram[1] == 5 && messageId == 78,
            "MP_TIME_CONTROL uses message 78 with LEN 5");
        if (timeControlDatagram.size() >= payloadOffset + 5)
        {
            passed &= Check(
                timeControlDatagram[payloadOffset] == 1 &&
                timeControlDatagram[payloadOffset + 1] == 1 &&
                timeControlDatagram[payloadOffset + 2] == 14 &&
                timeControlDatagram[payloadOffset + 3] == 5 &&
                timeControlDatagram[payloadOffset + 4] == 38,
                "MP_TIME_CONTROL payload is control/mask/hour/minute/second");
        }
    }
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 &&
        results.front().outcome == CommandOutcome::Sent &&
        results.front().attempts == 1,
        "MP_TIME_CONTROL is fire-and-forget without COMMAND_ACK retry");

    dispatcher.QueueExecuteDanceStep(1, 1);
    dispatcher.Tick(now + std::chrono::milliseconds(12));
    passed &= Check(
        endpoint.ConsumeMessage(received) && received.msgid == MAVLINK_MSG_ID_SET_MODE,
        "EXECUTE_DANCE_STEP emits SET_MODE first");
    mavlink_set_mode_t danceMode{};
    mavlink_msg_set_mode_decode(&received, &danceMode);
    passed &= Check(
        danceMode.target_system == 1 && danceMode.custom_mode == 3,
        "EXECUTE_DANCE_STEP selects ArduCopter AUTO mode");
    passed &= Check(
        endpoint.ConsumeMessage(received) && received.msgid == MAVLINK_MSG_ID_COMMAND_LONG,
        "EXECUTE_DANCE_STEP emits start COMMAND_LONG second");
    mavlink_msg_command_long_decode(&received, &command);
    passed &= Check(
        command.command == MAV_CMD_USER_1 &&
        std::fabs(command.param1 - 1.0f) < 0.001f,
        "EXECUTE_DANCE_STEP uses MAV_CMD_USER_1 param1=1");
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 && results.front().outcome == CommandOutcome::Sent,
        "EXECUTE_DANCE_STEP completes after both datagrams are sent");

    dispatcher.QueueEndDanceStep(1, 1, 1);
    dispatcher.Tick(now + std::chrono::milliseconds(13));
    timeControlDatagram.clear();
    passed &= Check(
        endpoint.ConsumeDatagram(timeControlDatagram),
        "END_DANCE_STEP emits MP_TIME_CONTROL cancel first");
    if (!timeControlDatagram.empty())
    {
        const bool mavlink2 = timeControlDatagram.front() == MAVLINK_STX;
        const std::size_t payloadOffset = mavlink2 ? 10 : 6;
        if (timeControlDatagram.size() >= payloadOffset + 5)
        {
            passed &= Check(
                timeControlDatagram[payloadOffset] == 0 &&
                timeControlDatagram[payloadOffset + 1] == 1 &&
                timeControlDatagram[payloadOffset + 2] == 0 &&
                timeControlDatagram[payloadOffset + 3] == 0 &&
                timeControlDatagram[payloadOffset + 4] == 0,
                "END_DANCE_STEP cancellation payload is 0/1/0/0/0");
        }
    }
    passed &= Check(
        endpoint.ConsumeMessage(received) && received.msgid == MAVLINK_MSG_ID_COMMAND_LONG,
        "END_DANCE_STEP emits land COMMAND_LONG second");
    mavlink_msg_command_long_decode(&received, &command);
    passed &= Check(
        command.command == MAV_CMD_USER_1 &&
        std::fabs(command.param1) < 0.001f,
        "END_DANCE_STEP uses MAV_CMD_USER_1 param1=0");
    results = dispatcher.ConsumeResults();
    passed &= Check(
        results.size() == 1 && results.front().outcome == CommandOutcome::Sent,
        "END_DANCE_STEP completes after cancel and land datagrams are sent");
    sender.Close();

    if (passed)
    {
        std::cout << "CommandDispatcher tests passed\n";
        return 0;
    }

    return 1;
}

int RunCommandManagerSafeModeTests()
{
    if (!SafetyConfig::SAFE_TEST_MODE)
    {
        std::cout
            << "CommandManager tests skipped: SAFE_TEST_MODE is disabled\n";
        return 0;
    }

    DroneManager droneManager;
    DroneSessionConfig config;
    config.droneId = "Drone001";

    bool passed = Check(
        droneManager.AddDrone(config),
        "single DroneSession is registered");
    passed &= Check(
        droneManager.FindDrone("Drone001") != nullptr,
        "registered DroneID is queryable");
    passed &= Check(
        droneManager.GetAllDrones().size() == 1,
        "GetAllDrones returns the single session");

    CommandManager commandManager(droneManager);
    const std::uint64_t armCommandId =
        commandManager.HandleConsoleKey('A', "Drone001");
    const auto completed = commandManager.ConsumeCompletedCommands();

    passed &= Check(armCommandId != 0, "keyboard creates a CommandID");
    passed &= Check(completed.size() == 1, "SAFE_TEST command completes locally");

    if (!completed.empty())
    {
        const ManagedDroneCommand& command = completed.front();
        passed &= Check(
            command.request.commandId == armCommandId,
            "CommandID is preserved");
        passed &= Check(
            command.request.commandType == DroneCommandType::Arm,
            "keyboard A maps to ARM");
        passed &= Check(
            command.request.targetDroneIds.size() == 1 &&
            command.request.targetDroneIds.front() == "Drone001",
            "single target DroneID is preserved");
        passed &= Check(
            command.request.source == "Keyboard",
            "command source is recorded");
        passed &= Check(
            command.request.timestamp !=
                std::chrono::system_clock::time_point{},
            "command timestamp is recorded");
        passed &= Check(
            command.targetResults.size() == 1 &&
            command.targetResults.front().state ==
                DroneCommandTargetState::Simulated,
            "SAFE_TEST_MODE never dispatches a real ARM command");
    }

    passed &= Check(
        commandManager.GetPendingCommands().empty(),
        "simulated command does not remain pending");

    if (passed)
    {
        std::cout << "CommandManager SAFE_TEST tests passed\n";
        return 0;
    }

    return 1;
}

// Production Session/dispatcher/encoder, with an in-memory sender only. No
// socket, real aircraft, arming command or flight-controller parameter write.
int RunSessionDisarmTests()
{
    bool passed = true;
    struct Scenario
    {
        const char* name;
        bool gpsFixed;
        int relativeAltitudeMm;
        int ackResult; // -1: no ACK, exercise timeout/retry without wall-clock sleeps
    };
    const Scenario scenarios[] =
    {
        { "indoor no GPS accepted", false, -100, MAV_RESULT_ACCEPTED },
        { "indoor no GPS denied", false, -100, MAV_RESULT_DENIED },
        { "indoor no GPS timeout", false, -100, -1 },
        { "GPS ground accepted", true, 0, MAV_RESULT_ACCEPTED },
        { "relative height is not landing state", true, 5000, MAV_RESULT_DENIED }
    };
    for (const Scenario& scenario : scenarios)
    {
        const std::string label = std::string(scenario.name) + ": ";
        CapturingDatagramSender endpoint;
        DroneSessionConfig config;
        config.droneId = "Drone000";
        config.remoteIp = "127.0.0.1";
        config.remotePort = 30001;
        config.realCommandEnabled = true;
        config.sharedMavlinkEndpoint = &endpoint;
        config.discoveredSystemId = 1;
        config.discoveredComponentId = 1;
        DroneSession session(config);
        passed &= Check(session.Start(), label + "session starts with capturing endpoint");

        DroneCommandRequest request;
        request.commandId = 1;
        request.commandType = DroneCommandType::Disarm;
        const auto offline = session.SubmitCommand(request);
        passed &= Check(!offline.accepted && offline.error == "vehicle offline",
            label + "offline gate retained");

        DiscoveredDroneIdentity identity;
        identity.sourceIp = config.remoteIp;
        identity.sourcePort = static_cast<std::uint16_t>(config.remotePort);
        identity.systemId = 1;
        identity.componentId = 1;
        identity.observedAt = std::chrono::steady_clock::now();
        mavlink_message_t message{};
        mavlink_msg_heartbeat_pack(1, 1, &message, MAV_TYPE_QUADROTOR,
            MAV_AUTOPILOT_ARDUPILOTMEGA,
            MAV_MODE_FLAG_CUSTOM_MODE_ENABLED | MAV_MODE_FLAG_SAFETY_ARMED,
            ArduCopterCustomMode::Stabilize, MAV_STATE_ACTIVE);
        passed &= Check(session.ApplyDiscoveredHeartbeat(message, identity),
            label + "armed telemetry applied, not an ARM command");

        mavlink_gps_raw_int_t gps{};
        gps.fix_type = scenario.gpsFixed ? 3 : 1;
        mavlink_msg_gps_raw_int_encode(1, 1, &message, &gps);
        passed &= Check(session.ApplyDiscoveredTelemetry(message, identity),
            label + "GPS telemetry applied");
        mavlink_global_position_int_t position{};
        position.relative_alt = scenario.relativeAltitudeMm;
        mavlink_msg_global_position_int_encode(1, 1, &message, &position);
        passed &= Check(session.ApplyDiscoveredTelemetry(message, identity),
            label + "relative altitude telemetry applied");
        passed &= Check(session.GetFlightData().armed &&
            session.GetFlightData().altitudeValid == scenario.gpsFixed,
            label + "reproduces armed/GPS validity inputs");

        const auto submission = session.SubmitCommand(request);
        passed &= Check(submission.accepted && !submission.completedImmediately &&
            !submission.simulated && submission.dispatchRequestIds.size() == 1,
            label + "ordinary DISARM queues and waits for flight controller");
        // Keep the baseline failure bounded: do not wait if the old GPS gate rejects.
        if (!submission.accepted) continue;
        const auto now = std::chrono::steady_clock::now();
        session.Tick(now);
        auto checkPackets = [&]()
        {
            int count = 0;
            mavlink_message_t packet{};
            while (endpoint.ConsumeMessage(packet))
            {
                if (packet.msgid != MAVLINK_MSG_ID_COMMAND_LONG) continue;
                mavlink_command_long_t command{};
                mavlink_msg_command_long_decode(&packet, &command);
                passed &= Check(command.command == MAV_CMD_COMPONENT_ARM_DISARM &&
                    command.target_system == 1 && command.target_component == 1 &&
                    command.param1 == 0.0f && command.param2 == 0.0f,
                    label + "only normal targeted DISARM, never ARM or forced DISARM");
                ++count;
            }
            return count;
        };
        passed &= Check(checkPackets() == 1, label + "one DISARM packet emitted");
        passed &= Check(session.ConsumeCommandResults().empty(),
            label + "send alone is not success");

        // Wrong endpoint and wrong MAV_CMD must not complete this request.
        mavlink_msg_command_ack_pack(1, 1, &message, MAV_CMD_COMPONENT_ARM_DISARM,
            MAV_RESULT_ACCEPTED, 100, 0, 255, MAV_COMP_ID_MISSIONPLANNER);
        auto wrongEndpoint = identity;
        wrongEndpoint.sourcePort = 30002;
        passed &= Check(!session.ApplyDiscoveredCommandMessage(message, wrongEndpoint),
            label + "other endpoint ACK rejected");
        mavlink_msg_command_ack_pack(1, 1, &message, MAV_CMD_NAV_TAKEOFF,
            MAV_RESULT_ACCEPTED, 100, 0, 255, MAV_COMP_ID_MISSIONPLANNER);
        session.ApplyDiscoveredCommandMessage(message, identity);
        session.Tick(now + std::chrono::milliseconds(1));
        passed &= Check(session.ConsumeCommandResults().empty(),
            label + "unrelated ACK does not succeed");

        if (scenario.ackResult >= 0)
        {
            mavlink_msg_command_ack_pack(1, 1, &message, MAV_CMD_COMPONENT_ARM_DISARM,
                static_cast<std::uint8_t>(scenario.ackResult), 100, 0,
                255, MAV_COMP_ID_MISSIONPLANNER);
            session.ApplyDiscoveredCommandMessage(message, identity);
            session.Tick(now + std::chrono::milliseconds(2));
        }
        else
        {
            for (int tick = 1; tick <= 10; ++tick)
                session.Tick(now + std::chrono::seconds(tick * 2));
        }
        const auto results = session.ConsumeCommandResults();
        const auto expected = scenario.ackResult < 0 ? CommandOutcome::Timeout :
            scenario.ackResult == MAV_RESULT_ACCEPTED ? CommandOutcome::Accepted :
            CommandOutcome::Rejected;
        passed &= Check(results.size() == 1 && results.front().outcome == expected &&
            results.front().requestId == submission.dispatchRequestIds.front(),
            label + "matching ACK or timeout determines result");
        if (scenario.ackResult >= 0 && !results.empty())
            passed &= Check(results.front().ackResult == scenario.ackResult,
                label + "flight controller result preserved");
        const int retries = checkPackets();
        passed &= Check(scenario.ackResult < 0 ? retries > 0 : retries == 0,
            label + "bounded normal retries only on missing ACK");
        passed &= Check(session.GetFlightData().armed,
            label + "ACK does not fabricate a disarmed heartbeat or stopped motors");
    }

    // Read-only and disabled real-command configurations never emit a packet.
    for (const bool receiveOnly : { false, true })
    {
        CapturingDatagramSender endpoint;
        DroneSessionConfig config;
        config.droneId = "Drone000";
        config.remoteIp = "127.0.0.1";
        config.remotePort = 30001;
        config.receiveOnly = receiveOnly;
        config.realCommandEnabled = false;
        config.sharedMavlinkEndpoint = &endpoint;
        DroneSession session(config);
        session.Start();
        DroneCommandRequest request;
        request.commandType = DroneCommandType::Disarm;
        const auto submission = session.SubmitCommand(request);
        passed &= Check(receiveOnly ? !submission.accepted :
            (SafetyConfig::SAFE_TEST_MODE ? submission.simulated : !submission.accepted),
            "disabled/receive-only gate retained");
        session.Tick(std::chrono::steady_clock::now());
        mavlink_message_t packet{};
        passed &= Check(!endpoint.ConsumeMessage(packet), "disabled mode emits no packets");
    }
    if (passed) std::cout << "Session normal DISARM regression passed (memory-only sender)\n";
    return passed ? 0 : 1;
}

int RunCommandTests()
{
    const int dispatcherResult = RunCommandPacketTests();
    const int managerResult = RunCommandManagerSafeModeTests();
    const int disarmResult = RunSessionDisarmTests();
    return dispatcherResult == 0 && managerResult == 0 && disarmResult == 0 ? 0 : 1;
}
