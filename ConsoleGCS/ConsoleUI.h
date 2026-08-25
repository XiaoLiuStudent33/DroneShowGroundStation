#pragma once

/**
 * @brief Console presentation boundary for the backend service.
 *
 * Producers only publish immutable fleet snapshots or complete log records.
 * A single renderer thread owns every physical stdout/cursor write.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "FlightData.h"
#include "HoverTest.h"

struct ConsoleDroneStatus
{
    std::string droneId;
    bool online = false;
};

enum class ConsoleOutputMode
{
    Auto,
    Dashboard,
    Linear
};

enum class ConsoleOutputHandleKind
{
    Console,
    Pipe,
    Disk,
    Invalid
};

enum class ConsoleLogSeverity
{
    Info,
    Error,
    Terminal
};

struct ConsoleTargetState
{
    ConsoleOutputHandleKind handleKind = ConsoleOutputHandleKind::Invalid;
    bool geometryValid = false;
    short width = 0;
    short height = 0;
};

/**
 * Test seam and physical-output abstraction. Only the renderer thread calls
 * WriteDashboard/WriteLinear/Restore. Inspect is read-only.
 */
class IConsoleRenderTarget
{
public:
    virtual ~IConsoleRenderTarget() = default;

    virtual ConsoleTargetState Inspect() = 0;
    virtual bool WriteDashboard(const std::vector<std::string>& lines) = 0;
    virtual bool WriteLinear(const std::string& line) = 0;
    virtual void Restore() = 0;
};

struct ConsoleUIStats
{
    std::uint64_t submittedLogCount = 0;
    std::uint64_t renderedLogCount = 0;
    std::uint64_t droppedLogCount = 0;
    std::uint64_t droppedCriticalLogCount = 0;
    std::uint64_t rejectedAfterShutdownCount = 0;
    std::size_t pendingLogCount = 0;
    std::size_t queueCapacity = 0;
    ConsoleOutputMode activeMode = ConsoleOutputMode::Linear;
    std::thread::id rendererThreadId;
};

class ConsoleUI
{
public:
    static void Init();
    static void Shutdown();

    static void UpdateFleet(const std::vector<ConsoleDroneStatus>& drones);
    static void Update(const FlightData& data, const HoverTest& test);
    static void Update(
        const std::string& droneId,
        const FlightData& data,
        const HoverTest& test);

    static void AddLog(const std::string& text);
    static void AddLog(
        const std::string& category,
        const std::string& text,
        ConsoleLogSeverity severity = ConsoleLogSeverity::Info);
    static void AddErrorLog(const std::string& text);
    static void AddTerminalLog(const std::string& text);

    // Console-only test controls. They must be configured while stopped.
    static bool ConfigureForTesting(
        std::shared_ptr<IConsoleRenderTarget> target,
        ConsoleOutputMode mode,
        std::size_t queueCapacity = 1024,
        std::chrono::milliseconds renderInterval =
            std::chrono::milliseconds(10));
    static bool WaitUntilIdle(std::chrono::milliseconds timeout);
    static ConsoleUIStats GetStats();
    static void ResetTestingConfiguration();

private:
    ConsoleUI() = delete;
};
