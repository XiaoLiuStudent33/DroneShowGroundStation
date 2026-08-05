#pragma once

#include "CommandDispatcher.h"
#include "DroneCommandRequest.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DroneManager;

enum class DroneCommandTargetState
{
    Pending,
    Sent,
    Accepted,
    Rejected,
    Timeout,
    Failed,
    Simulated
};

struct DroneCommandTargetResult
{
    std::string droneId;
    DroneCommandTargetState state = DroneCommandTargetState::Pending;
    std::uint8_t ackResult = 0;
    int attempts = 0;
    std::string detail;
};

struct ManagedDroneCommand
{
    DroneCommandRequest request;
    std::vector<DroneCommandTargetResult> targetResults;
};

class CommandManager
{
public:
    explicit CommandManager(DroneManager& droneManager);

    std::uint64_t Submit(
        DroneCommandType commandType,
        const std::vector<std::string>& targetDroneIds,
        const DroneCommandParameters& parameters,
        const std::string& source);

    bool Submit(const DroneCommandRequest& request);
    void Tick();

    std::vector<ManagedDroneCommand> GetPendingCommands() const;
    std::vector<ManagedDroneCommand> ConsumeCompletedCommands();

    // Keyboard is only an input adapter. It creates a CommandRequest and does
    // not invoke DroneSession, CommandDispatcher, or MAVLink directly.
    std::uint64_t HandleConsoleKey(
        int key,
        const std::string& targetDroneId);

private:
    struct DispatchOwner
    {
        std::uint64_t commandId = 0;
        std::string droneId;
    };

    static std::string MakeDispatchKey(
        const std::string& droneId,
        std::uint64_t dispatchRequestId);
    static bool IsTerminal(DroneCommandTargetState state);
    static DroneCommandTargetState MapOutcome(CommandOutcome outcome);

    void CompleteIfTerminal(std::uint64_t commandId);

private:
    DroneManager& droneManager;
    std::uint64_t nextCommandId = 1;
    std::unordered_map<std::uint64_t, ManagedDroneCommand> pendingCommands;
    std::unordered_set<std::uint64_t> knownCommandIds;
    std::unordered_map<std::string, DispatchOwner> dispatchOwners;
    std::vector<ManagedDroneCommand> completedCommands;
};
