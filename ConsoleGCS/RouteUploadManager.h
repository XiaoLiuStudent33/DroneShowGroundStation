#pragma once

#include "ConsoleUploadJobAuthority.h"
#include "DroneManager.h"
#include "MissionPreparationWorker.h"
#include "MissionUploadTransaction.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct RouteUploadTargetMetrics
{
    std::string droneId;
    std::int32_t routeSlotIndex = -1;
    std::uint32_t missionItemCount = 0;
    std::uint64_t logicalPayloadBytes = 0;
    std::uint64_t wireBytesSent = 0;
    std::uint64_t missionMessagesSent = 0;
    std::uint64_t missionRequestsReceived = 0;
    std::uint64_t duplicateRequests = 0;
    std::uint64_t retryCount = 0;
    std::uint64_t prepareDurationMs = 0;
    std::uint64_t uploadDurationMs = 0;
    std::uint64_t verificationDurationMs = 0;
    std::uint64_t totalDurationMs = 0;
    std::string result;
    std::string safeDiagnostic;
};

struct RouteUploadJobMetrics
{
    std::size_t targetCount = 0;
    std::uint32_t effectiveMaxConcurrentUploads = 0;
    std::size_t succeeded = 0;
    std::size_t failed = 0;
    std::size_t cancelled = 0;
    std::uint64_t elapsedMs = 0;
    std::uint64_t logicalBytes = 0;
    std::uint64_t wireBytesSent = 0;
    std::uint64_t aggregateLogicalThroughput = 0;
    std::size_t peakActiveUploads = 0;
    std::size_t peakPreparedMissions = 0;
};

/** Process-level scheduler/executor; ConsoleUploadJobAuthority remains authoritative. */
class RouteUploadManager final
{
public:
    RouteUploadManager(
        ConsoleUploadJobAuthority& authority,
        DroneManager& droneManager,
        bool routeUploadEnabled);
    ~RouteUploadManager();

    RouteUploadManager(const RouteUploadManager&) = delete;
    RouteUploadManager& operator=(const RouteUploadManager&) = delete;

    void Tick(std::chrono::steady_clock::time_point now);
    void Shutdown();

    const RouteUploadJobMetrics& GetJobMetricsForTest() const { return jobMetrics_; }
    const std::vector<RouteUploadTargetMetrics>& GetTargetMetricsForTest() const
        { return targetMetrics_; }
    std::size_t GetActiveSlotCountForTest() const;
    std::size_t GetPreparedMissionCountForTest() const;
    std::size_t GetWorkerThreadCountForTest() const
        { return preparationWorker_.GetWorkerThreadCountForTest(); }
    std::size_t GetSocketCountForTest() const { return 0; }
    std::size_t GetTimerCountForTest() const { return 0; }

private:
    struct RuntimeTarget
    {
        UploadJobTargetMetadata metadata;
        dspgcs::UploadRouteStateDTO state;
        std::vector<dronegcs::route_mission::MissionItemIntV1> prepared;
        std::unique_ptr<MissionUploadTransaction> transaction;
        DroneSession* session = nullptr;
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point admittedAt{};
        std::chrono::steady_clock::time_point uploadStartedAt{};
        std::chrono::steady_clock::time_point verificationStartedAt{};
        RouteUploadTargetMetrics metrics;
    };

    struct RuntimeJob
    {
        UploadJobExecutionView view;
        std::vector<RuntimeTarget> targets;
        std::chrono::steady_clock::time_point startedAt{};
        std::uint64_t nextGeneration = 0;
    };

    bool AttachJob(const std::string& uploadJobId,
        std::chrono::steady_clock::time_point now);
    void DetachJob();
    void ProcessCancellation(std::chrono::steady_clock::time_point now);
    void ProcessPreparation(std::chrono::steady_clock::time_point now,
        dspgcs::UploadJobState jobState, std::uint32_t maxConcurrency);
    void ProcessTransactions(std::chrono::steady_clock::time_point now);
    void AdmitPreparation(std::chrono::steady_clock::time_point now,
        std::uint32_t maxConcurrency);
    bool StartPrepared(RuntimeTarget& target,
        std::chrono::steady_clock::time_point now);
    void FailTarget(RuntimeTarget& target,
        dspgcs::RouteUploadErrorCode code, const char* diagnostic,
        bool retryable, std::chrono::steady_clock::time_point now);
    void ApplyTarget(RuntimeTarget& target,
        std::chrono::steady_clock::time_point now);
    void FinalizeJobIfTerminal(dspgcs::UploadJobState state,
        std::chrono::steady_clock::time_point now);
    RuntimeTarget* FindRuntimeTarget(std::int32_t routeSlotIndex);
    std::uint64_t ElapsedMs(std::chrono::steady_clock::time_point now) const;
    std::uint64_t LogicalThroughput(std::chrono::steady_clock::time_point now) const;
    void RefreshMetrics(std::chrono::steady_clock::time_point now);

    ConsoleUploadJobAuthority& authority_;
    DroneManager& droneManager_;
    bool routeUploadEnabled_ = false;
    bool shuttingDown_ = false;
    MissionPreparationWorker preparationWorker_;
    std::optional<RuntimeJob> activeJob_;
    RouteUploadJobMetrics jobMetrics_;
    std::vector<RouteUploadTargetMetrics> targetMetrics_;
};

