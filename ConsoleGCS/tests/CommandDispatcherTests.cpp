#define WIN32_LEAN_AND_MEAN

#include "../CommandDispatcher.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
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

    bool ReceiveMessage(
        SOCKET receiver,
        mavlink_message_t& message)
    {
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(1);

        mavlink_status_t status{};
        std::uint8_t buffer[2048];

        while (std::chrono::steady_clock::now() < deadline)
        {
            const int length = recv(
                receiver,
                reinterpret_cast<char*>(buffer),
                sizeof(buffer),
                0);

            if (length > 0)
            {
                for (int index = 0; index < length; ++index)
                {
                    if (mavlink_parse_char(
                        MAVLINK_COMM_1,
                        buffer[index],
                        &message,
                        &status))
                    {
                        return true;
                    }
                }
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(5));
        }

        return false;
    }
}

int main()
{
    WSADATA wsa{};

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        std::cerr << "FAILED: WSAStartup\n";
        return 1;
    }

    SOCKET receiver =
        socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (receiver == INVALID_SOCKET)
    {
        WSACleanup();
        return 1;
    }

    sockaddr_in receiverAddress{};
    receiverAddress.sin_family = AF_INET;
    receiverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    receiverAddress.sin_port = 0;

    if (bind(
        receiver,
        reinterpret_cast<sockaddr*>(&receiverAddress),
        sizeof(receiverAddress)) == SOCKET_ERROR)
    {
        closesocket(receiver);
        WSACleanup();
        return 1;
    }

    int receiverAddressLength = sizeof(receiverAddress);
    getsockname(
        receiver,
        reinterpret_cast<sockaddr*>(&receiverAddress),
        &receiverAddressLength);

    u_long nonBlocking = 1;
    ioctlsocket(
        receiver,
        FIONBIO,
        &nonBlocking);

    UdpClient sender;

    bool passed = Check(
        sender.Init(
            "127.0.0.1",
            ntohs(receiverAddress.sin_port),
            "127.0.0.1",
            0),
        "UDP sender initializes");

    CommandDispatcher dispatcher(sender);
    const auto now =
        std::chrono::steady_clock::now();

    dispatcher.QueueArm(1, 1);
    dispatcher.Tick(now);

    mavlink_message_t received{};
    passed &= Check(
        ReceiveMessage(receiver, received),
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
            ReceiveMessage(receiver, received),
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
        ReceiveMessage(receiver, received),
        "LAND packet is emitted");
    mavlink_msg_command_long_decode(
        &received,
        &command);
    passed &= Check(
        command.command == MAV_CMD_NAV_LAND,
        "priority packet is LAND");

    sender.Close();
    closesocket(receiver);
    WSACleanup();

    if (passed)
    {
        std::cout << "CommandDispatcher tests passed\n";
        return 0;
    }

    return 1;
}
