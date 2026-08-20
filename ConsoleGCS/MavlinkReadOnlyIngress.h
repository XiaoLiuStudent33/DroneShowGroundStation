/**
 * @brief 将共享 UDP Datagram 按 Remote Endpoint 解码并路由到正确 DroneSession。
 *
 * 分类：Production；ReadOnly 表示发现入口不主动创造控制语义，并非整个 Session 禁止命令。
 * 输入：MavlinkUdpReceiver Datagram；输出：Discovery 刷新、Telemetry 应用和 COMMAND_ACK 路由。
 * 关键约束：每个 Endpoint 维护独立 MAVLink parser state，避免千机字节流互相污染。
 */
#pragma once

#include "MavlinkDiscoveryService.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

struct MavlinkIngressStats
{
    std::size_t decodedMessages = 0;
    std::size_t heartbeatsApplied = 0;
    std::size_t telemetryApplied = 0;
    std::size_t commandMessagesApplied = 0;
    std::size_t ftpMessagesApplied = 0;
    std::size_t missionMessagesApplied = 0;
    std::size_t identityConflicts = 0;
    std::size_t ignoredMessages = 0;
};

// Decodes frames from received datagrams. Telemetry remains read-only; command
// ACK messages are routed back to the owning Session without sending data.
class MavlinkReadOnlyIngress
{
public:
    MavlinkReadOnlyIngress(
        DroneManager& droneManager,
        MavlinkDiscoveryService& discoveryService);

    MavlinkIngressStats SubmitDatagram(
        const std::uint8_t* bytes,
        std::size_t byteCount,
        const std::string& sourceIp,
        std::uint16_t sourcePort);

private:
    struct EndpointParserState
    {
        mavlink_message_t messageBuffer{};
        mavlink_status_t parseStatus{};
    };

    DroneManager& droneManager;
    MavlinkDiscoveryService& discoveryService;
    std::unordered_map<std::string, EndpointParserState> parserStates;
};
