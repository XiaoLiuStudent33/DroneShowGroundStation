#include "ConsoleUploadJobAuthority.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <unordered_map>

namespace
{
    dronegcs::route_artifact::Digest256 ToArtifactDigest(
        const dspgcs::UploadJobDigest& source)
    {
        dronegcs::route_artifact::Digest256 result;
        std::copy(source.begin(), source.end(), result.Bytes.begin());
        result.IsSet = true;
        return result;
    }

    dspgcs::UploadJobDigest ToWireDigest(
        const dronegcs::route_artifact::Digest256& source)
    {
        dspgcs::UploadJobDigest result{};
        std::copy(source.Bytes.begin(), source.Bytes.end(), result.begin());
        return result;
    }
}

ConsoleUploadJobAuthority::ConsoleUploadJobAuthority(
    std::string workspaceRoot,
    ConsoleArtifactConsumer* artifactConsumer,
    FrozenAssignmentStore* frozenAssignmentStore)
    : workspaceRoot_(std::move(workspaceRoot))
    , artifactConsumer_(artifactConsumer)
    , frozenAssignmentStore_(frozenAssignmentStore)
{
}

std::int64_t ConsoleUploadJobAuthority::UtcNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ConsoleUploadJobAuthority::MakeCanonicalId()
{
    static thread_local std::mt19937_64 generator(std::random_device{}());
    std::ostringstream stream;
    stream << std::hex << std::setfill('0')
        << std::setw(16) << generator() << std::setw(16) << generator();
    return stream.str();
}

bool ConsoleUploadJobAuthority::SameBinding(
    const dronegcs::route_artifact::AssignmentBinding& binding,
    const dspgcs::UploadArtifactIdentityDTO& identity)
{
    return binding.FleetSessionId == identity.fleetSessionId &&
        binding.AssignmentAuthoritySessionId == identity.assignmentAuthoritySessionId &&
        binding.AssignmentRevision >= 0 &&
        static_cast<std::uint64_t>(binding.AssignmentRevision) == identity.assignmentRevision &&
        binding.AssignmentDigest == ToArtifactDigest(identity.assignmentDigest);
}

bool ConsoleUploadJobAuthority::SameFrozenBinding(
    const FrozenAssignmentAcceptedSnapshot& frozen,
    const dspgcs::UploadArtifactIdentityDTO& identity)
{
    return frozen.identity.fleetSessionId == identity.fleetSessionId &&
        frozen.identity.assignmentAuthoritySessionId == identity.assignmentAuthoritySessionId &&
        frozen.identity.assignmentRevision == identity.assignmentRevision &&
        frozen.canonicalPayloadDigest == identity.assignmentDigest;
}

ExpectedArtifactIdentity ConsoleUploadJobAuthority::ToExpectedIdentity(
    const dspgcs::UploadArtifactIdentityDTO& identity)
{
    ExpectedArtifactIdentity result;
    result.artifactSetId = identity.artifactSetId;
    result.artifactSetDigest = ToArtifactDigest(identity.artifactSetDigest);
    result.assignmentBinding.FleetSessionId = identity.fleetSessionId;
    result.assignmentBinding.AssignmentAuthoritySessionId =
        identity.assignmentAuthoritySessionId;
    result.assignmentBinding.AssignmentRevision =
        static_cast<std::int64_t>(identity.assignmentRevision);
    result.assignmentBinding.AssignmentDigest = ToArtifactDigest(identity.assignmentDigest);
    return result;
}

dspgcs::UploadArtifactIdentityDTO ConsoleUploadJobAuthority::ToWireIdentity(
    const ValidatedArtifactSnapshot& snapshot)
{
    dspgcs::UploadArtifactIdentityDTO result;
    result.artifactSetId = snapshot.artifactSetId;
    result.artifactSetDigest = ToWireDigest(snapshot.artifactSetDigest);
    result.fleetSessionId = snapshot.assignmentBinding.FleetSessionId;
    result.assignmentAuthoritySessionId =
        snapshot.assignmentBinding.AssignmentAuthoritySessionId;
    result.assignmentRevision = static_cast<std::uint64_t>(
        snapshot.assignmentBinding.AssignmentRevision);
    result.assignmentDigest = ToWireDigest(snapshot.assignmentBinding.AssignmentDigest);
    return result;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::Reject(
    const dspgcs::UploadJobCommandDTO& command,
    const dspgcs::UploadJobErrorCode code,
    const char* diagnostic,
    const std::uint64_t revision,
    const std::uint32_t effectiveConcurrency) const
{
    dspgcs::UploadJobCommandAckDTO result;
    result.clientCommandId = command.clientCommandId;
    result.uploadJobId = command.uploadJobId;
    result.commandType = command.commandType;
    result.status = dspgcs::UploadJobCommandAckStatus::Rejected;
    result.resultCode = code;
    result.authoritativeJobRevision = revision;
    result.effectiveMaxConcurrentUploads = effectiveConcurrency;
    result.safeDiagnostic = diagnostic ? diagnostic : "";
    return result;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::Accept(
    const dspgcs::UploadJobCommandDTO& command,
    const Job& job) const
{
    dspgcs::UploadJobCommandAckDTO result;
    result.clientCommandId = command.clientCommandId;
    result.uploadJobId = command.uploadJobId;
    result.commandType = command.commandType;
    result.status = dspgcs::UploadJobCommandAckStatus::Accepted;
    result.resultCode = dspgcs::UploadJobErrorCode::None;
    result.authoritativeJobRevision = job.snapshot.jobRevision;
    result.effectiveMaxConcurrentUploads =
        job.snapshot.effectiveMaxConcurrentUploads;
    return result;
}

void ConsoleUploadJobAuthority::RememberCommand(
    const std::string& key,
    std::vector<std::uint8_t> canonicalPayload,
    const dspgcs::UploadJobCommandAckDTO& acknowledgement)
{
    while (idempotency_.size() >= MaxIdempotencyEntries &&
        !idempotencyOrder_.empty())
    {
        idempotency_.erase(idempotencyOrder_.front());
        idempotencyOrder_.pop_front();
    }
    idempotencyOrder_.push_back(key);
    idempotency_[key] = { std::move(canonicalPayload), acknowledgement };
}

ConsoleUploadJobAuthority::Job* ConsoleUploadJobAuthority::FindJob(
    const std::string& uploadJobId)
{
    const auto item = jobs_.find(uploadJobId);
    return item == jobs_.end() ? nullptr : &item->second;
}

dspgcs::UploadJobCommandAckDTO ConsoleUploadJobAuthority::HandleCommand(
    const std::string& clientSessionId,
    const dspgcs::UploadJobCommandDTO& command,
    const std::vector<std::uint8_t>& canonicalCommandPayload)
{
    const std::string idempotencyKey = clientSessionId + ":" + command.clientCommandId;
    const auto previous = idempotency_.find(idempotencyKey);
    if (previous != idempotency_.end())
    {
        if (previous->second.canonicalPayload == canonicalCommandPayload)
            return previous->second.acknowledgement;
        return Reject(command, dspgcs::UploadJobErrorCode::IdempotencyConflict,
            "ClientCommandID was already used with a different payload.");
    }

    dspgcs::UploadJobCommandAckDTO acknowledgement;
    Job* job = FindJob(command.uploadJobId);
    if (command.commandType == dspgcs::UploadJobCommandType::Start)
    {
        if (job)
        {
            if (!dspgcs::UploadArtifactIdentityEqual(
                job->snapshot.artifactIdentity, command.artifactIdentity))
            {
                acknowledgement = Reject(command,
                    dspgcs::UploadJobErrorCode::JobIdCollision,
                    "UploadJobID is already bound to a different Artifact identity.",
                    job->snapshot.jobRevision,
                    job->snapshot.effectiveMaxConcurrentUploads);
            }
            else
            {
                acknowledgement = Accept(command, *job);
            }
        }
        else if (!artifactConsumer_ || !frozenAssignmentStore_)
        {
            acknowledgement = Reject(command,
                dspgcs::UploadJobErrorCode::ArtifactNotValidated,
                "Artifact validation authority is unavailable.");
        }
        else
        {
            const auto frozen = frozenAssignmentStore_->GetAcceptedSnapshot(
                command.artifactIdentity.fleetSessionId);
            if (!frozen || !SameFrozenBinding(*frozen, command.artifactIdentity))
            {
                acknowledgement = Reject(command,
                    dspgcs::UploadJobErrorCode::FrozenAssignmentMismatch,
                    "Start identity does not match the accepted Frozen Assignment.");
            }
            else
            {
                Job created;
                created.snapshot.uploadJobId = command.uploadJobId;
                created.snapshot.artifactIdentity = command.artifactIdentity;
                created.snapshot.artifactGateState =
                    dspgcs::UploadArtifactGateState::Validating;
                created.snapshot.jobState = dspgcs::UploadJobState::Validating;
                created.snapshot.jobRevision = 1;
                created.snapshot.snapshotSequence = 1;
                created.snapshot.snapshotToken = MakeCanonicalId();
                created.snapshot.updatedAtUtcMs = UtcNowMs();
                created.snapshot.effectiveMaxConcurrentUploads =
                    command.requestedMaxConcurrentUploads;
                created.validationGeneration = artifactConsumer_->RequestValidation(
                    ToExpectedIdentity(command.artifactIdentity), frozen);
                validationJobs_[created.validationGeneration] = command.uploadJobId;
                auto insertion = jobs_.emplace(command.uploadJobId, std::move(created));
                acknowledgement = Accept(command, insertion.first->second);
            }
        }
    }
    else if (!job)
    {
        acknowledgement = Reject(command, dspgcs::UploadJobErrorCode::UnknownJob,
            "Upload Job does not exist.");
    }
    else if (command.expectedJobRevision != job->snapshot.jobRevision)
    {
        acknowledgement = Reject(command,
            dspgcs::UploadJobErrorCode::StaleJobRevision,
            "ExpectedJobRevision is stale.",
            job->snapshot.jobRevision,
            job->snapshot.effectiveMaxConcurrentUploads);
    }
    else
    {
        bool accepted = true;
        switch (command.commandType)
        {
        case dspgcs::UploadJobCommandType::Pause:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Ready &&
                job->snapshot.jobState != dspgcs::UploadJobState::Running)
                accepted = false;
            else job->snapshot.jobState = dspgcs::UploadJobState::Paused;
            break;
        case dspgcs::UploadJobCommandType::Resume:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Paused)
                accepted = false;
            else job->snapshot.jobState = dspgcs::UploadJobState::Running;
            break;
        case dspgcs::UploadJobCommandType::Cancel:
            if (job->snapshot.jobState == dspgcs::UploadJobState::Completed ||
                job->snapshot.jobState == dspgcs::UploadJobState::Cancelled)
                accepted = false;
            else
            {
                job->snapshot.jobState = dspgcs::UploadJobState::Cancelled;
                for (auto& route : job->routes)
                {
                    if (route.state != dspgcs::UploadRouteState::Succeeded)
                        route.state = dspgcs::UploadRouteState::Cancelled;
                }
                job->artifactLease.reset();
                RecalculateAggregate(*job);
            }
            break;
        case dspgcs::UploadJobCommandType::RetryFailed:
            if (job->snapshot.jobState != dspgcs::UploadJobState::Failed ||
                job->snapshot.failedCount == 0 || !job->artifactLease)
                accepted = false;
            else
            {
                for (auto& route : job->routes)
                    if (route.state == dspgcs::UploadRouteState::Failed)
                    {
                        route.state = dspgcs::UploadRouteState::Waiting;
                        route.errorCode = dspgcs::RouteUploadErrorCode::None;
                        route.safeFailureSummary.clear();
                        route.retryable = false;
                    }
                job->snapshot.jobState = dspgcs::UploadJobState::Ready;
                job->snapshot.firstFailure = {};
                RecalculateAggregate(*job);
            }
            break;
        case dspgcs::UploadJobCommandType::SetMaxConcurrentUploads:
            if (job->snapshot.jobState == dspgcs::UploadJobState::Completed ||
                job->snapshot.jobState == dspgcs::UploadJobState::Cancelled)
                accepted = false;
            else job->snapshot.effectiveMaxConcurrentUploads =
                command.requestedMaxConcurrentUploads;
            break;
        default:
            accepted = false;
            break;
        }
        if (!accepted)
        {
            acknowledgement = Reject(command,
                command.commandType == dspgcs::UploadJobCommandType::RetryFailed
                    ? dspgcs::UploadJobErrorCode::NoFailedRoutes
                    : dspgcs::UploadJobErrorCode::InvalidJobState,
                "Command is not legal in the current Job state.",
                job->snapshot.jobRevision,
                job->snapshot.effectiveMaxConcurrentUploads);
        }
        else
        {
            ++job->snapshot.jobRevision;
            job->snapshot.updatedAtUtcMs = UtcNowMs();
            acknowledgement = Accept(command, *job);
        }
    }

    RememberCommand(idempotencyKey, canonicalCommandPayload, acknowledgement);
    return acknowledgement;
}

void ConsoleUploadJobAuthority::FailJob(
    Job& job,
    const dspgcs::UploadJobErrorCode code,
    const char* diagnostic)
{
    job.snapshot.artifactGateState = dspgcs::UploadArtifactGateState::Rejected;
    job.snapshot.jobState = dspgcs::UploadJobState::Failed;
    job.snapshot.firstFailure.jobErrorCode = code;
    job.snapshot.firstFailure.occurredAtUtcMs = UtcNowMs();
    job.snapshot.firstFailure.safeSummary = diagnostic ? diagnostic : "";
    ++job.snapshot.jobRevision;
    job.snapshot.updatedAtUtcMs = UtcNowMs();
    job.artifactLease.reset();
}

bool ConsoleUploadJobAuthority::AcquireArtifactLease(
    Job& job,
    ConsoleArtifactDiagnostic& diagnostic)
{
    if (!artifactConsumer_->IsCurrentIdentityStill(
        ToExpectedIdentity(job.snapshot.artifactIdentity), diagnostic))
        return false;
    auto lease = std::make_unique<dronegcs::route_artifact::ArtifactReadLease>();
    bool recovered = false;
    std::string error;
    const auto result = lease->TryAcquire(
        std::filesystem::u8path(workspaceRoot_).wstring(), recovered, error);
    if (result != dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
    {
        diagnostic.errorCode = ConsoleArtifactErrorCode::IoFailure;
        diagnostic.safeDetail = result ==
            dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Busy
            ? "Artifact Current promotion gate is busy."
            : error;
        return false;
    }
    if (!artifactConsumer_->IsCurrentIdentityStill(
        ToExpectedIdentity(job.snapshot.artifactIdentity), diagnostic))
        return false;
    job.artifactLease = std::move(lease);
    return true;
}

void ConsoleUploadJobAuthority::CompleteValidation(
    Job& job,
    const ConsoleArtifactValidationResult& result)
{
    if (!result.success || !result.snapshot)
    {
        const auto code = result.diagnostic.errorCode ==
            ConsoleArtifactErrorCode::ArtifactIdentityMismatch
            ? dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch
            : dspgcs::UploadJobErrorCode::ArtifactValidationFailed;
        FailJob(job, code, "Artifact validation failed.");
        return;
    }
    const dspgcs::UploadArtifactIdentityDTO validatedIdentity =
        ToWireIdentity(*result.snapshot);
    if (!dspgcs::UploadArtifactIdentityEqual(
        validatedIdentity, job.snapshot.artifactIdentity))
    {
        FailJob(job, dspgcs::UploadJobErrorCode::ArtifactIdentityMismatch,
            "Validated Artifact identity differs from Start.");
        return;
    }
    ConsoleArtifactDiagnostic diagnostic;
    if (!AcquireArtifactLease(job, diagnostic))
    {
        const auto code = diagnostic.errorCode ==
            ConsoleArtifactErrorCode::ArtifactIdentityMismatch
            ? dspgcs::UploadJobErrorCode::CurrentReplaced
            : dspgcs::UploadJobErrorCode::ArtifactLeaseUnavailable;
        FailJob(job, code, diagnostic.safeDetail.c_str());
        return;
    }

    latestValidatedArtifact_ = result.snapshot;
    job.validatedArtifact = result.snapshot;
    job.routes.clear();
    job.routes.reserve(result.snapshot->matchedMappings.size());
    std::unordered_map<std::int32_t, const ValidatedArtifactRouteDescriptor*> descriptors;
    for (const auto& descriptor : result.snapshot->routes)
        descriptors.emplace(descriptor.routeSlotIndex, &descriptor);
    for (const auto& mapping : result.snapshot->matchedMappings)
    {
        dspgcs::UploadRouteStateDTO route;
        route.routeSlotIndex = mapping.RouteSlotIndex;
        route.droneId = mapping.DroneId;
        route.state = dspgcs::UploadRouteState::Waiting;
        const auto descriptor = descriptors.find(mapping.RouteSlotIndex);
        if (descriptor != descriptors.end() && descriptor->second->fileByteSize >= 0)
            route.totalBytes = static_cast<std::uint64_t>(descriptor->second->fileByteSize);
        route.updatedAtUtcMs = UtcNowMs();
        job.routes.push_back(std::move(route));
    }
    std::sort(job.routes.begin(), job.routes.end(),
        [](const auto& left, const auto& right)
        { return left.routeSlotIndex < right.routeSlotIndex; });
    job.snapshot.artifactGateState = dspgcs::UploadArtifactGateState::Validated;
    job.snapshot.jobState = dspgcs::UploadJobState::Ready;
    job.snapshot.matchedCount = static_cast<std::uint32_t>(result.snapshot->matchedCount);
    job.snapshot.vacantCount = static_cast<std::uint32_t>(result.snapshot->vacantCount);
    job.snapshot.spareCount = static_cast<std::uint32_t>(result.snapshot->spareCount);
    job.snapshot.excludedCount = static_cast<std::uint32_t>(result.snapshot->excludedCount);
    ++job.snapshot.jobRevision;
    job.snapshot.updatedAtUtcMs = UtcNowMs();
    RecalculateAggregate(job);
}

void ConsoleUploadJobAuthority::Tick()
{
    if (!artifactConsumer_) return;
    ConsoleArtifactValidationResult result;
    while (artifactConsumer_->PollCompleted(result))
    {
        const auto item = validationJobs_.find(result.generation);
        if (item == validationJobs_.end()) continue;
        Job* job = FindJob(item->second);
        if (job && job->validationGeneration == result.generation)
            CompleteValidation(*job, result);
        validationJobs_.erase(item);
    }
}

void ConsoleUploadJobAuthority::RecalculateAggregate(Job& job)
{
    auto& snapshot = job.snapshot;
    snapshot.waitingCount = snapshot.preparingCount = snapshot.uploadingCount = 0;
    snapshot.retryWaitingCount = snapshot.succeededCount = snapshot.failedCount = 0;
    snapshot.cancelledCount = 0;
    snapshot.totalBytes = snapshot.bytesCompleted = 0;
    for (const auto& route : job.routes)
    {
        snapshot.totalBytes += route.totalBytes;
        snapshot.bytesCompleted += route.bytesCompleted;
        switch (route.state)
        {
        case dspgcs::UploadRouteState::Waiting: ++snapshot.waitingCount; break;
        case dspgcs::UploadRouteState::Preparing: ++snapshot.preparingCount; break;
        case dspgcs::UploadRouteState::Uploading: ++snapshot.uploadingCount; break;
        case dspgcs::UploadRouteState::RetryWaiting: ++snapshot.retryWaitingCount; break;
        case dspgcs::UploadRouteState::Succeeded: ++snapshot.succeededCount; break;
        case dspgcs::UploadRouteState::Failed: ++snapshot.failedCount; break;
        case dspgcs::UploadRouteState::Cancelled: ++snapshot.cancelledCount; break;
        }
    }
}

dspgcs::UploadJobSnapshotDTO ConsoleUploadJobAuthority::BuildSnapshot(
    const dspgcs::UploadJobSnapshotRequestDTO& request)
{
    Job* job = FindJob(request.uploadJobId);
    if (!job)
    {
        dspgcs::UploadJobSnapshotDTO error;
        error.clientRequestId = request.clientRequestId;
        error.uploadJobId = request.uploadJobId;
        error.resultCode = dspgcs::UploadJobErrorCode::UnknownJob;
        error.safeDiagnostic = "Upload Job does not exist in this Console process.";
        return error;
    }
    job->snapshot.clientRequestId = request.clientRequestId;
    job->snapshot.snapshotToken = MakeCanonicalId();
    ++job->snapshot.snapshotSequence;
    job->snapshotTokenRevision = job->snapshot.jobRevision;
    return job->snapshot;
}

bool ConsoleUploadJobAuthority::BuildRoutePage(
    const dspgcs::UploadJobSnapshotRequestDTO& request,
    dspgcs::UploadJobRouteStatePageDTO& page,
    dspgcs::UploadJobErrorCode& errorCode)
{
    errorCode = dspgcs::UploadJobErrorCode::None;
    Job* job = FindJob(request.uploadJobId);
    if (!job)
    {
        errorCode = dspgcs::UploadJobErrorCode::UnknownJob;
        return false;
    }
    if (request.snapshotToken != job->snapshot.snapshotToken)
    {
        errorCode = dspgcs::UploadJobErrorCode::StaleJobRevision;
        return false;
    }
    if (job->pageSnapshotToken != request.snapshotToken)
    {
        if (request.routePageStart != 0 ||
            job->snapshotTokenRevision != job->snapshot.jobRevision)
        {
            errorCode = dspgcs::UploadJobErrorCode::StaleJobRevision;
            return false;
        }
        job->pageSnapshot = job->routes;
        job->pageSnapshotRevision = job->snapshotTokenRevision;
        job->pageSnapshotToken = request.snapshotToken;
    }
    if (request.routePageStart >= job->pageSnapshot.size())
    {
        errorCode = dspgcs::UploadJobErrorCode::MalformedMessage;
        return false;
    }
    page.clientRequestId = request.clientRequestId;
    page.uploadJobId = request.uploadJobId;
    page.snapshotToken = request.snapshotToken;
    page.jobRevision = job->pageSnapshotRevision;
    page.totalRouteCount = static_cast<std::uint32_t>(job->pageSnapshot.size());
    page.pageStart = request.routePageStart;
    const std::size_t fixedBytes = 135;
    std::size_t encodedBytes = fixedBytes;
    const std::size_t maximum = std::min<std::size_t>(
        request.maximumRouteEntries,
        job->pageSnapshot.size() - request.routePageStart);
    for (std::size_t offset = 0; offset < maximum; ++offset)
    {
        const auto& route = job->pageSnapshot[request.routePageStart + offset];
        const std::size_t routeBytes = dspgcs::UploadRouteStateEncodedBytes(route);
        if (encodedBytes + routeBytes > RoutePayloadBudgetBytes)
            break;
        encodedBytes += routeBytes;
        page.routes.push_back(route);
    }
    if (page.routes.empty())
    {
        errorCode = dspgcs::UploadJobErrorCode::MalformedMessage;
        return false;
    }
    page.nextPageStart = page.pageStart + static_cast<std::uint32_t>(page.routes.size());
    page.hasMore = page.nextPageStart < page.totalRouteCount;
    return true;
}

void ConsoleUploadJobAuthority::QueueRouteDeltas(
    Job& job,
    const std::vector<dspgcs::UploadRouteStateDTO>& updates)
{
    dspgcs::UploadJobRouteStateDeltaDTO batch;
    batch.uploadJobId = job.snapshot.uploadJobId;
    batch.jobRevision = job.snapshot.jobRevision;
    const std::size_t fixedBytes = 58;
    std::size_t encodedBytes = fixedBytes;
    for (const auto& update : updates)
    {
        const std::size_t routeBytes = dspgcs::UploadRouteStateEncodedBytes(update);
        if (!batch.routes.empty() && encodedBytes + routeBytes > RoutePayloadBudgetBytes)
        {
            batch.deltaSequence = ++job.nextDeltaSequence;
            pendingDeltas_.push_back(std::move(batch));
            batch = {};
            batch.uploadJobId = job.snapshot.uploadJobId;
            batch.jobRevision = job.snapshot.jobRevision;
            encodedBytes = fixedBytes;
        }
        encodedBytes += routeBytes;
        batch.routes.push_back(update);
    }
    if (!batch.routes.empty())
    {
        batch.deltaSequence = ++job.nextDeltaSequence;
        pendingDeltas_.push_back(std::move(batch));
    }
    job.snapshot.routeDeltaSequence = job.nextDeltaSequence;
}

bool ConsoleUploadJobAuthority::ApplyExecutorRouteUpdates(
    const std::string& uploadJobId,
    const std::vector<dspgcs::UploadRouteStateDTO>& updates,
    const std::uint64_t throughputBytesPerSecond,
    const std::uint32_t throughputWindowMs,
    const std::uint64_t elapsedMs,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease || updates.empty())
    {
        error = "Executor update requires a leased existing Job.";
        return false;
    }
    std::vector<std::pair<std::size_t, const dspgcs::UploadRouteStateDTO*>>
        resolvedUpdates;
    resolvedUpdates.reserve(updates.size());
    for (const auto& update : updates)
    {
        const auto item = std::lower_bound(job->routes.begin(), job->routes.end(),
            update.routeSlotIndex, [](const auto& route, const std::int32_t slot)
            { return route.routeSlotIndex < slot; });
        const auto stateValue = static_cast<std::uint8_t>(update.state);
        if (item == job->routes.end() || item->routeSlotIndex != update.routeSlotIndex ||
            update.droneId != item->droneId ||
            update.totalBytes != item->totalBytes ||
            stateValue < static_cast<std::uint8_t>(dspgcs::UploadRouteState::Waiting) ||
            stateValue > static_cast<std::uint8_t>(dspgcs::UploadRouteState::Cancelled) ||
            update.bytesCompleted > update.totalBytes)
        {
            error = "Executor route update identity is invalid.";
            return false;
        }
        resolvedUpdates.emplace_back(
            static_cast<std::size_t>(item - job->routes.begin()), &update);
    }
    auto adjustStateCount = [](dspgcs::UploadJobSnapshotDTO& snapshot,
        const dspgcs::UploadRouteState state, const bool increment)
    {
        std::uint32_t* count = nullptr;
        switch (state)
        {
        case dspgcs::UploadRouteState::Waiting: count = &snapshot.waitingCount; break;
        case dspgcs::UploadRouteState::Preparing: count = &snapshot.preparingCount; break;
        case dspgcs::UploadRouteState::Uploading: count = &snapshot.uploadingCount; break;
        case dspgcs::UploadRouteState::RetryWaiting: count = &snapshot.retryWaitingCount; break;
        case dspgcs::UploadRouteState::Succeeded: count = &snapshot.succeededCount; break;
        case dspgcs::UploadRouteState::Failed: count = &snapshot.failedCount; break;
        case dspgcs::UploadRouteState::Cancelled: count = &snapshot.cancelledCount; break;
        }
        if (increment) ++(*count); else --(*count);
    };
    for (const auto& resolved : resolvedUpdates)
    {
        auto& current = job->routes[resolved.first];
        const auto& update = *resolved.second;
        adjustStateCount(job->snapshot, current.state, false);
        adjustStateCount(job->snapshot, update.state, true);
        job->snapshot.bytesCompleted -= current.bytesCompleted;
        job->snapshot.bytesCompleted += update.bytesCompleted;
        current = update;
    }
    ++job->snapshot.jobRevision;
    job->snapshot.updatedAtUtcMs = UtcNowMs();
    job->snapshot.throughputBytesPerSecond = throughputBytesPerSecond;
    job->snapshot.throughputWindowMs = throughputWindowMs;
    job->snapshot.elapsedMs = elapsedMs;
    QueueRouteDeltas(*job, updates);
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::TransitionJobForExecutor(
    const std::string& uploadJobId,
    const dspgcs::UploadJobState newState,
    std::string& error)
{
    Job* job = FindJob(uploadJobId);
    if (!job || !job->artifactLease)
    {
        error = "Executor transition requires a leased existing Job.";
        return false;
    }
    const auto oldState = job->snapshot.jobState;
    const bool legal =
        (oldState == dspgcs::UploadJobState::Ready && newState == dspgcs::UploadJobState::Running) ||
        (oldState == dspgcs::UploadJobState::Running &&
            (newState == dspgcs::UploadJobState::Completed ||
             newState == dspgcs::UploadJobState::Failed));
    if (!legal)
    {
        error = "Executor Job transition is invalid.";
        return false;
    }
    job->snapshot.jobState = newState;
    ++job->snapshot.jobRevision;
    job->snapshot.updatedAtUtcMs = UtcNowMs();
    if (newState == dspgcs::UploadJobState::Completed)
        job->artifactLease.reset();
    error.clear();
    return true;
}

bool ConsoleUploadJobAuthority::DestroyFailedJob(const std::string& uploadJobId)
{
    const auto item = jobs_.find(uploadJobId);
    if (item == jobs_.end() || item->second.snapshot.jobState != dspgcs::UploadJobState::Failed)
        return false;
    item->second.artifactLease.reset();
    jobs_.erase(item);
    return true;
}

std::vector<dspgcs::UploadJobRouteStateDeltaDTO>
ConsoleUploadJobAuthority::DrainRouteDeltas()
{
    std::vector<dspgcs::UploadJobRouteStateDeltaDTO> result;
    result.swap(pendingDeltas_);
    return result;
}

const dspgcs::UploadJobSnapshotDTO*
ConsoleUploadJobAuthority::FindCurrentSnapshotForTest(
    const std::string& uploadJobId)
{
    Job* job = FindJob(uploadJobId);
    return job ? &job->snapshot : nullptr;
}

std::size_t ConsoleUploadJobAuthority::GetFrozenPageRouteCountForTest(
    const std::string& uploadJobId) const
{
    const auto item = jobs_.find(uploadJobId);
    return item == jobs_.end() ? 0 : item->second.pageSnapshot.size();
}

bool ConsoleUploadJobAuthority::InstallSyntheticReadyJobForTest(
    const std::string& uploadJobId,
    const dspgcs::UploadArtifactIdentityDTO& identity,
    std::vector<dspgcs::UploadRouteStateDTO> routes,
    const std::uint32_t vacantCount,
    const std::uint32_t spareCount,
    const std::uint32_t excludedCount,
    const std::uint32_t maxConcurrentUploads,
    std::string& error)
{
    if (!dspgcs::IsCanonicalUploadJobId(uploadJobId) || routes.empty() ||
        maxConcurrentUploads == 0 || FindJob(uploadJobId))
    {
        error = "Synthetic Job input is invalid.";
        return false;
    }
    auto lease = std::make_unique<dronegcs::route_artifact::ArtifactReadLease>();
    bool recovered = false;
    const auto acquire = lease->TryAcquire(
        std::filesystem::u8path(workspaceRoot_).wstring(), recovered, error);
    if (acquire != dronegcs::route_artifact::ArtifactReadLeaseAcquireResult::Acquired)
        return false;
    Job job;
    job.snapshot.uploadJobId = uploadJobId;
    job.snapshot.snapshotToken = MakeCanonicalId();
    job.snapshot.artifactIdentity = identity;
    job.snapshot.artifactGateState = dspgcs::UploadArtifactGateState::Validated;
    job.snapshot.jobState = dspgcs::UploadJobState::Ready;
    job.snapshot.jobRevision = 1;
    job.snapshot.snapshotSequence = 1;
    job.snapshot.updatedAtUtcMs = UtcNowMs();
    job.snapshot.matchedCount = static_cast<std::uint32_t>(routes.size());
    job.snapshot.vacantCount = vacantCount;
    job.snapshot.spareCount = spareCount;
    job.snapshot.excludedCount = excludedCount;
    job.snapshot.effectiveMaxConcurrentUploads = maxConcurrentUploads;
    std::sort(routes.begin(), routes.end(), [](const auto& left, const auto& right)
        { return left.routeSlotIndex < right.routeSlotIndex; });
    job.routes = std::move(routes);
    job.artifactLease = std::move(lease);
    RecalculateAggregate(job);
    jobs_.emplace(uploadJobId, std::move(job));
    error.clear();
    return true;
}
