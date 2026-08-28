/**
 * @brief 定义与 UE/MAVLink 解耦的 Console 领域命令模型。
 *
 * 分类：Production。
 * 输入：DSP-GCS 命令 DTO；输出：CommandManager/DroneSession 可执行请求。
 * 支持 Arm、Disarm、Takeoff、Land、RTL、SetMode 和应用级 HoverTest。
 */
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
    HoverTest,
    SetLedColor,
    TimeStart,
    TimeStop
};

// ArduPilot Copter custom_mode values used by the current GCS mode controls.
// These are intentionally kept separate from PX4 main/sub-mode encodings.
namespace ArduCopterCustomMode
{
    inline constexpr int Stabilize = 0;
    inline constexpr int Auto = 3;
    inline constexpr int Guided = 4;

    inline bool IsSupported(const int mode)
    {
        return mode == Stabilize || mode == Auto || mode == Guided;
    }

    inline const char* Name(const int mode)
    {
        switch (mode)
        {
        case Stabilize: return "STABILIZE";
        case Auto: return "AUTO";
        case Guided: return "GUIDED";
        default: return "UNKNOWN";
        }
    }
}

const char* DroneCommandTypeName(DroneCommandType type);

struct DroneCommandParameters
{
    float takeoffAltitudeMeters = 0.0f;
    // Reserved for Phase 5.3. Phase 5.2B transports these values only.
    float hoverAltitudeMeters = 0.0f;
    float hoverDurationSeconds = 0.0f;
    int customMode = -1;
    bool startHoverTest = true;
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t timeControlMask = 1;
    std::uint8_t scheduledHour = 0;
    std::uint8_t scheduledMinute = 0;
    std::uint8_t scheduledSecond = 0;
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
