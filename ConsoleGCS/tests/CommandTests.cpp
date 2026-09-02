#define WIN32_LEAN_AND_MEAN

#include "../CommandDispatcher.h"
#include "../CommandManager.h"
#include "../DroneManager.h"
#include "../MavlinkDatagramSender.h"
#include "../SafetyConfig.h"

#include <chrono>
#include <cmath>
#include <cstdint>
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

int RunCommandTests()
{
    const int dispatcherResult = RunCommandPacketTests();
    const int managerResult = RunCommandManagerSafeModeTests();
    return dispatcherResult == 0 && managerResult == 0 ? 0 : 1;
}
