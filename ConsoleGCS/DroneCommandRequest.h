#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <vector>

enum class DroneCommandType
{
    Arm,
    Disarm,
    Takeoff,
    Land,
    RTL,
    SetMode,
    HoverTest
};

const char* DroneCommandTypeName(DroneCommandType type);

struct DroneCommandParameters
{
    float takeoffAltitudeMeters = 0.0f;
    // Reserved for Phase 5.3. Phase 5.2B transports these values only.
    float hoverAltitudeMeters = 0.0f;
    float hoverDurationSeconds = 0.0f;
    int customMode = -1;
    bool startHoverTest = true;
};

// ConsoleGCS command-domain model. It is deliberately independent of UE and
// of MAVLink command IDs so additional targets can be introduced later.
struct DroneCommandRequest
{
    std::uint64_t commandId = 0;
    DroneCommandType commandType = DroneCommandType::Arm;
    std::vector<std::string> targetDroneIds;
    DroneCommandParameters parameters;
    std::chrono::system_clock::time_point timestamp{};
    std::string source;
};
