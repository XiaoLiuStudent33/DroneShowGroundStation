#include "../CommandManager.h"
#include "../DroneManager.h"
#include "../SafetyConfig.h"

#include <chrono>
#include <iostream>
#include <string>

namespace
{
    bool Check(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << message << '\n';
        }

        return condition;
    }
}

int main()
{
    if (!SafetyConfig::SAFE_TEST_MODE)
    {
        std::cout
            << "CommandManager tests skipped: SAFE_TEST_MODE is disabled\n";
        return 0;
    }

    DroneManager droneManager;
    DroneSessionConfig config;
    config.droneId = "Drone001";

    bool passed = Check(
        droneManager.AddDrone(config),
        "single DroneSession is registered");
    passed &= Check(
        droneManager.GetDrone("Drone001") != nullptr,
        "registered DroneID is queryable");
    passed &= Check(
        droneManager.GetAllDrones().size() == 1,
        "GetAllDrones returns the single session");

    CommandManager commandManager(droneManager);
    const std::uint64_t armCommandId =
        commandManager.HandleConsoleKey('A', "Drone001");
    const auto completed = commandManager.ConsumeCompletedCommands();

    passed &= Check(armCommandId != 0, "keyboard creates a CommandID");
    passed &= Check(completed.size() == 1, "SAFE_TEST command completes locally");

    if (!completed.empty())
    {
        const ManagedDroneCommand& command = completed.front();
        passed &= Check(
            command.request.commandId == armCommandId,
            "CommandID is preserved");
        passed &= Check(
            command.request.commandType == DroneCommandType::Arm,
            "keyboard A maps to ARM");
        passed &= Check(
            command.request.targetDroneIds.size() == 1 &&
            command.request.targetDroneIds.front() == "Drone001",
            "single target DroneID is preserved");
        passed &= Check(
            command.request.source == "Keyboard",
            "command source is recorded");
        passed &= Check(
            command.request.timestamp !=
                std::chrono::system_clock::time_point{},
            "command timestamp is recorded");
        passed &= Check(
            command.targetResults.size() == 1 &&
            command.targetResults.front().state ==
                DroneCommandTargetState::Simulated,
            "SAFE_TEST_MODE never dispatches a real ARM command");
    }

    passed &= Check(
        commandManager.GetPendingCommands().empty(),
        "simulated command does not remain pending");

    if (passed)
    {
        std::cout << "CommandManager single-drone SAFE_TEST tests passed\n";
        return 0;
    }

    return 1;
}
