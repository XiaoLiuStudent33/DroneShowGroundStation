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

    static std::string MakeEndpointKey(
        const std::string& sourceIp,
        std::uint16_t sourcePort);

    DroneManager& droneManager;
    MavlinkDiscoveryService& discoveryService;
    std::unordered_map<std::string, EndpointParserState> parserStates;
};
