#pragma once

#include <chrono>
#include <cstdint>
#include <string>

class DroneSession;

struct DiscoveredDroneIdentity
{
    // The remote UDP endpoint is the unique aircraft identity. MAVLink IDs
    // remain routing metadata for target_system / target_component only.
    std::uint8_t systemId = 0;
    std::uint8_t componentId = 0;
    std::string sourceIp;
    std::uint16_t sourcePort = 0;
    std::chrono::steady_clock::time_point observedAt{};

    std::string EndpointKey() const
    {
        return sourceIp + ":" + std::to_string(sourcePort);
    }
};

enum class DroneRegistrationStatus
{
    Created,
    Refreshed,
    DroneIdentityConflict,
    Rejected
};

struct DroneRegistrationResult
{
    DroneRegistrationStatus status = DroneRegistrationStatus::Rejected;
    std::string droneId;
    DroneSession* session = nullptr;
    std::string error;
};
