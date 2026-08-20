#include "DSPGCSFrozenAssignmentReceiver.h"
#include "DSPGCSFrozenAssignmentProtocol.h"
#include "DSPGCSProductionTelemetryProvider.h"
#include "DSPGCSTransportFactory.h"
#include "DroneManager.h"
#include "FrozenAssignmentStore.h"
#include "FrozenAssignmentTransactionAssembler.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    using Clock = FrozenAssignmentTransactionAssembler::Clock;

    void Expect(const bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw std::runtime_error(message);
        }
    }

    std::string DroneId(const std::size_t index)
    {
        std::ostringstream stream;
        stream << "Drone" << std::setw(5) << std::setfill('0') << index;
        return stream.str();
    }

    void AddDrones(
        DroneManager& manager,
        const std::size_t count,
        const std::size_t first = 0)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            DroneSessionConfig config;
            config.droneId = DroneId(first + index);
            Expect(manager.AddDrone(config), "failed to add test DroneSession");
        }
    }

    dspgcs::FrozenAssignmentPayloadDTO MakePayload(
        const std::size_t matchedCount,
        const std::uint32_t routeSlotCount = 0,
        const std::string& fleetSessionId = "PRODUCTION-TEST",
        const std::string& authoritySessionId = "AUTHORITY-A",
        const std::uint64_t revision = 1,
        const dspgcs::FrozenAssignmentFreezePolicy policy =
            dspgcs::FrozenAssignmentFreezePolicy::RequireFull)
    {
        dspgcs::FrozenAssignmentPayloadDTO payload;
        payload.sourceDatasetDigest.fill(0x5a);
        payload.normalizedShowOriginLatitudeE7 = 301230000;
        payload.normalizedShowOriginLongitudeE7 = 1204560000;
        payload.identity.fleetSessionId = fleetSessionId;
        payload.identity.assignmentAuthoritySessionId = authoritySessionId;
        payload.identity.assignmentRevision = revision;
        payload.fleetRevisionAtCapture = 17;
        payload.routeDatasetGeneration = 9;
        payload.routeSlotCount = routeSlotCount == 0
            ? static_cast<std::uint32_t>(matchedCount)
            : routeSlotCount;
        payload.matchedCount = static_cast<std::uint32_t>(matchedCount);
        payload.spareCount = 2;
        payload.excludedCount = 1;
        payload.vacantCount = payload.routeSlotCount - payload.matchedCount;
        payload.completionMode = payload.vacantCount == 0
            ? dspgcs::FrozenAssignmentCompletionMode::Full
            : dspgcs::FrozenAssignmentCompletionMode::Degraded;
        payload.freezePolicy = policy;
        payload.frozenAtUtcMs = 123456789;
        payload.mappings.reserve(matchedCount);
        for (std::size_t index = 0; index < matchedCount; ++index)
        {
            payload.mappings.push_back({
                DroneId(index), static_cast<std::int32_t>(index) });
        }
        return payload;
    }

    std::vector<std::uint8_t> EncodeRawPayload(
        const dspgcs::FrozenAssignmentPayloadDTO& payload)
    {
        dspgcs::frozen_assignment_detail::Writer writer;
        Expect(writer.U16(payload.payloadSchemaVersion), "raw version encode");
        Expect(writer.Digest(payload.sourceDatasetDigest), "raw source digest");
        Expect(writer.I32(payload.normalizedShowOriginLatitudeE7), "raw origin latitude");
        Expect(writer.I32(payload.normalizedShowOriginLongitudeE7), "raw origin longitude");
        Expect(writer.U32(payload.routeSlotCount), "raw route count");
        Expect(dspgcs::frozen_assignment_detail::WriteIdentity(
            writer, payload.identity), "raw identity encode");
        Expect(writer.U64(payload.fleetRevisionAtCapture), "raw fleet revision");
        Expect(writer.U64(payload.routeDatasetGeneration), "raw route generation");
        Expect(writer.U8(static_cast<std::uint8_t>(payload.completionMode)),
            "raw completion");
        Expect(writer.U8(static_cast<std::uint8_t>(payload.freezePolicy)),
            "raw freeze policy");
        Expect(writer.U32(payload.matchedCount), "raw matched count");
        Expect(writer.U32(payload.spareCount), "raw spare count");
        Expect(writer.U32(payload.excludedCount), "raw excluded count");
        Expect(writer.U32(payload.vacantCount), "raw vacant count");
        Expect(writer.I64(payload.frozenAtUtcMs), "raw freeze timestamp");
        Expect(writer.U32(static_cast<std::uint32_t>(payload.mappings.size())),
            "raw mapping count");
        for (const auto& mapping : payload.mappings)
        {
            Expect(writer.String(mapping.droneId), "raw DroneID");
            Expect(writer.I32(mapping.routeSlotIndex), "raw RouteSlotIndex");
        }
        return writer.Take();
    }

    struct BuiltTransaction
    {
        dspgcs::FrozenAssignmentPayloadDTO payload;
        std::vector<std::uint8_t> canonicalBytes;
        dspgcs::FrozenAssignmentDigest digest{};
        dspgcs::FrozenAssignmentBeginDTO begin;
        std::vector<dspgcs::FrozenAssignmentChunkDTO> chunks;
        dspgcs::FrozenAssignmentCommitDTO commit;
    };

    BuiltTransaction BuildTransactionFromBytes(
        dspgcs::FrozenAssignmentPayloadDTO payload,
        std::vector<std::uint8_t> canonicalBytes,
        const std::string& transactionId,
        const std::size_t datagramBudget =
            dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes)
    {
        BuiltTransaction built;
        built.payload = std::move(payload);
        built.canonicalBytes = std::move(canonicalBytes);
        std::string error;
        Expect(dspgcs::ComputeFrozenAssignmentSha256(
            built.canonicalBytes, built.digest, error), error);

        dspgcs::MessageEnvelope chunkEnvelope;
        chunkEnvelope.messageType = dspgcs::FrozenAssignmentMessageTypes::Chunk;
        chunkEnvelope.sourceId = "UE-TEST";
        chunkEnvelope.sessionId = built.payload.identity.fleetSessionId;
        chunkEnvelope.correlationId = transactionId;
        dspgcs::FrozenAssignmentChunkPlan plan;
        Expect(dspgcs::PlanFrozenAssignmentChunks(
            built.canonicalBytes,
            datagramBudget,
            chunkEnvelope,
            transactionId,
            plan,
            error), error);

        built.begin.transactionId = transactionId;
        built.begin.sourceDatasetDigest = built.payload.sourceDatasetDigest;
        built.begin.normalizedShowOriginLatitudeE7 =
            built.payload.normalizedShowOriginLatitudeE7;
        built.begin.normalizedShowOriginLongitudeE7 =
            built.payload.normalizedShowOriginLongitudeE7;
        built.begin.routeSlotCount = built.payload.routeSlotCount;
        built.begin.identity = built.payload.identity;
        built.begin.fleetRevisionAtCapture =
            built.payload.fleetRevisionAtCapture;
        built.begin.routeDatasetGeneration =
            built.payload.routeDatasetGeneration;
        built.begin.completionMode = built.payload.completionMode;
        built.begin.freezePolicy = built.payload.freezePolicy;
        built.begin.matchedCount = built.payload.matchedCount;
        built.begin.spareCount = built.payload.spareCount;
        built.begin.excludedCount = built.payload.excludedCount;
        built.begin.vacantCount = built.payload.vacantCount;
        built.begin.frozenAtUtcMs = built.payload.frozenAtUtcMs;
        built.begin.totalPayloadBytes = built.canonicalBytes.size();
        built.begin.chunkCount = static_cast<std::uint32_t>(plan.chunks.size());
        built.begin.canonicalPayloadDigest = built.digest;

        built.chunks.resize(plan.chunks.size());
        for (std::size_t index = 0; index < plan.chunks.size(); ++index)
        {
            Expect(dspgcs::BuildFrozenAssignmentChunk(
                built.canonicalBytes,
                plan,
                index,
                transactionId,
                built.chunks[index],
                error), error);
        }
        built.commit.transactionId = transactionId;
        built.commit.identity = built.payload.identity;
        built.commit.canonicalPayloadDigest = built.digest;
        return built;
    }

    BuiltTransaction BuildTransaction(
        dspgcs::FrozenAssignmentPayloadDTO payload,
        const std::string& transactionId,
        const std::size_t datagramBudget =
            dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes)
    {
        std::vector<std::uint8_t> canonical;
        std::string error;
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            payload, canonical, error), error);
        return BuildTransactionFromBytes(
            std::move(payload),
            std::move(canonical),
            transactionId,
            datagramBudget);
    }

    FrozenAssignmentTransactionResult Assemble(
        FrozenAssignmentTransactionAssembler& assembler,
        const BuiltTransaction& transaction,
        const DroneManager& manager,
        const bool reverseChunks = false)
    {
        const auto now = Clock::now();
        auto result = assembler.Begin(
            transaction.begin,
            transaction.begin.identity.fleetSessionId,
            now);
        if (!result.succeeded)
        {
            return result;
        }
        if (reverseChunks)
        {
            for (auto item = transaction.chunks.rbegin();
                 item != transaction.chunks.rend(); ++item)
            {
                result = assembler.AddChunk(*item, now);
                if (!result.succeeded) return result;
            }
        }
        else
        {
            for (const auto& chunk : transaction.chunks)
            {
                result = assembler.AddChunk(chunk, now);
                if (!result.succeeded) return result;
            }
        }
        return assembler.Commit(
            transaction.commit,
            transaction.begin.identity.fleetSessionId,
            [&manager](const std::string& droneId)
            {
                return manager.FindDrone(droneId) != nullptr;
            },
            now);
    }

    dspgcs::TransportFrame MakeFrame(
        const dspgcs::MessageType type,
        const std::vector<std::uint8_t>& payload,
        const std::string& correlationId)
    {
        dspgcs::MessageEnvelope envelope;
        envelope.messageType = type;
        envelope.sourceId = "UE-TEST";
        envelope.sessionId = "UE-SESSION";
        envelope.sequence = 1;
        envelope.timestampUtcMs = 123456789;
        envelope.correlationId = correlationId;
        return { dspgcs::MakeFrame(envelope, payload) };
    }

    void EnqueueTransaction(
        DSPGCSFrozenAssignmentReceiver& receiver,
        const BuiltTransaction& transaction,
        const bool reverseChunks)
    {
        std::string error;
        std::vector<std::uint8_t> payload;
        Expect(dspgcs::EncodeFrozenAssignmentBegin(
            transaction.begin, payload, error), error);
        receiver.EnqueueFrame(MakeFrame(
            dspgcs::FrozenAssignmentMessageTypes::Begin,
            payload,
            transaction.begin.transactionId));

        auto enqueueChunk = [&receiver, &error](
            const dspgcs::FrozenAssignmentChunkDTO& chunk)
        {
            std::vector<std::uint8_t> chunkPayload;
            Expect(dspgcs::EncodeFrozenAssignmentChunk(
                chunk, chunkPayload, error), error);
            receiver.EnqueueFrame(MakeFrame(
                dspgcs::FrozenAssignmentMessageTypes::Chunk,
                chunkPayload,
                chunk.transactionId));
        };
        if (reverseChunks)
        {
            for (auto item = transaction.chunks.rbegin();
                 item != transaction.chunks.rend(); ++item)
                enqueueChunk(*item);
        }
        else
        {
            for (const auto& chunk : transaction.chunks)
                enqueueChunk(chunk);
        }

        Expect(dspgcs::EncodeFrozenAssignmentCommit(
            transaction.commit, payload, error), error);
        receiver.EnqueueFrame(MakeFrame(
            dspgcs::FrozenAssignmentMessageTypes::Commit,
            payload,
            transaction.commit.transactionId));
    }

    void RunTransactionAssemblyTests()
    {
        DroneManager manager;
        AddDrones(manager, 4);
        const BuiltTransaction base = BuildTransaction(
            MakePayload(4), "TX-ASSEMBLY", 180);

        {
            FrozenAssignmentTransactionAssembler assembler;
            auto result = Assemble(assembler, base, manager);
            Expect(result.succeeded && result.candidateSnapshot,
                "normal Begin/chunks/Commit did not assemble");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            auto legacy = base.begin;
            legacy.payloadSchemaVersion = 1;
            const auto result = assembler.Begin(
                legacy, "PRODUCTION-TEST", Clock::now());
            Expect(!result.succeeded &&
                result.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::UnsupportedProtocolVersion &&
                result.diagnostic.find("Unsupported Frozen Assignment") !=
                    std::string::npos,
                "legacy Frozen Begin schema v1 was not classified as unsupported");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            auto result = Assemble(assembler, base, manager, true);
            Expect(result.succeeded, "out-of-order chunks were rejected");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "duplicate test Begin failed");
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).idempotent,
                "identical duplicate Begin was not idempotent");
            Expect(assembler.AddChunk(base.chunks.front(), now).succeeded,
                "first duplicate-test chunk failed");
            Expect(assembler.AddChunk(base.chunks.front(), now).idempotent,
                "identical duplicate chunk was not idempotent");

            auto conflicting = base.chunks.front();
            conflicting.chunkPayloadBytes.front() ^= 0x01;
            const auto conflict = assembler.AddChunk(conflicting, now);
            Expect(!conflict.succeeded &&
                conflict.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::MalformedAssignment &&
                assembler.GetStagingTransactionCount() == 0,
                "conflicting duplicate chunk was not transaction-fatal");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "conflicting Begin setup failed");
            auto conflictingBegin = base.begin;
            ++conflictingBegin.spareCount;
            const auto conflict = assembler.Begin(
                conflictingBegin, "PRODUCTION-TEST", now);
            Expect(!conflict.succeeded &&
                conflict.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::MalformedAssignment &&
                assembler.GetStagingTransactionCount() == 1,
                "conflicting duplicate Begin overwrote active staging");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "missing chunk Begin failed");
            for (std::size_t index = 0; index + 1 < base.chunks.size(); ++index)
                Expect(assembler.AddChunk(base.chunks[index], now).succeeded,
                    "missing chunk setup failed");
            const auto missing = assembler.Commit(
                base.commit, "PRODUCTION-TEST",
                [&manager](const std::string& id)
                { return manager.FindDrone(id) != nullptr; }, now);
            Expect(!missing.succeeded &&
                missing.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                "missing chunk did not reject as incomplete");
        }
        {
            auto gap = base;
            gap.chunks[1].offset += 1;
            FrozenAssignmentTransactionAssembler assembler;
            const auto result = Assemble(assembler, gap, manager);
            Expect(!result.succeeded &&
                result.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                "chunk gap was accepted");
        }
        {
            auto overlap = base;
            overlap.chunks[1].offset -= 1;
            FrozenAssignmentTransactionAssembler assembler;
            const auto result = Assemble(assembler, overlap, manager);
            Expect(!result.succeeded &&
                result.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
                "chunk overlap was accepted");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "overflow test Begin failed");
            auto overflow = base.chunks.front();
            overflow.offset = base.begin.totalPayloadBytes;
            overflow.chunkPayloadBytes.push_back(0);
            const auto result = assembler.AddChunk(overflow, now);
            Expect(!result.succeeded, "offset overflow was accepted");
        }
        {
            auto hashMismatch = base;
            hashMismatch.chunks.front().chunkPayloadBytes.front() ^= 0x01;
            FrozenAssignmentTransactionAssembler assembler;
            const auto result = Assemble(assembler, hashMismatch, manager);
            Expect(!result.succeeded &&
                result.errorCode == dspgcs::FrozenAssignmentErrorCode::HashMismatch,
                "digest mismatch was accepted");
        }
        {
            auto identityMismatch = base;
            identityMismatch.commit.identity.assignmentRevision += 1;
            FrozenAssignmentTransactionAssembler assembler;
            const auto result = Assemble(assembler, identityMismatch, manager);
            Expect(!result.succeeded &&
                result.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
                "Commit identity mismatch was accepted");
        }
        {
            for (int mismatch = 0; mismatch < 4; ++mismatch)
            {
                auto stableMismatch = base;
                switch (mismatch)
                {
                case 0:
                    stableMismatch.begin.sourceDatasetDigest[0] ^= 0xff;
                    break;
                case 1:
                    ++stableMismatch.begin.normalizedShowOriginLatitudeE7;
                    break;
                case 2:
                    ++stableMismatch.begin.normalizedShowOriginLongitudeE7;
                    break;
                default:
                    ++stableMismatch.begin.routeSlotCount;
                    ++stableMismatch.begin.vacantCount;
                    stableMismatch.begin.completionMode =
                        dspgcs::FrozenAssignmentCompletionMode::Degraded;
                    stableMismatch.begin.freezePolicy =
                        dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
                    break;
                }
                FrozenAssignmentTransactionAssembler assembler;
                const auto result = Assemble(assembler, stableMismatch, manager);
                Expect(!result.succeeded &&
                    result.errorCode ==
                        dspgcs::FrozenAssignmentErrorCode::MalformedAssignment,
                    "Begin stable source mismatch was accepted");
            }
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto beforeBegin = assembler.Commit(
                base.commit, "PRODUCTION-TEST", {}, Clock::now());
            Expect(!beforeBegin.succeeded &&
                beforeBegin.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                "Commit before Begin was accepted");
            const auto unknownChunk = assembler.AddChunk(
                base.chunks.front(), Clock::now());
            Expect(!unknownChunk.succeeded &&
                unknownChunk.errorCode ==
                    dspgcs::FrozenAssignmentErrorCode::IncompleteTransaction,
                "unknown transaction chunk was accepted");
        }
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "Abort setup failed");
            Expect(assembler.Abort(base.begin.transactionId),
                "Abort did not discard staging");
            Expect(!assembler.Abort(base.begin.transactionId),
                "duplicate Abort was not harmless");
        }
        {
            FrozenAssignmentAssemblerConfig config;
            config.transactionTimeout = std::chrono::milliseconds(5);
            FrozenAssignmentTransactionAssembler assembler(config);
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "timeout setup failed");
            Expect(assembler.PruneExpiredTransactions(
                now + std::chrono::milliseconds(5)) == 1 &&
                assembler.GetStagingTransactionCount() == 0,
                "expired staging was not pruned");
        }
        {
            FrozenAssignmentAssemblerConfig config;
            config.maxStagingTransactions = 1;
            config.maxAggregateStagingBytes =
                static_cast<std::size_t>(base.begin.totalPayloadBytes);
            FrozenAssignmentTransactionAssembler assembler(config);
            const auto now = Clock::now();
            Expect(assembler.Begin(base.begin, "PRODUCTION-TEST", now).succeeded,
                "resource-bound setup failed");
            auto secondBegin = base.begin;
            secondBegin.transactionId = "TX-RESOURCE-SECOND";
            Expect(!assembler.Begin(
                secondBegin, "PRODUCTION-TEST", now).succeeded,
                "staging transaction/aggregate bound was not enforced");
        }

        std::string error;
        dspgcs::FrozenAssignmentAbortDTO abort;
        abort.transactionId = "TX-ABORT";
        std::vector<std::uint8_t> abortBytes;
        Expect(dspgcs::EncodeFrozenAssignmentAbort(abort, abortBytes, error), error);
        dspgcs::FrozenAssignmentAbortDTO decodedAbort;
        Expect(dspgcs::DecodeFrozenAssignmentAbort(
            abortBytes, decodedAbort, error) &&
            decodedAbort.transactionId == abort.transactionId,
            "Abort wire round trip failed");
    }

    FrozenAssignmentTransactionResult AssembleRaw(
        dspgcs::FrozenAssignmentPayloadDTO payload,
        const std::string& transactionId,
        const DroneManager& manager)
    {
        std::vector<std::uint8_t> rawBytes = EncodeRawPayload(payload);
        auto transaction = BuildTransactionFromBytes(
            std::move(payload), std::move(rawBytes), transactionId, 180);
        FrozenAssignmentTransactionAssembler assembler;
        return Assemble(assembler, transaction, manager);
    }

    void RunMappingValidationTests()
    {
        DroneManager manager;
        AddDrones(manager, 4);

        auto duplicateDrone = MakePayload(2);
        duplicateDrone.mappings[1].droneId = duplicateDrone.mappings[0].droneId;
        const auto duplicateDroneResult =
            AssembleRaw(duplicateDrone, "TX-DUP-DRONE", manager);
        Expect(duplicateDroneResult.errorCode ==
            dspgcs::FrozenAssignmentErrorCode::DuplicateDroneID,
            "duplicate DroneID was not rejected precisely: code=" +
            std::to_string(static_cast<std::uint16_t>(
                duplicateDroneResult.errorCode)) +
            " diagnostic=" + duplicateDroneResult.diagnostic);

        auto duplicateRoute = MakePayload(2);
        duplicateRoute.mappings[1].routeSlotIndex = 0;
        Expect(AssembleRaw(duplicateRoute, "TX-DUP-ROUTE", manager).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::DuplicateRouteSlot,
            "duplicate RouteSlotIndex was not rejected precisely");

        auto outOfRange = MakePayload(2);
        outOfRange.mappings[1].routeSlotIndex = 2;
        Expect(AssembleRaw(outOfRange, "TX-RANGE", manager).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::RouteSlotOutOfRange,
            "out-of-range RouteSlotIndex was accepted");

        auto nonCanonical = MakePayload(2);
        std::swap(nonCanonical.mappings[0], nonCanonical.mappings[1]);
        Expect(!AssembleRaw(nonCanonical, "TX-ORDER", manager).succeeded,
            "noncanonical route order was accepted");

        auto emptyDrone = MakePayload(1);
        emptyDrone.mappings[0].droneId.clear();
        Expect(!AssembleRaw(emptyDrone, "TX-EMPTY-ID", manager).succeeded,
            "empty DroneID was accepted");

        auto unknown = BuildTransaction(MakePayload(1), "TX-UNKNOWN");
        DroneManager emptyManager;
        FrozenAssignmentTransactionAssembler unknownAssembler;
        Expect(Assemble(unknownAssembler, unknown, emptyManager).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::UnknownDroneID,
            "unknown DroneID was accepted");

        auto caseChanged = MakePayload(1);
        caseChanged.mappings[0].droneId = "drone00000";
        Expect(AssembleRaw(caseChanged, "TX-CASE", manager).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::UnknownDroneID,
            "case-changed DroneID resolved unexpectedly");

        auto offlineKnown = BuildTransaction(MakePayload(1), "TX-OFFLINE");
        FrozenAssignmentTransactionAssembler offlineAssembler;
        Expect(Assemble(offlineAssembler, offlineKnown, manager).succeeded,
            "known Offline DroneSession was rejected");

        auto countMismatch = MakePayload(2, 2);
        countMismatch.matchedCount = 1;
        countMismatch.vacantCount = 1;
        countMismatch.completionMode =
            dspgcs::FrozenAssignmentCompletionMode::Degraded;
        countMismatch.freezePolicy =
            dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
        Expect(!AssembleRaw(countMismatch, "TX-COUNT", manager).succeeded,
            "mapping count mismatch was accepted");
    }

    void RunCompletionModeTests()
    {
        DroneManager manager;
        AddDrones(manager, 3);
        for (const auto policy : {
                 dspgcs::FrozenAssignmentFreezePolicy::RequireFull,
                 dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded })
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto full = BuildTransaction(
                MakePayload(3, 3, "PRODUCTION-TEST", "AUTH-FULL", 1, policy),
                policy == dspgcs::FrozenAssignmentFreezePolicy::RequireFull
                    ? "TX-FULL-REQUIRE" : "TX-FULL-ALLOW");
            Expect(Assemble(assembler, full, manager).succeeded,
                "valid Full contract was rejected");
        }

        FrozenAssignmentTransactionAssembler degradedAssembler;
        const auto degraded = BuildTransaction(
            MakePayload(2, 3, "PRODUCTION-TEST", "AUTH-DEGRADED", 1,
                dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded),
            "TX-DEGRADED");
        Expect(Assemble(degradedAssembler, degraded, manager).succeeded,
            "valid Degraded contract was rejected");

        auto invalidDegraded = MakePayload(2, 3);
        invalidDegraded.freezePolicy =
            dspgcs::FrozenAssignmentFreezePolicy::RequireFull;
        std::string error;
        std::vector<std::uint8_t> ignored;
        Expect(!dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            invalidDegraded, ignored, error),
            "Degraded + RequireFull was accepted");

        auto invalidVacant = degraded.begin;
        ++invalidVacant.vacantCount;
        FrozenAssignmentTransactionAssembler invalidVacantAssembler;
        Expect(!invalidVacantAssembler.Begin(
            invalidVacant, "PRODUCTION-TEST", Clock::now()).succeeded,
            "vacant count mismatch was accepted");

        auto zero = MakePayload(0, 1, "PRODUCTION-TEST", "AUTH-ZERO", 1,
            dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded);
        const auto zeroTransaction = BuildTransaction(zero, "TX-ZERO");
        FrozenAssignmentTransactionAssembler zeroAssembler;
        Expect(!zeroAssembler.Begin(
            zeroTransaction.begin, "PRODUCTION-TEST", Clock::now()).succeeded,
            "zero-match Frozen contract was accepted");
    }

    FrozenAssignmentAcceptedSnapshot CandidateFrom(
        const BuiltTransaction& transaction,
        const DroneManager& manager)
    {
        FrozenAssignmentTransactionAssembler assembler;
        auto result = Assemble(assembler, transaction, manager);
        Expect(result.succeeded && result.candidateSnapshot,
            "candidate snapshot assembly failed");
        return std::move(*result.candidateSnapshot);
    }

    void RunRevisionAndFleetTests()
    {
        DroneManager manager;
        AddDrones(manager, 2);
        FrozenAssignmentStore store;
        store.SynchronizeFleetSession("PRODUCTION-TEST");

        auto revision1 = BuildTransaction(
            MakePayload(2, 2, "PRODUCTION-TEST", "AUTHORITY-A", 1),
            "TX-REV-1");
        Expect(store.Accept(CandidateFrom(revision1, manager)).accepted,
            "Revision 1 initial accept failed");

        auto revision2 = BuildTransaction(
            MakePayload(2, 2, "PRODUCTION-TEST", "AUTHORITY-A", 2),
            "TX-REV-2");
        auto revision2Candidate = CandidateFrom(revision2, manager);
        Expect(store.Accept(revision2Candidate).accepted,
            "Revision 2 did not replace Revision 1");
        Expect(store.EvaluateCandidate(
            revision1.begin.identity, revision1.digest).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::StaleRevision,
            "older same-authority revision was not stale");
        const auto idempotent = store.EvaluateCandidate(
            revision2.begin.identity, revision2.digest);
        Expect(idempotent.accepted && !idempotent.changed,
            "same revision and digest was not idempotent");
        auto collisionDigest = revision2.digest;
        collisionDigest[0] ^= 0xff;
        Expect(store.EvaluateCandidate(
            revision2.begin.identity, collisionDigest).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::RevisionCollision,
            "same revision with different digest was not collision");

        auto authorityB = BuildTransaction(
            MakePayload(2, 2, "PRODUCTION-TEST", "AUTHORITY-B", 1),
            "TX-AUTH-B");
        Expect(store.Accept(CandidateFrom(authorityB, manager)).accepted,
            "new authority epoch Revision 1 did not replace old high revision");
        Expect(store.EvaluateCandidate(
            revision2.begin.identity, revision2.digest).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::StaleAuthoritySession,
            "retired authority epoch was not rejected precisely");
        Expect(store.GetRetiredAuthoritySessionCount() == 1,
            "retired authority tracking was incorrect");

        FrozenAssignmentStore failedEpochStore;
        failedEpochStore.SynchronizeFleetSession("PRODUCTION-TEST");
        Expect(failedEpochStore.Accept(CandidateFrom(revision1, manager)).accepted,
            "failed-epoch baseline accept failed");
        Expect(failedEpochStore.EvaluateCandidate(
            authorityB.begin.identity, authorityB.digest).accepted,
            "new authority was rejected before validation");
        Expect(failedEpochStore.GetRetiredAuthoritySessionCount() == 0 &&
            failedEpochStore.HasAcceptedAssignment("PRODUCTION-TEST"),
            "failed new authority transaction retired old authority early");

        const auto oldSnapshot = store.GetAcceptedSnapshot("PRODUCTION-TEST");
        Expect(oldSnapshot != nullptr, "accepted snapshot unavailable");
        store.SynchronizeFleetSession("PRODUCTION-NEW");
        Expect(!store.HasAcceptedAssignment("PRODUCTION-NEW") &&
            !store.GetAcceptedSnapshot("PRODUCTION-TEST"),
            "FleetSession change did not invalidate accepted assignment");

        FrozenAssignmentTransactionAssembler wrongFleetAssembler;
        Expect(wrongFleetAssembler.Begin(
            revision1.begin, "PRODUCTION-NEW", Clock::now()).errorCode ==
            dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch,
            "wrong FleetSession Begin was accepted");

        FrozenAssignmentTransactionAssembler changedFleetAssembler;
        const auto now = Clock::now();
        Expect(changedFleetAssembler.Begin(
            revision1.begin, "PRODUCTION-TEST", now).succeeded,
            "Fleet-change transaction Begin failed");
        for (const auto& chunk : revision1.chunks)
            Expect(changedFleetAssembler.AddChunk(chunk, now).succeeded,
                "Fleet-change transaction chunk failed");
        Expect(changedFleetAssembler.Commit(
            revision1.commit,
            "PRODUCTION-NEW",
            [&manager](const std::string& id)
            { return manager.FindDrone(id) != nullptr; },
            now).errorCode == dspgcs::FrozenAssignmentErrorCode::FleetSessionMismatch,
            "old FleetSession transaction committed after session change");
    }

    void RunAtomicityTests()
    {
        DroneManager manager;
        AddDrones(manager, 3);
        FrozenAssignmentStore store;
        store.SynchronizeFleetSession("PRODUCTION-TEST");
        const auto revision5 = BuildTransaction(
            MakePayload(2, 2, "PRODUCTION-TEST", "AUTH-ATOMIC", 5),
            "TX-ATOMIC-5");
        Expect(store.Accept(CandidateFrom(revision5, manager)).accepted,
            "atomicity baseline accept failed");
        const auto baseline = store.GetAcceptedSnapshot("PRODUCTION-TEST");
        Expect(baseline != nullptr, "atomicity baseline missing");

        auto AssertUnchanged = [&store, &baseline]()
        {
            const auto current = store.GetAcceptedSnapshot("PRODUCTION-TEST");
            Expect(current &&
                current->identity.assignmentRevision == 5 &&
                current->canonicalPayloadDigest ==
                    baseline->canonicalPayloadDigest &&
                current->matchedMappings.size() ==
                    baseline->matchedMappings.size() &&
                current->matchedMappings.front().droneId ==
                    baseline->matchedMappings.front().droneId &&
                current->matchedMappings.back().routeSlotIndex ==
                    baseline->matchedMappings.back().routeSlotIndex,
                "failed transaction mutated Accepted Store");
        };

        auto revision6 = BuildTransaction(
            MakePayload(2, 2, "PRODUCTION-TEST", "AUTH-ATOMIC", 6),
            "TX-ATOMIC-6", 180);
        {
            FrozenAssignmentTransactionAssembler assembler;
            const auto now = Clock::now();
            Expect(assembler.Begin(revision6.begin, "PRODUCTION-TEST", now).succeeded,
                "atomic missing setup");
            Expect(!assembler.Commit(revision6.commit, "PRODUCTION-TEST", {}, now).succeeded,
                "atomic missing accepted");
            AssertUnchanged();
        }
        {
            auto badHash = revision6;
            badHash.chunks.front().chunkPayloadBytes.front() ^= 1;
            FrozenAssignmentTransactionAssembler assembler;
            Expect(!Assemble(assembler, badHash, manager).succeeded,
                "atomic hash mismatch accepted");
            AssertUnchanged();
        }
        {
            DroneManager empty;
            FrozenAssignmentTransactionAssembler assembler;
            Expect(!Assemble(assembler, revision6, empty).succeeded,
                "atomic unknown Drone accepted");
            AssertUnchanged();
        }
        {
            auto duplicate = revision6.payload;
            duplicate.mappings[1].routeSlotIndex = 0;
            Expect(!AssembleRaw(duplicate, "TX-ATOMIC-DUP", manager).succeeded,
                "atomic duplicate route accepted");
            AssertUnchanged();
        }
        {
            FrozenAssignmentAssemblerConfig config;
            config.transactionTimeout = std::chrono::milliseconds(1);
            FrozenAssignmentTransactionAssembler assembler(config);
            const auto now = Clock::now();
            Expect(assembler.Begin(revision6.begin, "PRODUCTION-TEST", now).succeeded,
                "atomic timeout setup");
            assembler.PruneExpiredTransactions(now + std::chrono::milliseconds(1));
            AssertUnchanged();
        }
    }

    void RunReceiverAckTests()
    {
        DroneManager manager;
        AddDrones(manager, 3);
        FrozenAssignmentStore store;
        DSPGCSFrozenAssignmentReceiver receiver;
        std::vector<dspgcs::FrozenAssignmentAckDTO> acknowledgements;
        auto sender = [&acknowledgements](
            const dspgcs::FrozenAssignmentAckDTO& acknowledgement)
        {
            acknowledgements.push_back(acknowledgement);
            return true;
        };

        const auto success = BuildTransaction(
            MakePayload(3), "TX-ACK-SUCCESS", 180);
        EnqueueTransaction(receiver, success, true);
        auto stats = receiver.ProcessPending(
            "PRODUCTION-TEST", manager, store, sender);
        Expect(stats.transactionsAccepted == 1 &&
            stats.acknowledgementsSent == 1 &&
            acknowledgements.size() == 1,
            "successful transaction did not emit one final ACK");
        const auto& ack = acknowledgements.back();
        Expect(ack.result == dspgcs::FrozenAssignmentAckResult::Accepted &&
            ack.errorCode == dspgcs::FrozenAssignmentErrorCode::None &&
            ack.transactionId == success.begin.transactionId &&
            ack.identity.assignmentAuthoritySessionId ==
                success.begin.identity.assignmentAuthoritySessionId &&
            ack.identity.assignmentRevision ==
                success.begin.identity.assignmentRevision &&
            ack.acceptedDigest == success.digest &&
            ack.acceptedMatchedCount == 3,
            "successful ACK identity or accepted data was incorrect");

        auto repeat = success;
        repeat.begin.transactionId = "TX-ACK-REPEAT";
        repeat.commit.transactionId = repeat.begin.transactionId;
        for (auto& chunk : repeat.chunks)
            chunk.transactionId = repeat.begin.transactionId;
        EnqueueTransaction(receiver, repeat, false);
        stats = receiver.ProcessPending(
            "PRODUCTION-TEST", manager, store, sender);
        Expect(stats.transactionsAccepted == 1 &&
            acknowledgements.size() == 2 &&
            acknowledgements.back().result ==
                dspgcs::FrozenAssignmentAckResult::Accepted,
            "idempotent retransmission did not emit Accepted ACK again");

        auto unknownPayload = MakePayload(
            1, 1, "PRODUCTION-TEST", "AUTHORITY-A", 2);
        unknownPayload.mappings[0].droneId = "UNKNOWN-DRONE";
        const auto unknown = BuildTransactionFromBytes(
            unknownPayload,
            EncodeRawPayload(unknownPayload),
            "TX-ACK-UNKNOWN");
        EnqueueTransaction(receiver, unknown, false);
        stats = receiver.ProcessPending(
            "PRODUCTION-TEST", manager, store, sender);
        Expect(stats.transactionsRejected == 1 &&
            acknowledgements.back().result ==
                dspgcs::FrozenAssignmentAckResult::Rejected &&
            acknowledgements.back().errorCode ==
                dspgcs::FrozenAssignmentErrorCode::UnknownDroneID,
            "failed transaction did not emit precise NAK");
    }

    void RunProductionAckTransportTest()
    {
        dspgcs::DSPGCSTransportConfig consoleConfig;
        consoleConfig.type = dspgcs::TransportType::Loopback;
        consoleConfig.bindAddress = "127.0.0.1";
        consoleConfig.bindPort = 12611;
        consoleConfig.remoteAddress = "127.0.0.1";
        consoleConfig.remotePort = 12612;

        dspgcs::DSPGCSTransportConfig ueConfig = consoleConfig;
        ueConfig.bindPort = consoleConfig.remotePort;
        ueConfig.remotePort = consoleConfig.bindPort;
        std::string error;
        auto ueTransport = dspgcs::DSPGCSTransportFactory::CreateTransport(
            ueConfig, error);
        Expect(ueTransport != nullptr, error);
        std::mutex receivedMutex;
        std::vector<dspgcs::TransportFrame> received;
        ueTransport->Receive([&receivedMutex, &received](
            dspgcs::TransportFrame frame)
        {
            std::lock_guard<std::mutex> lock(receivedMutex);
            received.push_back(std::move(frame));
        });
        Expect(ueTransport->Start(),
            "ACK test UE-side loopback transport failed to start");

        DSPGCSProductionTelemetryProvider provider;
        Expect(provider.Start(consoleConfig, 1200, 100, error), error);
        Expect(provider.GetCurrentFleetSessionID().find("PRODUCTION-") == 0,
            "Current FleetSessionID getter returned an invalid production ID");

        dspgcs::FrozenAssignmentAckDTO acknowledgement;
        acknowledgement.transactionId = "TX-PRODUCTION-ACK";
        acknowledgement.identity.fleetSessionId =
            provider.GetCurrentFleetSessionID();
        acknowledgement.identity.assignmentAuthoritySessionId = "AUTH-ACK";
        acknowledgement.identity.assignmentRevision = 7;
        acknowledgement.result = dspgcs::FrozenAssignmentAckResult::Accepted;
        acknowledgement.errorCode = dspgcs::FrozenAssignmentErrorCode::None;
        acknowledgement.acceptedDigest.fill(0x5a);
        acknowledgement.acceptedMatchedCount = 3;
        Expect(provider.SendFrozenAssignmentAck(acknowledgement),
            "production ACK send failed");

        for (int attempt = 0; attempt < 200; ++attempt)
        {
            {
                std::lock_guard<std::mutex> lock(receivedMutex);
                if (!received.empty()) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        provider.Stop();
        ueTransport->Stop();

        dspgcs::TransportFrame frame;
        {
            std::lock_guard<std::mutex> lock(receivedMutex);
            Expect(received.size() == 1,
                "production ACK did not use one transaction-level datagram");
            frame = received.front();
        }
        dspgcs::MessageEnvelope envelope;
        std::vector<std::uint8_t> payload;
        dspgcs::FrozenAssignmentAckDTO decoded;
        Expect(dspgcs::DecodeEnvelope(frame.bytes, envelope, payload) &&
            envelope.messageType ==
                dspgcs::FrozenAssignmentMessageTypes::Ack &&
            envelope.correlationId == acknowledgement.transactionId &&
            dspgcs::DecodeFrozenAssignmentAck(payload, decoded, error) &&
            decoded.transactionId == acknowledgement.transactionId &&
            decoded.acceptedDigest == acknowledgement.acceptedDigest &&
            decoded.acceptedMatchedCount == acknowledgement.acceptedMatchedCount,
            "production ACK envelope or payload decode failed");
    }

    void RunStoreQueryTests()
    {
        DroneManager manager;
        AddDrones(manager, 2);
        FrozenAssignmentStore store;
        store.SynchronizeFleetSession("PRODUCTION-TEST");
        const auto transaction = BuildTransaction(
            MakePayload(2), "TX-QUERY");
        Expect(store.Accept(CandidateFrom(transaction, manager)).accepted,
            "query snapshot accept failed");
        std::int32_t slot = -1;
        std::string droneId;
        Expect(store.TryGetRouteSlotForDroneID(
            "PRODUCTION-TEST", DroneId(0), slot) && slot == 0,
            "DroneID to RouteSlot lookup failed");
        Expect(store.TryGetDroneIDForRouteSlot(
            "PRODUCTION-TEST", 1, droneId) && droneId == DroneId(1),
            "RouteSlot to DroneID lookup failed");
        Expect(!store.TryGetRouteSlotForDroneID(
            "PRODUCTION-TEST", "drone00000", slot),
            "store lookup was not case-sensitive");
        const auto accepted = store.GetAcceptedSnapshot("PRODUCTION-TEST");
        Expect(accepted &&
            accepted->sourceDatasetDigest == transaction.payload.sourceDatasetDigest &&
            accepted->normalizedShowOriginLatitudeE7 ==
                transaction.payload.normalizedShowOriginLatitudeE7 &&
            accepted->normalizedShowOriginLongitudeE7 ==
                transaction.payload.normalizedShowOriginLongitudeE7 &&
            accepted->routeSlotCount == transaction.payload.routeSlotCount,
            "Console accepted store lost the stable source tuple");
    }

    struct ScaleResult
    {
        std::size_t mappings = 0;
        std::size_t chunks = 0;
        long long elapsedMilliseconds = 0;
    };

    ScaleResult RunScaleTest()
    {
        constexpr std::size_t mappingCount = 10000;
        DroneManager manager;
        AddDrones(manager, mappingCount);
        const auto transaction = BuildTransaction(
            MakePayload(mappingCount), "TX-SCALE-10000");

        FrozenAssignmentStore store;
        store.SynchronizeFleetSession("PRODUCTION-TEST");
        FrozenAssignmentTransactionAssembler assembler;
        const auto start = std::chrono::steady_clock::now();
        auto result = Assemble(assembler, transaction, manager, true);
        Expect(result.succeeded && result.candidateSnapshot,
            "10K out-of-order assembly/validation failed");
        Expect(store.Accept(std::move(*result.candidateSnapshot)).accepted,
            "10K atomic store accept failed");
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        const auto accepted = store.GetAcceptedSnapshot("PRODUCTION-TEST");
        Expect(accepted && accepted->matchedMappings.size() == mappingCount,
            "10K accepted mapping count changed");
        std::int32_t lastSlot = -1;
        Expect(store.TryGetRouteSlotForDroneID(
            "PRODUCTION-TEST", DroneId(mappingCount - 1), lastSlot) &&
            lastSlot == static_cast<std::int32_t>(mappingCount - 1),
            "10K lookup correctness failed");

        return { mappingCount, transaction.chunks.size(), elapsed };
    }
}

int RunDSPFrozenAssignmentReceiverTests()
{
    try
    {
        RunTransactionAssemblyTests();
        std::cout << "PASS: Frozen transaction assembly tests\n";
        RunMappingValidationTests();
        std::cout << "PASS: Frozen mapping validation tests\n";
        RunCompletionModeTests();
        std::cout << "PASS: Full/Degraded semantic tests\n";
        RunRevisionAndFleetTests();
        std::cout << "PASS: Revision, authority epoch, and FleetSession tests\n";
        RunAtomicityTests();
        std::cout << "PASS: Frozen Accepted Store atomicity tests\n";
        RunReceiverAckTests();
        std::cout << "PASS: Frozen Receiver ACK/NAK tests\n";
        RunProductionAckTransportTest();
        std::cout << "PASS: Frozen production ACK shared UDP transport test\n";
        RunStoreQueryTests();
        std::cout << "PASS: Frozen Accepted Store query tests\n";
        const ScaleResult scale = RunScaleTest();
        std::cout
            << "PASS: Frozen 10K mappings=" << scale.mappings
            << " chunks=" << scale.chunks
            << " assemblyValidationAcceptMs=" << scale.elapsedMilliseconds
            << " lookup=unordered_map-average-O(1)"
            << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: Frozen Assignment Receiver: "
                  << error.what() << '\n';
        return 1;
    }
}
