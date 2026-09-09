/**
 * @brief 管理单个 DroneSession 的 MAVLink 命令队列、重试和 ACK 状态。
 *
 * 分类：Production。
 * 输入：领域动作和目标 MAVLink 路由信息；输出：MavlinkCommand 调用及完成结果。
 * 关键约束：ACK 必须匹配当前 Session、目标 sysid 和期望 MAV_CMD。
 */
#pragma once

#include "UdpClient.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

extern "C"
{
#include "mdhk/mavlink.h"
}

enum class CommandOutcome
{
    Sent,
    Accepted,
    Rejected,
    Timeout,
    SendFailed,
    Superseded,
    QueueFull
};

struct CommandDispatchResult
{
    std::uint64_t requestId = 0;
    std::uint16_t command = 0;
    CommandOutcome outcome = CommandOutcome::SendFailed;
    std::uint8_t ackResult = 0;
    int attempts = 0;
    bool modeConfirmationRequired = false;
    std::uint8_t expectedCustomMode = 0;
};

class CommandDispatcher
{
public:
    explicit CommandDispatcher(UdpClient& udp);

    std::uint64_t QueueArm(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueDisarm(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueGuided(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueSetMode(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t customMode);

    std::uint64_t QueueTakeoff(
        std::uint8_t system,
        std::uint8_t component,
        float altitude);

    std::uint64_t QueueLand(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueRTL(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueMissionPause(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueMissionForward(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueMissionBackward(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueLoiter(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueSetLedColor(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t red,
        std::uint8_t green,
        std::uint8_t blue);

    std::uint64_t QueueTimeControl(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t control,
        std::uint8_t controlMask,
        std::uint8_t hour,
        std::uint8_t minute,
        std::uint8_t second);

    std::uint64_t QueueExecuteDanceStep(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueEndDanceStep(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t controlMask);

    std::uint64_t QueueChangeSpeed(
        std::uint8_t system,
        std::uint8_t component,
        float metersPerSecond);

    /** Relay 0 is pulsed for one second and is always reset to zero. */
    std::uint64_t QueueTriggerSmoke(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueStopTelemetry(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueResumeTelemetry(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueSetLedOutputDisabled(
        std::uint8_t system,
        std::uint8_t component,
        bool disabled);

    std::uint64_t QueuePowerControl(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t autopilotAction,
        bool safetyPriority);

    /** Legacy MDHK hardware power-off contract: relay 1 latched high. */
    std::uint64_t QueueShutdown(
        std::uint8_t system,
        std::uint8_t component);

    void Tick(
        std::chrono::steady_clock::time_point now);

    void HandleAck(
        const mavlink_message_t& message);

    void HandleHeartbeat(
        const mavlink_message_t& message);

    /** Completes ResumeTelemetry only after a fresh MDHK_STATUS #187 arrives. */
    void HandleTelemetryMessage(std::uint32_t messageId);

    std::vector<CommandDispatchResult> ConsumeResults();

private:
    enum class Action
    {
        Arm,
        Disarm,
        SetMode,
        Takeoff,
        Land,
        RTL,
        MissionPause,
        MissionForward,
        MissionBackward,
        Loiter,
        SetLedColor,
        TimeControl,
        ExecuteDanceStep,
        EndDanceStep,
        ChangeSpeed,
        TriggerSmoke,
        TelemetryInterval,
        SetLedOutputDisabled,
        PowerControl,
        ShutdownRelay
    };

    struct Request
    {
        std::uint64_t id = 0;
        Action action = Action::Arm;
        std::uint8_t system = 0;
        std::uint8_t component = 0;
        std::uint16_t expectedAckCommand = 0;
        float altitude = 0.0f;
        std::uint8_t customMode = 0;
        std::uint8_t red = 0;
        std::uint8_t green = 0;
        std::uint8_t blue = 0;
        std::uint8_t timeControl = 0;
        std::uint8_t timeControlMask = 1;
        std::uint8_t scheduledHour = 0;
        std::uint8_t scheduledMinute = 0;
        std::uint8_t scheduledSecond = 0;
        float advancedValue = 0.0f;
        std::vector<std::uint32_t> messageIds;
        std::size_t messageIndex = 0;
        std::int32_t messageIntervalMicroseconds = 0;
        std::uint8_t relayStep = 0;
        bool waitForTelemetryConfirmation = false;
        bool waitingForTelemetryConfirmation = false;
        int attempts = 0;
        int totalAttempts = 0;
        bool sent = false;
        bool waitForModeConfirmation = false;
        std::chrono::steady_clock::time_point sentAt{};
        std::chrono::steady_clock::time_point notBefore{};
    };

    std::uint64_t Queue(
        Action action,
        std::uint8_t system,
        std::uint8_t component,
        std::uint16_t expectedAckCommand,
        float altitude,
        bool safetyPriority,
        std::uint8_t customMode = 0,
        std::uint8_t red = 0,
        std::uint8_t green = 0,
        std::uint8_t blue = 0,
        bool waitForModeConfirmation = false,
        std::uint8_t timeControl = 0,
        std::uint8_t timeControlMask = 1,
        std::uint8_t scheduledHour = 0,
        std::uint8_t scheduledMinute = 0,
        std::uint8_t scheduledSecond = 0,
        float advancedValue = 0.0f);

    std::uint64_t QueueTelemetryIntervals(
        std::uint8_t system,
        std::uint8_t component,
        std::int32_t intervalMicroseconds,
        bool verifyMdhkStatus);

    bool Send(const Request& request);

    void CompleteLocked(
        const Request& request,
        CommandOutcome outcome,
        std::uint8_t ackResult = 0);

    void CancelOutstandingLocked();

private:
    UdpClient& udp;

    std::mutex mutex;
    std::deque<Request> queued;
    std::optional<Request> pending;
    std::deque<CommandDispatchResult> completed;
    std::uint64_t nextRequestId = 1;

    static constexpr std::size_t MaxQueueSize = 32;
    static constexpr int MaxRetries = 1;
    static constexpr std::chrono::milliseconds AckTimeout{ 3000 };
    static constexpr std::chrono::milliseconds SmokePulseDuration{ 1000 };
};

const char* CommandOutcomeName(CommandOutcome outcome);

/** Human-readable MAV_RESULT name for COMMAND_ACK diagnostics. */
const char* MavResultName(std::uint8_t result);
