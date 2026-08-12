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
#include "ardupilotmega/mavlink.h"
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

    std::uint64_t QueueLoiter(
        std::uint8_t system,
        std::uint8_t component);

    std::uint64_t QueueSetLedColor(
        std::uint8_t system,
        std::uint8_t component,
        std::uint8_t red,
        std::uint8_t green,
        std::uint8_t blue);

    void Tick(
        std::chrono::steady_clock::time_point now);

    void HandleAck(
        const mavlink_message_t& message);

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
        Loiter,
        SetLedColor
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
        int attempts = 0;
        bool sent = false;
        std::chrono::steady_clock::time_point sentAt{};
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
        std::uint8_t blue = 0);

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
};

const char* CommandOutcomeName(CommandOutcome outcome);
