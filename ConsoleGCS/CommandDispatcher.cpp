/**
 * @brief 实现单 Session 命令排队、发送、超时重试和 COMMAND_ACK 匹配。
 *
 * 分类：Production。
 * 调用链：DroneSession -> CommandDispatcher -> MavlinkCommand；#77 ACK 反向完成 pending request。
 * SYSID 只用于协议 ACK 匹配，不承担 Fleet 唯一身份职责。
 */
#include "CommandDispatcher.h"

#include "MavlinkCommand.h"

#include <utility>

CommandDispatcher::CommandDispatcher(UdpClient& udp)
    : udp(udp)
{
}

std::uint64_t CommandDispatcher::QueueArm(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::Arm,
        system,
        component,
        MAV_CMD_COMPONENT_ARM_DISARM,
        0.0f,
        false);
}

std::uint64_t CommandDispatcher::QueueDisarm(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::Disarm,
        system,
        component,
        MAV_CMD_COMPONENT_ARM_DISARM,
        0.0f,
        true);
}

std::uint64_t CommandDispatcher::QueueGuided(
    std::uint8_t system,
    std::uint8_t component)
{
    return QueueSetMode(system, component, 4);
}

std::uint64_t CommandDispatcher::QueueSetMode(
    std::uint8_t system,
    std::uint8_t component,
    std::uint8_t customMode)
{
    return Queue(
        Action::SetMode,
        system,
        component,
        0,
        0.0f,
        false,
        customMode);
}

std::uint64_t CommandDispatcher::QueueTakeoff(
    std::uint8_t system,
    std::uint8_t component,
    float altitude)
{
    return Queue(
        Action::Takeoff,
        system,
        component,
        MAV_CMD_NAV_TAKEOFF,
        altitude,
        false);
}

std::uint64_t CommandDispatcher::QueueLand(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::Land,
        system,
        component,
        MAV_CMD_NAV_LAND,
        0.0f,
        true);
}

std::uint64_t CommandDispatcher::QueueRTL(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::RTL,
        system,
        component,
        MAV_CMD_NAV_RETURN_TO_LAUNCH,
        0.0f,
        true);
}

std::uint64_t CommandDispatcher::QueueLoiter(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::Loiter,
        system,
        component,
        0,
        0.0f,
        false);
}

std::uint64_t CommandDispatcher::QueueSetLedColor(
    std::uint8_t system,
    std::uint8_t component,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    return Queue(
        Action::SetLedColor,
        system,
        component,
        0,
        0.0f,
        false,
        0,
        red,
        green,
        blue);
}

std::uint64_t CommandDispatcher::Queue(
    Action action,
    std::uint8_t system,
    std::uint8_t component,
    std::uint16_t expectedAckCommand,
    float altitude,
    bool safetyPriority,
    std::uint8_t customMode,
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    std::lock_guard<std::mutex> lock(mutex);

    Request request;
    request.id = nextRequestId++;
    request.action = action;
    request.system = system;
    request.component = component;
    request.expectedAckCommand = expectedAckCommand;
    request.altitude = altitude;
    request.customMode = customMode;
    request.red = red;
    request.green = green;
    request.blue = blue;

    if (system == 0 ||
        component == 0 ||
        queued.size() >= MaxQueueSize)
    {
        CompleteLocked(
            request,
            CommandOutcome::QueueFull);
        return request.id;
    }

    if (safetyPriority)
    {
        CancelOutstandingLocked();
        queued.push_front(request);
    }
    else
    {
        queued.push_back(request);
    }

    return request.id;
}

void CommandDispatcher::Tick(
    std::chrono::steady_clock::time_point now)
{
    std::lock_guard<std::mutex> lock(mutex);

    if (pending &&
        pending->sent &&
        now - pending->sentAt >= AckTimeout)
    {
        if (pending->attempts > MaxRetries)
        {
            CompleteLocked(
                *pending,
                CommandOutcome::Timeout);
            pending.reset();
        }
        else
        {
            pending->sent = false;
        }
    }

    if (!pending && !queued.empty())
    {
        pending = queued.front();
        queued.pop_front();
    }

    if (!pending || pending->sent)
    {
        return;
    }

    pending->sent = true;
    pending->sentAt = now;
    ++pending->attempts;

    const bool sendSucceeded = Send(*pending);

    if (!sendSucceeded)
    {
        CompleteLocked(
            *pending,
            CommandOutcome::SendFailed);
        pending.reset();
        return;
    }

    if (pending->expectedAckCommand == 0)
    {
        CompleteLocked(
            *pending,
            CommandOutcome::Sent);
        pending.reset();
    }
}

void CommandDispatcher::HandleAck(
    const mavlink_message_t& message)
{
    if (message.msgid != MAVLINK_MSG_ID_COMMAND_ACK)
    {
        return;
    }

    mavlink_command_ack_t ack;
    mavlink_msg_command_ack_decode(
        &message,
        &ack);

    std::lock_guard<std::mutex> lock(mutex);

    // #77 ACK 必须属于本 Session 当前等待的目标 sysid，并携带期望的
    // MAV_CMD。这里的 SYSID 仅用于协议应答关联，不能替代 Endpoint Fleet 身份。
    if (!pending ||
        pending->system != message.sysid ||
        pending->expectedAckCommand != ack.command)
    {
        return;
    }

    if (ack.result == MAV_RESULT_IN_PROGRESS)
    {
        pending->sentAt =
            std::chrono::steady_clock::now();
        return;
    }

    const CommandOutcome outcome =
        ack.result == MAV_RESULT_ACCEPTED
        ? CommandOutcome::Accepted
        : CommandOutcome::Rejected;

    CompleteLocked(
        *pending,
        outcome,
        ack.result);
    pending.reset();
}

std::vector<CommandDispatchResult>
CommandDispatcher::ConsumeResults()
{
    std::lock_guard<std::mutex> lock(mutex);

    std::vector<CommandDispatchResult> results;
    results.reserve(completed.size());

    while (!completed.empty())
    {
        results.push_back(completed.front());
        completed.pop_front();
    }

    return results;
}

bool CommandDispatcher::Send(
    const Request& request)
{
    switch (request.action)
    {
    case Action::Arm:
        return MavlinkCommand::Arm(
            udp,
            request.system,
            request.component);

    case Action::Disarm:
        return MavlinkCommand::Disarm(
            udp,
            request.system,
            request.component);

    case Action::SetMode:
        return MavlinkCommand::SetMode(
            udp,
            request.system,
            request.customMode);

    case Action::Takeoff:
        return MavlinkCommand::Takeoff(
            udp,
            request.system,
            request.component,
            request.altitude);

    case Action::Land:
        return MavlinkCommand::Land(
            udp,
            request.system,
            request.component);

    case Action::RTL:
        return MavlinkCommand::RTL(
            udp,
            request.system,
            request.component);

    case Action::Loiter:
        return MavlinkCommand::Loiter(
            udp,
            request.system,
            request.component);

    case Action::SetLedColor:
        return MavlinkCommand::SetLedColor(
            udp,
            request.red,
            request.green,
            request.blue);
    }

    return false;
}

void CommandDispatcher::CompleteLocked(
    const Request& request,
    CommandOutcome outcome,
    std::uint8_t ackResult)
{
    CommandDispatchResult result;
    result.requestId = request.id;
    result.command = request.expectedAckCommand;
    result.outcome = outcome;
    result.ackResult = ackResult;
    result.attempts = request.attempts;

    completed.push_back(result);
}

void CommandDispatcher::CancelOutstandingLocked()
{
    if (pending)
    {
        CompleteLocked(
            *pending,
            CommandOutcome::Superseded);
        pending.reset();
    }

    while (!queued.empty())
    {
        CompleteLocked(
            queued.front(),
            CommandOutcome::Superseded);
        queued.pop_front();
    }
}

const char* CommandOutcomeName(
    CommandOutcome outcome)
{
    switch (outcome)
    {
    case CommandOutcome::Sent:
        return "SENT";
    case CommandOutcome::Accepted:
        return "ACCEPTED";
    case CommandOutcome::Rejected:
        return "REJECTED";
    case CommandOutcome::Timeout:
        return "TIMEOUT";
    case CommandOutcome::SendFailed:
        return "SEND_FAILED";
    case CommandOutcome::Superseded:
        return "SUPERSEDED";
    case CommandOutcome::QueueFull:
        return "QUEUE_FULL";
    }

    return "UNKNOWN";
}
