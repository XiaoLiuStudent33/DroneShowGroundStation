/**
 * @brief 实现多机命令目标解析、提交、结果轮询和 HoverTest 协调。
 *
 * 分类：Production。
 * 输入：DroneCommandRequest；输出：可经 DSP-GCS 返回 UE 的领域命令结果。
 */
#include "CommandManager.h"

#include "ConsoleUI.h"
#include "DroneManager.h"
#include "DroneSession.h"
#include "SafetyConfig.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <utility>

CommandManager::CommandManager(DroneManager& manager)
    : droneManager(manager)
{
}

std::uint64_t CommandManager::Submit(
    DroneCommandType commandType,
    const std::vector<std::string>& targetDroneIds,
    const DroneCommandParameters& parameters,
    const std::string& source)
{
    DroneCommandRequest request;
    request.commandId = nextCommandId++;
    request.commandType = commandType;
    request.targetDroneIds = targetDroneIds;
    request.parameters = parameters;
    request.timestamp = std::chrono::system_clock::now();
    request.source = source;
    Submit(request);
    return request.commandId;
}

bool CommandManager::Submit(const DroneCommandRequest& request)
{
    if (request.commandId == 0 ||
        request.targetDroneIds.empty() ||
        request.source.empty() ||
        knownCommandIds.find(request.commandId) != knownCommandIds.end())
    {
        ConsoleUI::AddLog("Command rejected: invalid or duplicate request");
        return false;
    }

    ManagedDroneCommand managed;
    managed.request = request;
    knownCommandIds.insert(request.commandId);
    nextCommandId = (std::max)(nextCommandId, request.commandId + 1);
    bool allAccepted = true;

    for (const std::string& droneId : request.targetDroneIds)
    {
        DroneCommandTargetResult targetResult;
        targetResult.droneId = droneId;
        DroneSession* session = droneManager.GetDrone(droneId);

        if (session == nullptr)
        {
            targetResult.state = DroneCommandTargetState::Rejected;
            targetResult.detail = "unknown DroneID";
            managed.targetResults.push_back(targetResult);
            allAccepted = false;
            continue;
        }

        // Each target receives an independent task object. The top-level
        // CommandID remains the batch correlation ID, while every Session owns
        // its own dispatcher queue, endpoint, retry state, and ACK lifecycle.
        DroneCommandRequest targetRequest = request;
        targetRequest.targetDroneIds = { droneId };
        const DroneSessionCommandSubmission submission =
            session->SubmitCommand(targetRequest);

        if (!submission.accepted)
        {
            targetResult.state = DroneCommandTargetState::Rejected;
            targetResult.detail = submission.error;
            managed.targetResults.push_back(targetResult);
            allAccepted = false;
            continue;
        }

        if (submission.simulated)
        {
            targetResult.state = DroneCommandTargetState::Simulated;
            targetResult.detail = submission.detail;
        }
        else if (submission.completedImmediately)
        {
            targetResult.state = DroneCommandTargetState::Accepted;
            targetResult.detail = submission.detail;
        }
        else
        {
            targetResult.state = DroneCommandTargetState::Pending;

            for (const std::uint64_t dispatchRequestId :
                submission.dispatchRequestIds)
            {
                DispatchOwner owner;
                owner.commandId = request.commandId;
                owner.droneId = droneId;
                dispatchOwners.emplace(
                    MakeDispatchKey(droneId, dispatchRequestId),
                    std::move(owner));
            }
        }

        managed.targetResults.push_back(targetResult);
    }

    pendingCommands.emplace(request.commandId, std::move(managed));
    CompleteIfTerminal(request.commandId);
    return allAccepted;
}

void CommandManager::Tick()
{
    const std::vector<DroneSession*> sessions =
        droneManager.GetAllDrones();
    std::vector<std::uint64_t> touchedCommands;

    for (DroneSession* session : sessions)
    {
        if (session == nullptr)
        {
            continue;
        }

        const std::vector<CommandDispatchResult> results =
            session->ConsumeCommandResults();

        for (const CommandDispatchResult& result : results)
        {
            const std::string key = MakeDispatchKey(
                session->GetDroneId(),
                result.requestId);
            const auto ownerItem = dispatchOwners.find(key);

            if (ownerItem == dispatchOwners.end())
            {
                // Internal hover-mission commands are intentionally owned by
                // FlightControl rather than by a top-level CommandRequest.
                continue;
            }

            const DispatchOwner owner = ownerItem->second;
            dispatchOwners.erase(ownerItem);
            const auto commandItem = pendingCommands.find(owner.commandId);

            if (commandItem == pendingCommands.end())
            {
                continue;
            }

            auto targetItem = std::find_if(
                commandItem->second.targetResults.begin(),
                commandItem->second.targetResults.end(),
                [&owner](const DroneCommandTargetResult& target)
                {
                    return target.droneId == owner.droneId;
                });

            if (targetItem == commandItem->second.targetResults.end())
            {
                continue;
            }

            targetItem->state = MapOutcome(result.outcome);
            targetItem->ackResult = result.ackResult;
            targetItem->attempts = result.attempts;
            targetItem->detail = CommandOutcomeName(result.outcome);
            touchedCommands.push_back(owner.commandId);
        }
    }

    std::sort(touchedCommands.begin(), touchedCommands.end());
    touchedCommands.erase(
        std::unique(touchedCommands.begin(), touchedCommands.end()),
        touchedCommands.end());

    for (const std::uint64_t commandId : touchedCommands)
    {
        CompleteIfTerminal(commandId);
    }
}

std::vector<ManagedDroneCommand> CommandManager::GetPendingCommands() const
{
    std::vector<ManagedDroneCommand> result;
    result.reserve(pendingCommands.size());

    for (const auto& item : pendingCommands)
    {
        result.push_back(item.second);
    }

    return result;
}

std::vector<ManagedDroneCommand> CommandManager::ConsumeCompletedCommands()
{
    std::vector<ManagedDroneCommand> result;
    result.swap(completedCommands);
    return result;
}

std::uint64_t CommandManager::HandleConsoleKey(
    int keyValue,
    const std::string& targetDroneId)
{
    const int key = keyValue == 27
        ? keyValue
        : std::toupper(static_cast<unsigned char>(keyValue));
    DroneCommandType commandType;
    DroneCommandParameters parameters;

    switch (key)
    {
    case 'A':
        commandType = DroneCommandType::Arm;
        break;
    case 'D':
        commandType = DroneCommandType::Disarm;
        break;
    case 'G':
        commandType = DroneCommandType::SetMode;
        parameters.customMode = 4;
        break;
    case 'T':
        commandType = DroneCommandType::Takeoff;
        parameters.takeoffAltitudeMeters =
            SafetyConfig::DEFAULT_TAKEOFF_ALTITUDE_METERS;
        break;
    case 'R':
        commandType = DroneCommandType::RTL;
        break;
    case 'L':
        commandType = DroneCommandType::Land;
        break;
    case 'H':
        commandType = DroneCommandType::HoverTest;
        parameters.startHoverTest = true;
        break;
    case 'X':
    case 27:
        commandType = DroneCommandType::HoverTest;
        parameters.startHoverTest = false;
        break;
    default:
        return 0;
    }

    return Submit(
        commandType,
        { targetDroneId },
        parameters,
        "Keyboard");
}

std::string CommandManager::MakeDispatchKey(
    const std::string& droneId,
    std::uint64_t dispatchRequestId)
{
    return droneId + "#" + std::to_string(dispatchRequestId);
}

bool CommandManager::IsTerminal(DroneCommandTargetState state)
{
    return state != DroneCommandTargetState::Pending;
}

DroneCommandTargetState CommandManager::MapOutcome(
    CommandOutcome outcome)
{
    switch (outcome)
    {
    case CommandOutcome::Sent:
        return DroneCommandTargetState::Sent;
    case CommandOutcome::Accepted:
        return DroneCommandTargetState::Accepted;
    case CommandOutcome::Rejected:
        return DroneCommandTargetState::Rejected;
    case CommandOutcome::Timeout:
        return DroneCommandTargetState::Timeout;
    case CommandOutcome::SendFailed:
    case CommandOutcome::Superseded:
    case CommandOutcome::QueueFull:
        return DroneCommandTargetState::Failed;
    }

    return DroneCommandTargetState::Failed;
}

void CommandManager::CompleteIfTerminal(std::uint64_t commandId)
{
    const auto item = pendingCommands.find(commandId);

    if (item == pendingCommands.end())
    {
        return;
    }

    const bool complete = std::all_of(
        item->second.targetResults.begin(),
        item->second.targetResults.end(),
        [](const DroneCommandTargetResult& target)
        {
            return IsTerminal(target.state);
        });

    if (!complete)
    {
        return;
    }

    completedCommands.push_back(std::move(item->second));
    pendingCommands.erase(item);
}
