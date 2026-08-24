#pragma once

#include "RouteArtifactContractV2.h"
#include "Wpl110Core.h"

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

enum class ConsoleArtifactV2ErrorCode : std::uint16_t
{
    None,
    Cancelled,
    DeadlineExceeded,
    LegacyUnsupported,
    InvalidWorkspaceRoot,
    CurrentMissing,
    PathEscapesRoot,
    ReparsePointDetected,
    UnexpectedCurrentEntry,
    ReadyMissingOrInvalid,
    ManifestMissingOrInvalid,
    ArtifactIdentityMismatch,
    RouteMissing,
    RouteSizeMismatch,
    RouteDigestMismatch,
    WaypointSemanticInvalid,
    FileChangedDuringValidation,
    IoFailure,
    InternalError
};

struct ConsoleArtifactV2Diagnostic
{
    ConsoleArtifactV2ErrorCode errorCode = ConsoleArtifactV2ErrorCode::None;
    std::int32_t routeSlotIndex = -1;
    std::int32_t rowIndex = -1;
    std::int32_t fieldIndex = -1;
    dronegcs::wpl110::ErrorCode waypointErrorCode =
        dronegcs::wpl110::ErrorCode::None;
    std::string safeDetail;
};

struct ExpectedRouteArtifactV2Identity
{
    std::string artifactSetId;
    dronegcs::route_artifact::Digest256 artifactSetDigest;
};

struct RouteArtifactV2CurrentDirectoryIdentity
{
    std::uint32_t volumeSerialNumber = 0;
    std::uint32_t fileIndexHigh = 0;
    std::uint32_t fileIndexLow = 0;

    bool operator==(const RouteArtifactV2CurrentDirectoryIdentity& other) const
    {
        return volumeSerialNumber == other.volumeSerialNumber
            && fileIndexHigh == other.fileIndexHigh
            && fileIndexLow == other.fileIndexLow;
    }
};

struct AcceptedRouteArtifactV2StableSource
{
    dronegcs::route_artifact::Digest256 sourceDatasetDigest;
    std::int32_t normalizedShowOriginLatitudeE7 = 0;
    std::int32_t normalizedShowOriginLongitudeE7 = 0;
    std::int32_t routeSlotCount = 0;
};

struct RouteArtifactV2FileIdentity
{
    std::uint32_t volumeSerialNumber = 0;
    std::uint32_t fileIndexHigh = 0;
    std::uint32_t fileIndexLow = 0;
    std::uint32_t fileSizeHigh = 0;
    std::uint32_t fileSizeLow = 0;
    std::uint32_t lastWriteTimeHigh = 0;
    std::uint32_t lastWriteTimeLow = 0;

    bool operator==(const RouteArtifactV2FileIdentity& other) const
    {
        return volumeSerialNumber == other.volumeSerialNumber
            && fileIndexHigh == other.fileIndexHigh
            && fileIndexLow == other.fileIndexLow
            && fileSizeHigh == other.fileSizeHigh
            && fileSizeLow == other.fileSizeLow
            && lastWriteTimeHigh == other.lastWriteTimeHigh
            && lastWriteTimeLow == other.lastWriteTimeLow;
    }
};

struct ConsoleArtifactV2MatchedRoute
{
    std::string droneId;
    std::int32_t routeSlotIndex = -1;

    bool operator==(const ConsoleArtifactV2MatchedRoute& other) const
    {
        return droneId == other.droneId
            && routeSlotIndex == other.routeSlotIndex;
    }
};

struct ConsoleArtifactV2ValidationScope
{
    bool fullArtifactValidation = true;
    std::uint64_t frozenAssignmentRevision = 0;
    std::array<std::uint8_t, 32> frozenAssignmentDigest{};
    std::vector<ConsoleArtifactV2MatchedRoute> matchedRoutes;

    bool operator==(const ConsoleArtifactV2ValidationScope& other) const
    {
        return fullArtifactValidation == other.fullArtifactValidation
            && frozenAssignmentRevision == other.frozenAssignmentRevision
            && frozenAssignmentDigest == other.frozenAssignmentDigest
            && matchedRoutes == other.matchedRoutes;
    }
};

struct AcceptedRouteArtifactV2RouteRecord
{
    std::int32_t routeSlotIndex = -1;
    std::string relativePath;
    std::int64_t fileByteSize = 0;
    dronegcs::route_artifact::Digest256 routeArtifactDigest;
    bool validationConfirmed = false;
    RouteArtifactV2FileIdentity validatedFileIdentity;
};

/** Immutable authority snapshot for a validated schema-v2 Current and scope. */
struct AcceptedRouteArtifactV2
{
    std::uint16_t artifactSchemaVersion =
        dronegcs::route_artifact::v2::ArtifactSchemaVersion;
    std::string artifactSetId;
    dronegcs::route_artifact::Digest256 artifactSetDigest;
    std::int64_t createdAtUtcMs = 0;

    AcceptedRouteArtifactV2StableSource stableSource;
    std::int64_t sourceByteSize = 0;
    std::string sourceDisplayFileName;
    std::int32_t frameCount = 0;

    bool hasRange = false;
    std::int32_t rangeStart = 0;
    std::int32_t rangeEnd = 0;

    std::string waypointGeneratorVersion;
    dronegcs::route_artifact::Digest256 generatorInputDigest;
    std::vector<AcceptedRouteArtifactV2RouteRecord> routes;

    std::string canonicalWorkspaceRoot;
    std::string resolvedCurrentRoot;
    RouteArtifactV2CurrentDirectoryIdentity currentDirectoryIdentity;
    std::int64_t validatedAtUtcMs = 0;
    std::uint64_t validationGeneration = 0;
    ConsoleArtifactV2ValidationScope validationScope;
};

struct ConsoleArtifactV2ValidationMetrics
{
    bool cacheHit = false;
    bool fullArtifactValidation = true;
    std::uint64_t manifestRouteCount = 0;
    std::uint64_t selectedRouteCount = 0;
    std::uint64_t routeFilesRead = 0;
    std::uint64_t physicalRouteOpenCount = 0;
    std::uint64_t rowsParsed = 0;
    std::uint64_t bytesRead = 0;
    std::uint32_t peakProcessHandles = 0;
    std::uint64_t peakWorkingSetBytes = 0;
    std::uint64_t metadataDurationMs = 0;
    std::uint64_t routeValidationDurationMs = 0;
    std::uint64_t metadataRecheckDurationMs = 0;
    std::uint64_t cacheIdentityCheckDurationMs = 0;
    double elapsedSeconds = 0.0;
};

struct ConsoleArtifactV2ValidationResult
{
    bool success = false;
    std::uint64_t generation = 0;
    std::shared_ptr<const AcceptedRouteArtifactV2> snapshot;
    ConsoleArtifactV2Diagnostic diagnostic;
    ConsoleArtifactV2ValidationMetrics metrics;
};

/**
 * Pure schema-v2 Route Artifact consumer. It owns one persistent validation
 * worker and has no Frozen Assignment, Fleet, mapping, UploadJob, or mission
 * encoder dependency.
 */
class ConsoleArtifactConsumerV2 final
{
public:
    using RouteValidatedHook = std::function<void(std::int32_t)>;

    explicit ConsoleArtifactConsumerV2(std::string configuredWorkspaceRoot);
    ~ConsoleArtifactConsumerV2();

    ConsoleArtifactConsumerV2(const ConsoleArtifactConsumerV2&) = delete;
    ConsoleArtifactConsumerV2& operator=(const ConsoleArtifactConsumerV2&) = delete;

    std::uint64_t RequestValidation(
        ExpectedRouteArtifactV2Identity expectedIdentity,
        std::chrono::milliseconds deadline = std::chrono::minutes(5));
    std::uint64_t RequestValidation(
        ExpectedRouteArtifactV2Identity expectedIdentity,
        ConsoleArtifactV2ValidationScope validationScope,
        std::chrono::milliseconds deadline = std::chrono::minutes(5));
    bool PollCompleted(ConsoleArtifactV2ValidationResult& result);
    void CancelValidation();

    std::shared_ptr<const AcceptedRouteArtifactV2> GetAcceptedSnapshot() const;
    std::optional<AcceptedRouteArtifactV2RouteRecord> FindRouteRecord(
        std::int32_t routeSlotIndex) const;

    bool IsCurrentIdentityStill(
        const ExpectedRouteArtifactV2Identity& expectedIdentity,
        ConsoleArtifactV2Diagnostic& diagnostic) const;
    bool IsCurrentIdentityStill(
        const ExpectedRouteArtifactV2Identity& expectedIdentity,
        const RouteArtifactV2CurrentDirectoryIdentity& expectedCurrentDirectoryIdentity,
        ConsoleArtifactV2Diagnostic& diagnostic) const;

    static ConsoleArtifactV2ValidationResult ValidateCurrentForTest(
        const std::string& configuredWorkspaceRoot,
        const ExpectedRouteArtifactV2Identity& expectedIdentity,
        const std::atomic_bool* cancellation = nullptr,
        std::uint64_t generation = 1,
        std::chrono::milliseconds deadline = std::chrono::minutes(5),
        RouteValidatedHook routeValidatedHook = {},
        ConsoleArtifactV2ValidationScope validationScope = {});

private:
    struct Request
    {
        std::uint64_t generation = 0;
        std::uint64_t cancellationVersion = 0;
        ExpectedRouteArtifactV2Identity expectedIdentity;
        ConsoleArtifactV2ValidationScope validationScope;
        std::chrono::steady_clock::time_point deadline;
    };

    void WorkerMain();

    std::string workspaceRoot_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<Request> pending_;
    std::optional<ConsoleArtifactV2ValidationResult> completed_;
    std::shared_ptr<const AcceptedRouteArtifactV2> accepted_;
    std::atomic_bool stop_{ false };
    std::atomic<std::uint64_t> cancellationVersion_{ 0 };
    std::uint64_t nextGeneration_ = 0;
};
