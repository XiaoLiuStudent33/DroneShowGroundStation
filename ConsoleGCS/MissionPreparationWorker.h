#pragma once

#include "ConsoleUploadJobAuthority.h"
#include "RouteMissionCodec.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class MissionPreparationError : std::uint8_t
{
    None,
    Cancelled,
    InvalidSource,
    RouteMissing,
    RouteSizeMismatch,
    RouteDigestMismatch,
    WaypointValidationFailed,
    MissionConversionFailed,
    IoFailure
};

struct MissionPreparationRequest
{
    std::string uploadJobId;
    std::uint64_t generation = 0;
    std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot;
    UploadJobTargetMetadata target;
};

struct MissionPreparationResult
{
    std::string uploadJobId;
    std::uint64_t generation = 0;
    UploadJobTargetMetadata target;
    MissionPreparationError error = MissionPreparationError::None;
    std::string safeDiagnostic;
    std::vector<dronegcs::route_mission::MissionItemIntV1> mission;
    std::uint64_t bytesRead = 0;
    std::uint64_t routeFileOpenDurationMs = 0;
    std::uint64_t routeReadDurationMs = 0;
    std::uint64_t waypointParseCompileDurationMs = 0;
    std::uint64_t hashFinalizeDurationMs = 0;
    std::uint64_t missionItemsParsed = 0;
    std::uint64_t prepareDurationMs = 0;

    bool Succeeded() const { return error == MissionPreparationError::None; }
};

/** One persistent worker with one pending/in-flight request and one completion. */
class MissionPreparationWorker final
{
public:
    MissionPreparationWorker();
    ~MissionPreparationWorker();

    MissionPreparationWorker(const MissionPreparationWorker&) = delete;
    MissionPreparationWorker& operator=(const MissionPreparationWorker&) = delete;

    bool Submit(MissionPreparationRequest request);
    bool Poll(MissionPreparationResult& result);
    void CancelActive();
    void Shutdown();
    bool IsBusy() const;
    std::size_t GetWorkerThreadCountForTest() const { return 1; }
    std::size_t GetCompletionCapacityForTest() const { return 1; }

    static MissionPreparationResult PrepareForTest(
        const MissionPreparationRequest& request,
        const std::atomic_bool* cancelled = nullptr);

private:
    void ThreadMain();

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<MissionPreparationRequest> pending_;
    std::optional<MissionPreparationResult> completed_;
    std::thread worker_;
    std::atomic_bool stop_{ false };
    std::atomic_bool cancel_{ false };
    bool inFlight_ = false;
};
