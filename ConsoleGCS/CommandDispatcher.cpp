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
        customMode,
        0,
        0,
        0,
        true);
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
    constexpr std::uint8_t RtlCustomMode = 6;
    return Queue(
        Action::RTL,
        system,
        component,
        0,
        0.0f,
        true,
        RtlCustomMode,
        0,
        0,
        0,
        true);
}

std::uint64_t CommandDispatcher::QueueMissionPause(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::MissionPause,
        system,
        component,
        MAV_CMD_NAV_LOITER_UNLIM,
        0.0f,
        false);
}

std::uint64_t CommandDispatcher::QueueMissionForward(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::MissionForward,
        system,
        component,
        MAV_CMD_NAV_RETURN_TO_LAUNCH,
        0.0f,
        false);
}

std::uint64_t CommandDispatcher::QueueMissionBackward(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::MissionBackward,
        system,
        component,
        MAV_CMD_NAV_RETURN_TO_LAUNCH,
        0.0f,
        false);
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

std::uint64_t CommandDispatcher::QueueTimeControl(
    std::uint8_t system,
    std::uint8_t component,
    std::uint8_t control,
    std::uint8_t controlMask,
    std::uint8_t hour,
    std::uint8_t minute,
    std::uint8_t second)
{
    return Queue(
        Action::TimeControl,
        system,
        component,
        0,
        0.0f,
        control == 0,
        0,
        0,
        0,
        0,
        false,
        control,
        controlMask,
        hour,
        minute,
        second);
}

std::uint64_t CommandDispatcher::QueueExecuteDanceStep(
    std::uint8_t system,
    std::uint8_t component)
{
    return Queue(
        Action::ExecuteDanceStep,
        system,
        component,
        0,
        0.0f,
        false);
}

std::uint64_t CommandDispatcher::QueueEndDanceStep(
    std::uint8_t system,
    std::uint8_t component,
    std::uint8_t controlMask)
{
    return Queue(
        Action::EndDanceStep,
        system,
        component,
        0,
        0.0f,
        true,
        0,
        0,
        0,
        0,
        false,
        0,
        controlMask);
}

std::uint64_t CommandDispatcher::QueueChangeSpeed(
    const std::uint8_t system,
    const std::uint8_t component,
    const float metersPerSecond)
{
    return Queue(Action::ChangeSpeed, system, component, MAV_CMD_USER_2,
        0.0f, false, 0, 0, 0, 0, false, 0, 1, 0, 0, 0,
        metersPerSecond);
}

std::uint64_t CommandDispatcher::QueueTriggerSmoke(
    const std::uint8_t system,
    const std::uint8_t component)
{
    std::lock_guard<std::mutex> lock(mutex);
    Request request;
    request.id = nextRequestId++;
    request.action = Action::TriggerSmoke;
    request.system = system;
    request.component = component;
    request.expectedAckCommand = MAV_CMD_DO_SET_RELAY;
    if (system == 0 || component == 0 || queued.size() >= MaxQueueSize)
    {
        CompleteLocked(request, CommandOutcome::QueueFull);
    }
    else
    {
        queued.push_back(request);
    }
    return request.id;
}

std::uint64_t CommandDispatcher::QueueStopTelemetry(
    const std::uint8_t system,
    const std::uint8_t component)
{
    return QueueTelemetryIntervals(system, component, -1, false);
}

std::uint64_t CommandDispatcher::QueueResumeTelemetry(
    const std::uint8_t system,
    const std::uint8_t component)
{
    return QueueTelemetryIntervals(system, component, 1000000, true);
}

std::uint64_t CommandDispatcher::QueueSetLedOutputDisabled(
    const std::uint8_t system,
    const std::uint8_t component,
    const bool disabled)
{
    return Queue(Action::SetLedOutputDisabled, system, component, MAV_CMD_USER_3,
        0.0f, false, 0, 0, 0, 0, false, 0, 1, 0, 0, 0,
        disabled ? 1.0f : 0.0f);
}

std::uint64_t CommandDispatcher::QueuePowerControl(
    const std::uint8_t system,
    const std::uint8_t component,
    const std::uint8_t autopilotAction,
    const bool safetyPriority)
{
    return Queue(Action::PowerControl, system, component,
        MAV_CMD_PREFLIGHT_REBOOT_SHUTDOWN, 0.0f, safetyPriority,
        0, 0, 0, 0, false, 0, 1, 0, 0, 0,
        static_cast<float>(autopilotAction));
}

std::uint64_t CommandDispatcher::QueueShutdown(
    const std::uint8_t system,
    const std::uint8_t component)
{
    return Queue(Action::ShutdownRelay, system, component,
        MAV_CMD_DO_SET_RELAY, 0.0f, true);
}

std::uint64_t CommandDispatcher::QueueTelemetryIntervals(
    const std::uint8_t system,
    const std::uint8_t component,
    const std::int32_t intervalMicroseconds,
    const bool verifyMdhkStatus)
{
    std::lock_guard<std::mutex> lock(mutex);
    Request request;
    request.id = nextRequestId++;
    request.action = Action::TelemetryInterval;
    request.system = system;
    request.component = component;
    request.expectedAckCommand = MAV_CMD_SET_MESSAGE_INTERVAL;
    // Preserve heartbeat (#0) and COMMAND_ACK (#77). #187 is last so a
    // post-ACK sample proves the custom telemetry stream really resumed.
    request.messageIds = { 1, 24, 33, 30, 187 };
    request.messageIntervalMicroseconds = intervalMicroseconds;
    request.waitForTelemetryConfirmation = verifyMdhkStatus;
    if (system == 0 || component == 0 || queued.size() >= MaxQueueSize)
    {
        CompleteLocked(request, CommandOutcome::QueueFull);
    }
    else
    {
        queued.push_back(request);
    }
    return request.id;
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
    std::uint8_t blue,
    bool waitForModeConfirmation,
    std::uint8_t timeControl,
    std::uint8_t timeControlMask,
    std::uint8_t scheduledHour,
    std::uint8_t scheduledMinute,
    std::uint8_t scheduledSecond,
    float advancedValue)
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
    request.waitForModeConfirmation = waitForModeConfirmation;
    request.timeControl = timeControl;
    request.timeControlMask = timeControlMask;
    request.scheduledHour = scheduledHour;
    request.scheduledMinute = scheduledMinute;
    request.scheduledSecond = scheduledSecond;
    request.advancedValue = advancedValue;

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
        pending->waitingForTelemetryConfirmation &&
        now - pending->sentAt >= AckTimeout)
    {
        CompleteLocked(*pending, CommandOutcome::Timeout);
        pending.reset();
    }

    if (pending &&
        pending->sent &&
        !pending->waitingForTelemetryConfirmation &&
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

    if (!pending || pending->sent ||
        (pending->notBefore.time_since_epoch().count() != 0 && now < pending->notBefore))
    {
        return;
    }

    pending->sent = true;
    pending->sentAt = now;
    ++pending->attempts;
    ++pending->totalAttempts;

    const bool sendSucceeded = Send(*pending);

    if (!sendSucceeded)
    {
        if (pending->action == Action::TriggerSmoke &&
            pending->relayStep == 1 && pending->attempts <= MaxRetries)
        {
            pending->sent = false;
            pending->notBefore = now + SmokePulseDuration;
            return;
        }
        CompleteLocked(
            *pending,
            CommandOutcome::SendFailed);
        pending.reset();
        return;
    }

    // A relay pulse must not depend on receiving the ON command ACK. UDP may
    // deliver the command while its ACK is lost; waiting for that ACK before
    // scheduling relay=0 could leave the smoke output latched indefinitely.
    // Once the ON datagram is sent, always advance to the OFF step and retry
    // that safety-critical reset until it is acknowledged or times out.
    if (pending->action == Action::TriggerSmoke && pending->relayStep == 0)
    {
        pending->relayStep = 1;
        pending->attempts = 0;
        pending->sent = false;
        pending->notBefore = now + SmokePulseDuration;
        return;
    }

    if (pending->expectedAckCommand == 0 &&
        !pending->waitForModeConfirmation)
    {
        CompleteLocked(
            *pending,
            CommandOutcome::Sent);
        pending.reset();
    }
}

void CommandDispatcher::HandleHeartbeat(
    const mavlink_message_t& message)
{
    if (message.msgid != MAVLINK_MSG_ID_HEARTBEAT)
    {
        return;
    }

    mavlink_heartbeat_t heartbeat{};
    mavlink_msg_heartbeat_decode(&message, &heartbeat);

    std::lock_guard<std::mutex> lock(mutex);
    if (!pending ||
        !pending->waitForModeConfirmation ||
        pending->system != message.sysid ||
        (heartbeat.base_mode & MAV_MODE_FLAG_CUSTOM_MODE_ENABLED) == 0 ||
        heartbeat.custom_mode != pending->customMode)
    {
        return;
    }

    CompleteLocked(
        *pending,
        CommandOutcome::Accepted);
    pending.reset();
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
        !pending->sent ||
        pending->waitForModeConfirmation ||
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

    if (ack.result == MAV_RESULT_ACCEPTED &&
        pending->action == Action::TelemetryInterval)
    {
        if (pending->messageIndex + 1 < pending->messageIds.size())
        {
            ++pending->messageIndex;
            pending->attempts = 0;
            pending->sent = false;
            return;
        }
        if (pending->waitForTelemetryConfirmation)
        {
            pending->waitingForTelemetryConfirmation = true;
            pending->sentAt = std::chrono::steady_clock::now();
            return;
        }
    }

    const CommandOutcome outcome = ack.result == MAV_RESULT_ACCEPTED
        ? CommandOutcome::Accepted : CommandOutcome::Rejected;

    CompleteLocked(
        *pending,
        outcome,
        ack.result);
    pending.reset();
}

void CommandDispatcher::HandleTelemetryMessage(const std::uint32_t messageId)
{
    if (messageId != 187)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (!pending || !pending->waitingForTelemetryConfirmation)
    {
        return;
    }
    CompleteLocked(*pending, CommandOutcome::Accepted);
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
        return MavlinkCommand::SetMode(
            udp,
            request.system,
            request.customMode);

    case Action::MissionPause:
        return MavlinkCommand::LegacyMissionPause(
            udp,
            request.system,
            request.component);

    case Action::MissionForward:
        return MavlinkCommand::LegacyMissionPlayback(
            udp,
            request.system,
            request.component,
            false);

    case Action::MissionBackward:
        return MavlinkCommand::LegacyMissionPlayback(
            udp,
            request.system,
            request.component,
            true);

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

    case Action::TimeControl:
        return MavlinkCommand::TimeControl(
            udp,
            request.timeControl,
            request.timeControlMask,
            request.scheduledHour,
            request.scheduledMinute,
            request.scheduledSecond);

    case Action::ExecuteDanceStep:
        return MavlinkCommand::ExecuteDanceStep(
            udp,
            request.system,
            request.component);

    case Action::EndDanceStep:
        return MavlinkCommand::EndDanceStep(
            udp,
            request.system,
            request.component,
            request.timeControlMask);

    case Action::ChangeSpeed:
        return MavlinkCommand::ChangeSpeed(
            udp, request.system, request.component, request.advancedValue);

    case Action::TriggerSmoke:
        return MavlinkCommand::SetRelay(
            udp, request.system, request.component, 0, request.relayStep == 0);

    case Action::TelemetryInterval:
        if (request.messageIndex >= request.messageIds.size())
        {
            return false;
        }
        return MavlinkCommand::SetMessageInterval(
            udp, request.system, request.component,
            request.messageIds[request.messageIndex],
            request.messageIntervalMicroseconds);

    case Action::SetLedOutputDisabled:
        return MavlinkCommand::SetLedOutputDisabled(
            udp, request.system, request.component, request.advancedValue != 0.0f);

    case Action::PowerControl:
        return MavlinkCommand::PowerControl(
            udp, request.system, request.component,
            static_cast<std::uint8_t>(request.advancedValue));

    case Action::ShutdownRelay:
        return MavlinkCommand::SetRelay(
            udp, request.system, request.component, 1, true);
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
    result.attempts = request.totalAttempts > 0
        ? request.totalAttempts
        : request.attempts;
    result.modeConfirmationRequired = request.waitForModeConfirmation;
    result.expectedCustomMode = request.customMode;

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

const char* MavResultName(const std::uint8_t result)
{
    switch (result)
    {
    case MAV_RESULT_ACCEPTED:
        return "MAV_RESULT_ACCEPTED";
    case MAV_RESULT_TEMPORARILY_REJECTED:
        return "MAV_RESULT_TEMPORARILY_REJECTED";
    case MAV_RESULT_DENIED:
        return "MAV_RESULT_DENIED";
    case MAV_RESULT_UNSUPPORTED:
        return "MAV_RESULT_UNSUPPORTED";
    case MAV_RESULT_FAILED:
        return "MAV_RESULT_FAILED";
    case MAV_RESULT_IN_PROGRESS:
        return "MAV_RESULT_IN_PROGRESS";
    case MAV_RESULT_CANCELLED:
        return "MAV_RESULT_CANCELLED";
    case MAV_RESULT_COMMAND_LONG_ONLY:
        return "MAV_RESULT_COMMAND_LONG_ONLY";
    case MAV_RESULT_COMMAND_INT_ONLY:
        return "MAV_RESULT_COMMAND_INT_ONLY";
    case MAV_RESULT_COMMAND_UNSUPPORTED_MAV_FRAME:
        return "MAV_RESULT_COMMAND_UNSUPPORTED_MAV_FRAME";
    default:
        return "MAV_RESULT_UNKNOWN";
    }
}
