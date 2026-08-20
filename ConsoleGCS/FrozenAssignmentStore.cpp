#include "FrozenAssignmentStore.h"

#include <utility>

void FrozenAssignmentStore::SynchronizeFleetSession(
    const std::string& currentFleetSessionId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (currentFleetSessionId_ == currentFleetSessionId)
    {
        return;
    }

    currentFleetSessionId_ = currentFleetSessionId;
    accepted_.reset();
    retiredAuthorityOrder_.clear();
    retiredAuthoritySessions_.clear();
}

std::string FrozenAssignmentStore::GetCurrentFleetSessionID() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return currentFleetSessionId_;
}

bool FrozenAssignmentStore::HasAcceptedAssignment(
    const std::string& currentFleetSessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return accepted_ &&
        currentFleetSessionId_ == currentFleetSessionId &&
        accepted_->identity.fleetSessionId == currentFleetSessionId;
}

std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>
FrozenAssignmentStore::GetAcceptedSnapshot(
    const std::string& currentFleetSessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepted_ || currentFleetSessionId_ != currentFleetSessionId ||
        accepted_->identity.fleetSessionId != currentFleetSessionId)
    {
        return {};
    }
    return accepted_;
}

bool FrozenAssignmentStore::TryGetRouteSlotForDroneID(
    const std::string& currentFleetSessionId,
    const std::string& droneId,
    std::int32_t& routeSlotIndex) const
{
    const auto snapshot = GetAcceptedSnapshot(currentFleetSessionId);
    if (!snapshot)
    {
        return false;
    }
    const auto item = snapshot->routeSlotByDroneId.find(droneId);
    if (item == snapshot->routeSlotByDroneId.end())
    {
        return false;
    }
    routeSlotIndex = item->second;
    return true;
}

bool FrozenAssignmentStore::TryGetDroneIDForRouteSlot(
    const std::string& currentFleetSessionId,
    const std::int32_t routeSlotIndex,
    std::string& droneId) const
{
    const auto snapshot = GetAcceptedSnapshot(currentFleetSessionId);
    if (!snapshot)
    {
        return false;
    }
    const auto item = snapshot->droneIdByRouteSlot.find(routeSlotIndex);
    if (item == snapshot->droneIdByRouteSlot.end())
    {
        return false;
    }
    droneId = item->second;
    return true;
}

FrozenAssignmentStoreResult FrozenAssignmentStore::EvaluateCandidate(
    const dspgcs::FrozenAssignmentIdentityDTO& identity,
    const dspgcs::FrozenAssignmentDigest& digest) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return EvaluateCandidateLocked(identity, digest);
}

FrozenAssignmentStoreResult FrozenAssignmentStore::EvaluateCandidateLocked(
    const dspgcs::FrozenAssignmentIdentityDTO& identity,
    const dspgcs::FrozenAssignmentDigest& digest) const
{
    FrozenAssignmentStoreResult result;
    if (identity.fleetSessionId != currentFleetSessionId_)
    {
        result.errorCode =
            dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch;
        return result;
    }
    if (retiredAuthoritySessions_.find(
            identity.assignmentAuthoritySessionId) !=
        retiredAuthoritySessions_.end())
    {
        result.errorCode =
            dspgcs::FrozenAssignmentErrorCode::StaleAuthoritySession;
        return result;
    }
    if (!accepted_)
    {
        result.accepted = true;
        result.changed = true;
        return result;
    }

    const auto relation = dspgcs::CompareFrozenAssignmentRevision(
        accepted_->identity,
        accepted_->canonicalPayloadDigest,
        identity,
        digest);
    switch (relation)
    {
    case dspgcs::FrozenAssignmentRevisionRelation::Stale:
        result.errorCode = dspgcs::FrozenAssignmentErrorCode::StaleRevision;
        break;
    case dspgcs::FrozenAssignmentRevisionRelation::Collision:
        result.errorCode =
            dspgcs::FrozenAssignmentErrorCode::RevisionCollision;
        break;
    case dspgcs::FrozenAssignmentRevisionRelation::DifferentFleetSession:
        result.errorCode =
            dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch;
        break;
    case dspgcs::FrozenAssignmentRevisionRelation::Idempotent:
        result.accepted = true;
        result.changed = false;
        break;
    case dspgcs::FrozenAssignmentRevisionRelation::Higher:
    case dspgcs::FrozenAssignmentRevisionRelation::DifferentAuthoritySession:
        result.accepted = true;
        result.changed = true;
        break;
    }
    return result;
}

FrozenAssignmentStoreResult FrozenAssignmentStore::Accept(
    FrozenAssignmentAcceptedSnapshot snapshot)
{
    std::lock_guard<std::mutex> lock(mutex_);
    FrozenAssignmentStoreResult result = EvaluateCandidateLocked(
        snapshot.identity,
        snapshot.canonicalPayloadDigest);
    if (!result.accepted || !result.changed)
    {
        return result;
    }

    snapshot.routeSlotByDroneId.clear();
    snapshot.droneIdByRouteSlot.clear();
    snapshot.routeSlotByDroneId.reserve(snapshot.matchedMappings.size());
    snapshot.droneIdByRouteSlot.reserve(snapshot.matchedMappings.size());
    for (const auto& mapping : snapshot.matchedMappings)
    {
        snapshot.routeSlotByDroneId.emplace(
            mapping.droneId, mapping.routeSlotIndex);
        snapshot.droneIdByRouteSlot.emplace(
            mapping.routeSlotIndex, mapping.droneId);
    }

    if (accepted_ &&
        accepted_->identity.assignmentAuthoritySessionId !=
            snapshot.identity.assignmentAuthoritySessionId)
    {
        RetireAuthorityLocked(
            accepted_->identity.assignmentAuthoritySessionId);
    }
    accepted_ = std::make_shared<const FrozenAssignmentAcceptedSnapshot>(
        std::move(snapshot));
    return result;
}

std::size_t FrozenAssignmentStore::GetRetiredAuthoritySessionCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return retiredAuthoritySessions_.size();
}

void FrozenAssignmentStore::RetireAuthorityLocked(
    const std::string& authoritySessionId)
{
    if (authoritySessionId.empty() ||
        !retiredAuthoritySessions_.insert(authoritySessionId).second)
    {
        return;
    }
    retiredAuthorityOrder_.push_back(authoritySessionId);
    while (retiredAuthorityOrder_.size() > MaxRetiredAuthoritySessions)
    {
        retiredAuthoritySessions_.erase(retiredAuthorityOrder_.front());
        retiredAuthorityOrder_.pop_front();
    }
}
