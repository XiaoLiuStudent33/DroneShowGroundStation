#pragma once

#include "ConsoleArtifactConsumerV2.h"
#include "DSPGCSUploadJobProtocol.h"
#include "FrozenAssignmentStore.h"
#include "RouteArtifactReadLease.h"

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct UploadJobTargetMetadata
{
    std::string droneId;
    std::int32_t routeSlotIndex = -1;
    std::string relativePath;
    std::int64_t fileByteSize = 0;
    std::uint32_t missionItemCount = 0;
    std::uint64_t missionPayloadByteSize = 0;
    dronegcs::route_artifact::Digest256 routeArtifactDigest;
};

/** Value-copy execution contract; it never queries latest Artifact/Frozen authorities. */
struct UploadJobExecutionView
{
    std::string uploadJobId;
    std::uint64_t jobRevision = 0;
    std::chrono::steady_clock::time_point createdAt{};
    dspgcs::UploadArtifactBindingDTO artifactBinding;
    dspgcs::UploadFrozenAssignmentBindingDTO frozenBinding;
    std::uint32_t effectiveMaxConcurrentUploads = 0;
    std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot;
    std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenSnapshot;
    RouteArtifactV2CurrentDirectoryIdentity currentDirectoryIdentity;
    const dronegcs::route_artifact::ArtifactReadLease* artifactReadLease = nullptr;
    std::vector<UploadJobTargetMetadata> targets;
};

class ConsoleUploadJobAuthority final
{
public:
    static constexpr std::size_t MaxIdempotencyEntries = 1024;
    static constexpr std::size_t RoutePayloadBudgetBytes = 900;

    ConsoleUploadJobAuthority(
        std::string workspaceRoot,
        ConsoleArtifactConsumerV2* artifactConsumer,
        FrozenAssignmentStore* frozenAssignmentStore);

    dspgcs::UploadJobCommandAckDTO HandleCommand(
        const std::string& clientSessionId,
        const dspgcs::UploadJobCommandDTO& command,
        const std::vector<std::uint8_t>& canonicalCommandPayload);

    void Tick();

    dspgcs::UploadJobSnapshotDTO BuildSnapshot(
        const dspgcs::UploadJobSnapshotRequestDTO& request);
    bool BuildRoutePage(
        const dspgcs::UploadJobSnapshotRequestDTO& request,
        dspgcs::UploadJobRouteStatePageDTO& page,
        dspgcs::UploadJobErrorCode& errorCode);
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> DrainRouteDeltas();

    /** Narrow future RouteUploadManager seam; it owns the measurements. */
    bool ApplyExecutorRouteUpdates(
        const std::string& uploadJobId,
        const std::vector<dspgcs::UploadRouteStateDTO>& updates,
        std::uint64_t throughputBytesPerSecond,
        std::uint32_t throughputWindowMs,
        std::uint64_t elapsedMs,
        std::string& error);
    bool RefreshExecutorAggregate(
        const std::string& uploadJobId,
        std::uint64_t throughputBytesPerSecond,
        std::uint32_t throughputWindowMs,
        std::uint64_t elapsedMs,
        std::string& error);
    bool TransitionJobForExecutor(
        const std::string& uploadJobId,
        dspgcs::UploadJobState newState,
        std::string& error);
    std::optional<UploadJobExecutionView> GetExecutionView(
        const std::string& uploadJobId) const;
    std::vector<std::string> GetExecutableJobIds() const;
    std::optional<dspgcs::UploadJobSnapshotDTO> GetExecutorSnapshot(
        const std::string& uploadJobId) const;
    std::vector<dspgcs::UploadRouteStateDTO> GetExecutorRouteStates(
        const std::string& uploadJobId) const;
    bool IsCancellationRequested(const std::string& uploadJobId) const;
    bool FinalizeExecutorCancellation(
        const std::string& uploadJobId,
        std::string& error);
    bool FinalizeExecutorShutdown(
        const std::string& uploadJobId,
        std::string& error);
    bool DestroyFailedJob(const std::string& uploadJobId);

    const dspgcs::UploadJobSnapshotDTO* FindCurrentSnapshotForTest(
        const std::string& uploadJobId);
    std::optional<UploadJobTargetMetadata> FindTargetMetadataForTest(
        const std::string& uploadJobId,
        std::int32_t routeSlotIndex) const;
    bool InstallSyntheticReadyJobForTest(
        const std::string& uploadJobId,
        const dspgcs::UploadArtifactBindingDTO& artifactBinding,
        const dspgcs::UploadFrozenAssignmentBindingDTO& frozenBinding,
        const dspgcs::UploadStableSourceDTO& stableSource,
        std::vector<dspgcs::UploadRouteStateDTO> routes,
        std::uint32_t vacantCount,
        std::uint32_t spareCount,
        std::uint32_t excludedCount,
        std::uint32_t maxConcurrentUploads,
        std::string& error);
    bool InstallSyntheticExecutionBindingForTest(
        const std::string& uploadJobId,
        std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot,
        std::vector<UploadJobTargetMetadata> targets,
        std::string& error);
    std::size_t GetJobCount() const { return jobs_.size(); }
    std::size_t GetPendingStartCount() const { return pendingStarts_.size(); }
    std::size_t GetIdempotencyEntryCount() const { return idempotency_.size(); }
    std::size_t GetFrozenPageRouteCountForTest(const std::string& uploadJobId) const;

private:
    struct Target
    {
        UploadJobTargetMetadata metadata;
        dspgcs::UploadRouteStateDTO state;
    };

    struct Job
    {
        dspgcs::UploadJobSnapshotDTO snapshot;
        std::vector<Target> targets;
        std::vector<dspgcs::UploadRouteStateDTO> pageSnapshot;
        std::uint64_t pageSnapshotRevision = 0;
        std::string pageSnapshotToken;
        std::uint64_t nextDeltaSequence = 0;
        std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot;
        std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenSnapshot;
        RouteArtifactV2CurrentDirectoryIdentity currentDirectoryIdentity;
        std::unique_ptr<dronegcs::route_artifact::ArtifactReadLease> artifactLease;
        std::chrono::steady_clock::time_point createdAt{};
    };

    struct PendingStart
    {
        dspgcs::UploadJobSnapshotDTO snapshot;
        std::uint64_t validationGeneration = 0;
        std::string idempotencyKey;
        dspgcs::UploadJobCommandDTO command;
        ExpectedRouteArtifactV2Identity expectedArtifact;
        std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> frozenSnapshot;
        std::chrono::steady_clock::time_point createdAt{};
        std::chrono::steady_clock::time_point validationRequestedAt{};
        std::uint64_t authorityLookupDurationMs = 0;
    };

    struct StartOutcome
    {
        dspgcs::UploadJobErrorCode errorCode = dspgcs::UploadJobErrorCode::None;
        std::string safeDiagnostic;
    };

    struct IdempotencyRecord
    {
        std::vector<std::uint8_t> canonicalPayload;
        dspgcs::UploadJobCommandAckDTO acknowledgement;
    };

    static std::string MakeCanonicalId();
    static bool SameFrozenBinding(
        const FrozenAssignmentAcceptedSnapshot& frozen,
        const dspgcs::UploadFrozenAssignmentBindingDTO& binding);
    static ExpectedRouteArtifactV2Identity ToExpectedIdentity(
        const dspgcs::UploadArtifactBindingDTO& binding);
    static dspgcs::UploadArtifactBindingDTO ToWireArtifactBinding(
        const AcceptedRouteArtifactV2& snapshot);
    static dspgcs::UploadStableSourceDTO ToWireStableSource(
        const AcceptedRouteArtifactV2StableSource& stableSource);
    static std::int64_t UtcNowMs();

    dspgcs::UploadJobCommandAckDTO Reject(
        const dspgcs::UploadJobCommandDTO& command,
        dspgcs::UploadJobErrorCode code,
        const char* diagnostic,
        std::uint64_t revision = 0,
        std::uint32_t effectiveConcurrency = 0) const;
    dspgcs::UploadJobCommandAckDTO Accept(
        const dspgcs::UploadJobCommandDTO& command,
        std::uint64_t revision,
        std::uint32_t effectiveConcurrency) const;
    void RememberCommand(
        const std::string& key,
        std::vector<std::uint8_t> canonicalPayload,
        const dspgcs::UploadJobCommandAckDTO& acknowledgement);
    void CompletePendingStart(
        const std::string& uploadJobId,
        PendingStart pending,
        const ConsoleArtifactV2ValidationResult& result);
    void FailPendingStart(
        const std::string& uploadJobId,
        dspgcs::UploadJobErrorCode code,
        const char* diagnostic,
        const PendingStart* pending = nullptr);
    bool AcquireArtifactLease(
        const PendingStart& pending,
        const RouteArtifactV2CurrentDirectoryIdentity& currentDirectoryIdentity,
        std::unique_ptr<dronegcs::route_artifact::ArtifactReadLease>& lease,
        ConsoleArtifactV2Diagnostic& diagnostic);
    void RecalculateAggregate(Job& job);
    void RecalculateFirstFailure(Job& job);
    void QueueRouteDeltas(Job& job, const std::vector<dspgcs::UploadRouteStateDTO>& updates);
    void FinalizeCancellation(Job& job);
    Job* FindJob(const std::string& uploadJobId);

    std::string workspaceRoot_;
    ConsoleArtifactConsumerV2* artifactConsumer_ = nullptr;
    FrozenAssignmentStore* frozenAssignmentStore_ = nullptr;
    std::unordered_map<std::string, Job> jobs_;
    std::unordered_map<std::string, PendingStart> pendingStarts_;
    std::unordered_map<std::uint64_t, std::string> validationStarts_;
    std::unordered_map<std::string, StartOutcome> startOutcomes_;
    std::unordered_map<std::string, IdempotencyRecord> idempotency_;
    std::deque<std::string> idempotencyOrder_;
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> pendingDeltas_;
};
