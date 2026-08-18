#pragma once

#include "FrozenAssignmentStore.h"
#include "RouteArtifactContract.h"
#include "Wpl110Core.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class ConsoleArtifactErrorCode : std::uint16_t
{
    None,
    Cancelled,
    InvalidWorkspaceRoot,
    CurrentMissing,
    PathEscapesRoot,
    ReparsePointDetected,
    UnexpectedCurrentEntry,
    ReadyMissingOrInvalid,
    ManifestMissingOrInvalid,
    ArtifactIdentityMismatch,
    FrozenAssignmentMissing,
    FrozenAssignmentMismatch,
    RouteMissing,
    RouteSizeMismatch,
    RouteDigestMismatch,
    WaypointSemanticInvalid,
    FileChangedDuringValidation,
    IoFailure,
    InternalError
};

struct ConsoleArtifactDiagnostic
{
    ConsoleArtifactErrorCode errorCode = ConsoleArtifactErrorCode::None;
    std::int32_t routeSlotIndex = -1;
    std::int32_t rowIndex = -1;
    std::int32_t fieldIndex = -1;
    dronegcs::wpl110::ErrorCode waypointErrorCode =
        dronegcs::wpl110::ErrorCode::None;
    std::string safeDetail;
};

struct ExpectedArtifactIdentity
{
    std::string artifactSetId;
    dronegcs::route_artifact::Digest256 artifactSetDigest;
    dronegcs::route_artifact::AssignmentBinding assignmentBinding;
};

struct ValidatedArtifactRouteDescriptor
{
    std::int32_t routeSlotIndex = -1;
    std::string relativePath;
    std::int64_t fileByteSize = 0;
    dronegcs::route_artifact::Digest256 routeArtifactDigest;
};

struct ValidatedArtifactSnapshot
{
    std::string artifactSetId;
    dronegcs::route_artifact::Digest256 artifactSetDigest;
    dronegcs::route_artifact::AssignmentBinding assignmentBinding;
    std::int64_t fleetRevisionAtCapture = 0;
    std::int64_t routeDatasetGeneration = 0;
    dronegcs::route_artifact::CompletionMode completionMode =
        dronegcs::route_artifact::CompletionMode::Full;
    dronegcs::route_artifact::FreezePolicy freezePolicy =
        dronegcs::route_artifact::FreezePolicy::RequireFull;
    std::int32_t matchedCount = 0;
    std::int32_t spareCount = 0;
    std::int32_t excludedCount = 0;
    std::int32_t vacantCount = 0;
    dronegcs::route_artifact::SourceIdentity sourceIdentity;
    std::string waypointGeneratorVersion;
    dronegcs::route_artifact::Digest256 generatorInputDigest;
    std::int32_t frameCount = 0;
    std::int32_t routeCount = 0;
    std::vector<dronegcs::route_artifact::MatchedMapping> matchedMappings;
    std::vector<ValidatedArtifactRouteDescriptor> routes;
    std::int64_t validatedAtUtcMs = 0;
    std::uint64_t validationGeneration = 0;
};

struct ConsoleArtifactValidationMetrics
{
    std::uint64_t routeFilesRead = 0;
    std::uint64_t physicalRouteOpenCount = 0;
    std::uint64_t rowsParsed = 0;
    std::uint64_t bytesRead = 0;
    std::uint32_t peakProcessHandles = 0;
    double elapsedSeconds = 0.0;
};

struct ConsoleArtifactValidationResult
{
    bool success = false;
    std::uint64_t generation = 0;
    std::shared_ptr<const ValidatedArtifactSnapshot> snapshot;
    ConsoleArtifactDiagnostic diagnostic;
    ConsoleArtifactValidationMetrics metrics;
};

/**
 * Console-owned, read-only Artifact authority consumer. Validation runs on one
 * persistent worker; no request creates per-route threads or retains WPL rows.
 */
class ConsoleArtifactConsumer final
{
public:
    explicit ConsoleArtifactConsumer(std::string configuredWorkspaceRoot);
    ~ConsoleArtifactConsumer();

    ConsoleArtifactConsumer(const ConsoleArtifactConsumer&) = delete;
    ConsoleArtifactConsumer& operator=(const ConsoleArtifactConsumer&) = delete;

    std::uint64_t RequestValidation(
        ExpectedArtifactIdentity expectedIdentity,
        std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenAssignment);
    bool PollCompleted(ConsoleArtifactValidationResult& result);
    void CancelValidation();

    bool IsCurrentIdentityStill(
        const ExpectedArtifactIdentity& expectedIdentity,
        ConsoleArtifactDiagnostic& diagnostic) const;

    static ConsoleArtifactValidationResult ValidateCurrentForTest(
        const std::string& configuredWorkspaceRoot,
        const ExpectedArtifactIdentity& expectedIdentity,
        const std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>& frozenAssignment,
        const std::atomic_bool* cancellation = nullptr,
        std::uint64_t generation = 1);

private:
    struct Request
    {
        std::uint64_t generation = 0;
        std::uint64_t cancellationVersion = 0;
        ExpectedArtifactIdentity expectedIdentity;
        std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenAssignment;
    };
    void WorkerMain();

    std::string workspaceRoot_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::optional<ConsoleArtifactValidationResult> completed_;
    std::atomic_bool stop_{ false };
    std::atomic<std::uint64_t> cancellationVersion_{ 0 };
    std::uint64_t nextGeneration_ = 0;
};
