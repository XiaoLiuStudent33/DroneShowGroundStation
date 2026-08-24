#pragma once

#include "AppConfig.h"
#include "ConsoleUploadJobAuthority.h"
#include "DroneManager.h"
#include "MissionPreparationWorker.h"
#include "MissionTransport.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct RouteUploadManagerConfig
{
    bool routeUploadEnabled = false;
    MissionTransportMode transportMode = MissionTransportMode::MissionProtocol;
    std::uint32_t mavftpSegmentItems = AppConfigDefaults::MavftpSegmentItems;
    int mavftpRequestTimeoutMs = AppConfigDefaults::MavftpRequestTimeoutMs;
    int mavftpTerminateTimeoutMs = AppConfigDefaults::MavftpTerminateTimeoutMs;
    std::uint32_t mavftpRequestRetryCount =
        AppConfigDefaults::MavftpRequestRetryCount;
    std::uint32_t mavftpWriteWindow = AppConfigDefaults::MavftpWriteWindow;
};

/** Single production factory; explicit selection never falls back. */
std::unique_ptr<IMissionTransport> CreateConfiguredMissionTransport(
    DroneSession& session,
    const RouteUploadManagerConfig& config,
    std::string& diagnostic);

struct RouteUploadTargetMetrics
{
    std::string droneId;
    std::int32_t routeSlotIndex = -1;
    std::uint32_t missionItemCount = 0;
    std::uint64_t artifactRouteItems = 0;
    std::uint64_t wireMissionItems = 0;
    std::uint64_t logicalPayloadBytes = 0;
	std::uint64_t uniqueItemsServed = 0;
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
    std::string terminalPhase;
	dspgcs::RouteUploadErrorCode resultCode = dspgcs::RouteUploadErrorCode::None;
    std::string safeDiagnostic;
    std::string verificationMismatchDiagnostic;
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
    RouteUploadManager(
        ConsoleUploadJobAuthority& authority,
        DroneManager& droneManager,
        RouteUploadManagerConfig config);
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
	const std::string& GetLastTerminalDiagnosticForTest() const
		{ return lastTerminalDiagnostic_; }
	const std::string& GetLastJobSummaryForTest() const { return lastJobSummary_; }

private:
    struct RuntimeTarget
    {
        UploadJobTargetMetadata metadata;
        dspgcs::UploadRouteStateDTO state;
        std::vector<dronegcs::route_mission::MissionItemIntV1> prepared;
        std::unique_ptr<IMissionTransport> transport;
        DroneSession* session = nullptr;
        std::string endpoint;
        std::uint64_t lastLoggedMavftpSegment =
            std::numeric_limits<std::uint64_t>::max();
        bool loggedMavftpVerification = false;
        std::uint64_t generation = 0;
        std::chrono::steady_clock::time_point admittedAt{};
        std::chrono::steady_clock::time_point uploadStartedAt{};
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
	void PersistTargetTerminal(const RuntimeTarget& target);
	void PersistJobSummary(const dspgcs::UploadJobFailureDTO* firstFailure);

    ConsoleUploadJobAuthority& authority_;
    DroneManager& droneManager_;
    bool routeUploadEnabled_ = false;
    RouteUploadManagerConfig config_;
    bool shuttingDown_ = false;
    MissionPreparationWorker preparationWorker_;
    std::optional<RuntimeJob> activeJob_;
    RouteUploadJobMetrics jobMetrics_;
    std::vector<RouteUploadTargetMetrics> targetMetrics_;
	std::string lastTerminalDiagnostic_;
	std::string lastJobSummary_;
};
