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
            if (datagrams.empty())
            {
                return false;
            }

            const std::vector<std::uint8_t> datagram =
                std::move(datagrams.front());
            datagrams.erase(datagrams.begin());
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
            results.size() == 1 &&
            results.front().outcome == CommandOutcome::Sent,
            std::string(expected.name) + " SET_MODE is marked sent");
    }

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
        droneManager.GetDrone("Drone001") != nullptr,
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
