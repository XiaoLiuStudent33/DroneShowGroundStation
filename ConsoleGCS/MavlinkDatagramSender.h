#pragma once

#include <cstdint>
#include <string>

// Sending boundary used by discovered DroneSessions. The concrete endpoint
// owns the bound UDP socket; a Session only supplies its discovered target.
class IMavlinkDatagramSender
{
public:
    virtual ~IMavlinkDatagramSender() = default;

    virtual bool SendTo(
        const std::string& targetIp,
        std::uint16_t targetPort,
        const std::uint8_t* data,
        int length) = 0;
};
