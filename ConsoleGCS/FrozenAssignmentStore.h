#pragma once

#include "DSPGCSFrozenAssignmentProtocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct FrozenAssignmentAcceptedSnapshot
{
    dspgcs::FrozenAssignmentIdentityDTO identity;
    std::uint64_t fleetRevisionAtCapture = 0;
    std::uint64_t routeDatasetGeneration = 0;
    dspgcs::FrozenAssignmentCompletionMode completionMode =
        dspgcs::FrozenAssignmentCompletionMode::Full;
    dspgcs::FrozenAssignmentFreezePolicy freezePolicy =
        dspgcs::FrozenAssignmentFreezePolicy::RequireFull;
    std::uint32_t routeSlotCount = 0;
    std::uint32_t matchedCount = 0;
    std::uint32_t spareCount = 0;
    std::uint32_t excludedCount = 0;
    std::uint32_t vacantCount = 0;
    std::int64_t frozenAtUtcMs = 0;
    dspgcs::FrozenAssignmentDigest canonicalPayloadDigest{};
    std::vector<dspgcs::FrozenAssignmentMappingDTO> matchedMappings;

    std::unordered_map<std::string, std::int32_t> routeSlotByDroneId;
    std::unordered_map<std::int32_t, std::string> droneIdByRouteSlot;
};

struct FrozenAssignmentStoreResult
{
    bool accepted = false;
    bool changed = false;
    dspgcs::FrozenAssignmentErrorCode errorCode =
        dspgcs::FrozenAssignmentErrorCode::None;
};

class FrozenAssignmentStore
{
public:
    static constexpr std::size_t MaxRetiredAuthoritySessions = 32;

    void SynchronizeFleetSession(const std::string& currentFleetSessionId);

    bool HasAcceptedAssignment(
        const std::string& currentFleetSessionId) const;
    std::shared_ptr<const FrozenAssignmentAcceptedSnapshot>
        GetAcceptedSnapshot(const std::string& currentFleetSessionId) const;
    bool TryGetRouteSlotForDroneID(
        const std::string& currentFleetSessionId,
        const std::string& droneId,
        std::int32_t& routeSlotIndex) const;
    bool TryGetDroneIDForRouteSlot(
        const std::string& currentFleetSessionId,
        std::int32_t routeSlotIndex,
        std::string& droneId) const;

    FrozenAssignmentStoreResult EvaluateCandidate(
        const dspgcs::FrozenAssignmentIdentityDTO& identity,
        const dspgcs::FrozenAssignmentDigest& digest) const;
    FrozenAssignmentStoreResult Accept(
        FrozenAssignmentAcceptedSnapshot snapshot);

    std::size_t GetRetiredAuthoritySessionCount() const;

private:
    FrozenAssignmentStoreResult EvaluateCandidateLocked(
        const dspgcs::FrozenAssignmentIdentityDTO& identity,
        const dspgcs::FrozenAssignmentDigest& digest) const;
    void RetireAuthorityLocked(const std::string& authoritySessionId);

    mutable std::mutex mutex_;
    std::string currentFleetSessionId_;
    std::shared_ptr<const FrozenAssignmentAcceptedSnapshot> accepted_;
    std::deque<std::string> retiredAuthorityOrder_;
    std::unordered_set<std::string> retiredAuthoritySessions_;
};
