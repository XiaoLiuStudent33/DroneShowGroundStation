#include "DSPGCSFrozenAssignmentProtocol.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    void Expect(const bool condition, const std::string& message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    std::string MakeDroneId(const std::size_t index)
    {
        std::ostringstream output;
        output << "Drone" << std::setw(6) << std::setfill('0') << index;
        return output.str();
    }

    dspgcs::FrozenAssignmentPayloadDTO MakePayload(
        const std::size_t mappingCount,
        const bool reverseInput = false)
    {
        dspgcs::FrozenAssignmentPayloadDTO payload;
        payload.sourceDatasetDigest.fill(0x5a);
        payload.normalizedShowOriginLatitudeE7 = 301230000;
        payload.normalizedShowOriginLongitudeE7 = 1204560000;
        payload.identity.fleetSessionId = "PRODUCTION-TEST";
        payload.identity.assignmentAuthoritySessionId =
            "11111111-2222-3333-4444-555555555555";
        payload.identity.assignmentRevision = 17;
        payload.fleetRevisionAtCapture = 42;
        payload.routeDatasetGeneration = 9;
        payload.completionMode = dspgcs::FrozenAssignmentCompletionMode::Full;
        payload.freezePolicy = dspgcs::FrozenAssignmentFreezePolicy::RequireFull;
        payload.routeSlotCount = static_cast<std::uint32_t>(mappingCount);
        payload.matchedCount = static_cast<std::uint32_t>(mappingCount);
        payload.spareCount = 3;
        payload.excludedCount = 2;
        payload.vacantCount = 0;
        payload.frozenAtUtcMs = 1786924800123;
        if (mappingCount == 0)
        {
            payload.routeSlotCount = 1;
            payload.vacantCount = 1;
            payload.completionMode =
                dspgcs::FrozenAssignmentCompletionMode::Degraded;
            payload.freezePolicy =
                dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
        }
        payload.mappings.reserve(mappingCount);
        for (std::size_t index = 0; index < mappingCount; ++index)
        {
            dspgcs::FrozenAssignmentMappingDTO mapping;
            mapping.droneId = MakeDroneId(index);
            mapping.routeSlotIndex = static_cast<std::int32_t>(index);
            payload.mappings.push_back(std::move(mapping));
        }
        if (reverseInput) std::reverse(payload.mappings.begin(), payload.mappings.end());
        return payload;
    }

    dspgcs::MessageEnvelope MakeChunkEnvelope()
    {
        dspgcs::MessageEnvelope envelope;
        envelope.messageType = dspgcs::FrozenAssignmentMessageTypes::Chunk;
        envelope.sourceId = "UE-PRODUCTION";
        envelope.sessionId = "UE-AUTHORITY-TEST";
        envelope.sequence = 1;
        envelope.timestampUtcMs = 1786924800123;
        envelope.correlationId = "TX-TEST";
        envelope.fleetRevision = 42;
        return envelope;
    }

    void VerifyExistingDSPCompatibility()
    {
        Expect(static_cast<std::uint16_t>(dspgcs::MessageType::TelemetryBatch) == 1,
            "TelemetryBatch wire value changed");
        Expect(static_cast<std::uint16_t>(dspgcs::MessageType::CommandRequest) == 2,
            "CommandRequest wire value changed");
        Expect(static_cast<std::uint16_t>(dspgcs::MessageType::CommandResultBatch) == 3,
            "CommandResultBatch wire value changed");
        Expect(dspgcs::ProtocolMajor == 1 && dspgcs::ProtocolMinor == 0 &&
            dspgcs::SchemaVersion == 5, "protocol envelope version changed");

        dspgcs::MessageEnvelope envelope;
        envelope.sourceId = "COMPAT";
        envelope.sessionId = "SESSION";
        std::vector<std::uint8_t> payloadBytes;
        dspgcs::MessageEnvelope decodedEnvelope;

        dspgcs::TelemetryBatchDTO telemetry;
        telemetry.batchId = "BATCH";
        telemetry.snapshotId = "SNAPSHOT";
        telemetry.chunkCount = 1;
        const auto telemetryFrame = dspgcs::EncodeTelemetry(envelope, telemetry);
        Expect(dspgcs::DecodeEnvelope(telemetryFrame, decodedEnvelope, payloadBytes),
            "legacy telemetry envelope no longer decodes");
        dspgcs::TelemetryBatchDTO decodedTelemetry;
        Expect(dspgcs::DecodeTelemetry(payloadBytes, decodedTelemetry),
            "legacy telemetry payload no longer decodes");

        dspgcs::CommandRequestDTO command;
        command.commandId = "COMMAND";
        command.targetDroneIds = { "Drone000001" };
        envelope.messageType = dspgcs::MessageType::CommandRequest;
        const auto commandFrame = dspgcs::EncodeCommand(envelope, command);
        Expect(dspgcs::DecodeEnvelope(commandFrame, decodedEnvelope, payloadBytes),
            "legacy command envelope no longer decodes");
        dspgcs::CommandRequestDTO decodedCommand;
        Expect(dspgcs::DecodeCommand(payloadBytes, decodedCommand),
            "legacy command payload no longer decodes");

        dspgcs::CommandResultBatchDTO result;
        result.commandId = "COMMAND";
        envelope.messageType = dspgcs::MessageType::CommandResultBatch;
        const auto resultFrame = dspgcs::EncodeCommandResult(envelope, result);
        Expect(dspgcs::DecodeEnvelope(resultFrame, decodedEnvelope, payloadBytes),
            "legacy result envelope no longer decodes");
        dspgcs::CommandResultBatchDTO decodedResult;
        Expect(dspgcs::DecodeCommandResult(payloadBytes, decodedResult),
            "legacy result payload no longer decodes");
    }

    void VerifyCanonicalCodecAndDigest(
        std::size_t& thousandBytes,
        std::size_t& tenThousandBytes)
    {
        std::string error;
        std::vector<std::uint8_t> canonical;
        auto emptyPayload = MakePayload(0);
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            emptyPayload, canonical, error), "zero-mapping payload: " + error);
        dspgcs::FrozenAssignmentPayloadDTO decodedEmpty;
        Expect(dspgcs::DecodeCanonicalFrozenAssignmentPayload(
            canonical, decodedEmpty, error), "zero-mapping decode: " + error);
        Expect(decodedEmpty.mappings.empty(), "zero mappings did not round-trip");

        auto utf8Payload = MakePayload(1);
        utf8Payload.mappings[0].droneId =
            "\xe6\x97\xa0\xe4\xba\xba\xe6\x9c\xba-\xe7\x94\xb2";
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            utf8Payload, canonical, error), "UTF-8 DroneID encode: " + error);
        dspgcs::FrozenAssignmentPayloadDTO decodedUtf8;
        Expect(dspgcs::DecodeCanonicalFrozenAssignmentPayload(
            canonical, decodedUtf8, error), "UTF-8 DroneID decode: " + error);
        Expect(decodedUtf8.mappings[0].droneId == utf8Payload.mappings[0].droneId,
            "UTF-8 DroneID bytes changed");

        auto ordered = MakePayload(1000);
        auto reversed = MakePayload(1000, true);
        std::vector<std::uint8_t> orderedBytes;
        std::vector<std::uint8_t> reversedBytes;
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            ordered, orderedBytes, error), "1000 mapping serialization: " + error);
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            reversed, reversedBytes, error), "reordered serialization: " + error);
        Expect(orderedBytes == reversedBytes,
            "mapping input order changed canonical bytes");
        thousandBytes = orderedBytes.size();

        dspgcs::FrozenAssignmentDigest firstDigest{};
        dspgcs::FrozenAssignmentDigest secondDigest{};
        Expect(dspgcs::ComputeFrozenAssignmentSha256(
            orderedBytes, firstDigest, error), "first SHA-256: " + error);
        Expect(dspgcs::ComputeFrozenAssignmentSha256(
            reversedBytes, secondDigest, error), "second SHA-256: " + error);
        Expect(firstDigest == secondDigest, "same canonical content changed SHA-256");

        auto differentDataset = ordered;
        differentDataset.sourceDatasetDigest[0] ^= 0xff;
        std::vector<std::uint8_t> differentDatasetBytes;
        dspgcs::FrozenAssignmentDigest differentDatasetDigest{};
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            differentDataset, differentDatasetBytes, error) &&
            dspgcs::ComputeFrozenAssignmentSha256(
                differentDatasetBytes, differentDatasetDigest, error) &&
            differentDatasetDigest != firstDigest,
            "SourceDatasetDigest did not bind the Frozen digest");

        auto differentOrigin = ordered;
        ++differentOrigin.normalizedShowOriginLatitudeE7;
        std::vector<std::uint8_t> differentOriginBytes;
        dspgcs::FrozenAssignmentDigest differentOriginDigest{};
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            differentOrigin, differentOriginBytes, error) &&
            dspgcs::ComputeFrozenAssignmentSha256(
                differentOriginBytes, differentOriginDigest, error) &&
            differentOriginDigest != firstDigest,
            "normalized ShowOrigin did not bind the Frozen digest");

        auto differentGeneration = ordered;
        ++differentGeneration.routeDatasetGeneration;
        Expect(differentGeneration.sourceDatasetDigest == ordered.sourceDatasetDigest &&
            differentGeneration.normalizedShowOriginLatitudeE7 ==
                ordered.normalizedShowOriginLatitudeE7 &&
            differentGeneration.normalizedShowOriginLongitudeE7 ==
                ordered.normalizedShowOriginLongitudeE7 &&
            differentGeneration.routeSlotCount == ordered.routeSlotCount,
            "runtime generation changed the stable source tuple");
        std::vector<std::uint8_t> differentGenerationBytes;
        dspgcs::FrozenAssignmentDigest differentGenerationDigest{};
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            differentGeneration, differentGenerationBytes, error) &&
            dspgcs::ComputeFrozenAssignmentSha256(
                differentGenerationBytes, differentGenerationDigest, error) &&
            differentGenerationDigest != firstDigest,
            "runtime generation no longer binds the overall Frozen digest");

        const std::vector<std::uint8_t> abc = { 'a', 'b', 'c' };
        dspgcs::FrozenAssignmentDigest abcDigest{};
        Expect(dspgcs::ComputeFrozenAssignmentSha256(abc, abcDigest, error),
            "known-vector SHA-256: " + error);
        const dspgcs::FrozenAssignmentDigest expectedAbc =
        {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
            0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
            0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
        };
        Expect(abcDigest == expectedAbc, "Windows CNG SHA-256 known vector failed");

        auto tenThousand = MakePayload(10000);
        std::vector<std::uint8_t> tenThousandCanonical;
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            tenThousand, tenThousandCanonical, error),
            "10000 mapping serialization: " + error);
        tenThousandBytes = tenThousandCanonical.size();

        auto duplicateRoute = MakePayload(2);
        duplicateRoute.mappings[1].routeSlotIndex = 0;
        Expect(!dspgcs::ValidateFrozenAssignmentMappings(duplicateRoute, error),
            "duplicate RouteSlotIndex was accepted");
        auto duplicateDrone = MakePayload(2);
        duplicateDrone.mappings[1].droneId = duplicateDrone.mappings[0].droneId;
        Expect(!dspgcs::ValidateFrozenAssignmentMappings(duplicateDrone, error),
            "duplicate DroneID was accepted");
        auto outOfRange = MakePayload(1);
        outOfRange.mappings[0].routeSlotIndex = 1;
        Expect(!dspgcs::ValidateFrozenAssignmentMappings(outOfRange, error),
            "out-of-range RouteSlotIndex was accepted");

        auto route1200 = MakePayload(1);
        route1200.routeSlotCount = 1200;
        route1200.vacantCount = 1199;
        route1200.completionMode = dspgcs::FrozenAssignmentCompletionMode::Degraded;
        route1200.freezePolicy = dspgcs::FrozenAssignmentFreezePolicy::AllowDegraded;
        route1200.mappings[0].routeSlotIndex = 1199;
        Expect(dspgcs::ValidateFrozenAssignmentMappings(route1200, error),
            "RouteSlot 1199 was rejected in a 1200-route domain");
        route1200.routeSlotCount = 1000;
        route1200.vacantCount = 999;
        Expect(!dspgcs::ValidateFrozenAssignmentMappings(route1200, error),
            "RouteSlot 1199 was accepted in a 1000-route domain");

        for (auto invalid : {
                 [] { auto value = MakePayload(1); value.sourceDatasetDigest.fill(0); return value; }(),
                 [] { auto value = MakePayload(1); value.normalizedShowOriginLatitudeE7 = -900000001; return value; }(),
                 [] { auto value = MakePayload(1); value.normalizedShowOriginLatitudeE7 = 900000001; return value; }(),
                 [] { auto value = MakePayload(1); value.normalizedShowOriginLongitudeE7 = -1800000001; return value; }(),
                 [] { auto value = MakePayload(1); value.normalizedShowOriginLongitudeE7 = 1800000001; return value; }() })
        {
            Expect(!dspgcs::ValidateFrozenAssignmentMappings(invalid, error),
                "invalid stable source identity was accepted");
        }

        std::vector<std::uint8_t> malformed = orderedBytes;
        malformed.resize(malformed.size() - 1);
        dspgcs::FrozenAssignmentPayloadDTO ignored;
        Expect(!dspgcs::DecodeCanonicalFrozenAssignmentPayload(malformed, ignored, error),
            "truncated canonical payload was accepted");
        malformed = orderedBytes;
        malformed[0] = 0;
        malformed[1] = 1;
        Expect(!dspgcs::DecodeCanonicalFrozenAssignmentPayload(malformed, ignored, error),
            "legacy Frozen payload schema v1 was accepted");
    }

    void VerifyMessageCodecs()
    {
        std::string error;
        const auto payload = MakePayload(3);
        std::vector<std::uint8_t> canonical;
        Expect(dspgcs::SerializeCanonicalFrozenAssignmentPayload(
            payload, canonical, error), "message setup canonical payload: " + error);
        dspgcs::FrozenAssignmentDigest digest{};
        Expect(dspgcs::ComputeFrozenAssignmentSha256(canonical, digest, error),
            "message setup digest: " + error);

        dspgcs::FrozenAssignmentBeginDTO begin;
        begin.transactionId = "TX-BEGIN";
        begin.sourceDatasetDigest = payload.sourceDatasetDigest;
        begin.normalizedShowOriginLatitudeE7 =
            payload.normalizedShowOriginLatitudeE7;
        begin.normalizedShowOriginLongitudeE7 =
            payload.normalizedShowOriginLongitudeE7;
        begin.routeSlotCount = payload.routeSlotCount;
        begin.identity = payload.identity;
        begin.fleetRevisionAtCapture = payload.fleetRevisionAtCapture;
        begin.routeDatasetGeneration = payload.routeDatasetGeneration;
        begin.completionMode = payload.completionMode;
        begin.freezePolicy = payload.freezePolicy;
        begin.matchedCount = payload.matchedCount;
        begin.spareCount = payload.spareCount;
        begin.excludedCount = payload.excludedCount;
        begin.vacantCount = payload.vacantCount;
        begin.frozenAtUtcMs = payload.frozenAtUtcMs;
        begin.totalPayloadBytes = canonical.size();
        begin.chunkCount = 1;
        begin.canonicalPayloadDigest = digest;
        std::vector<std::uint8_t> bytes;
        Expect(dspgcs::EncodeFrozenAssignmentBegin(begin, bytes, error),
            "Begin encode: " + error);
        dspgcs::FrozenAssignmentBeginDTO decodedBegin;
        Expect(dspgcs::DecodeFrozenAssignmentBegin(bytes, decodedBegin, error),
            "Begin decode: " + error);
        Expect(decodedBegin.transactionId == begin.transactionId &&
            decodedBegin.sourceDatasetDigest == begin.sourceDatasetDigest &&
            decodedBegin.normalizedShowOriginLatitudeE7 ==
                begin.normalizedShowOriginLatitudeE7 &&
            decodedBegin.normalizedShowOriginLongitudeE7 ==
                begin.normalizedShowOriginLongitudeE7 &&
            decodedBegin.routeSlotCount == begin.routeSlotCount &&
            decodedBegin.identity.assignmentAuthoritySessionId ==
                begin.identity.assignmentAuthoritySessionId &&
            decodedBegin.canonicalPayloadDigest == digest,
            "Begin round-trip changed fields");

        auto legacyBeginBytes = bytes;
        legacyBeginBytes[0] = 0;
        legacyBeginBytes[1] = 1;
        Expect(!dspgcs::DecodeFrozenAssignmentBegin(
            legacyBeginBytes, decodedBegin, error) &&
            error.find("Unsupported Frozen Assignment") != std::string::npos,
            "legacy Frozen Begin schema v1 was not explicitly rejected");

        dspgcs::FrozenAssignmentChunkDTO chunk;
        chunk.transactionId = "TX-CHUNK";
        chunk.chunkCount = 2;
        chunk.chunkIndex = 1;
        chunk.offset = 3;
        chunk.chunkPayloadBytes = { 4, 5, 6 };
        Expect(dspgcs::EncodeFrozenAssignmentChunk(chunk, bytes, error),
            "Chunk encode: " + error);
        dspgcs::FrozenAssignmentChunkDTO decodedChunk;
        Expect(dspgcs::DecodeFrozenAssignmentChunk(bytes, decodedChunk, error),
            "Chunk decode: " + error);
        Expect(decodedChunk.offset == 3 && decodedChunk.chunkPayloadBytes == chunk.chunkPayloadBytes,
            "Chunk round-trip changed fields");

        std::vector<std::uint8_t> malformedLength = bytes;
        const std::size_t lengthOffset = 2 + 4 + chunk.transactionId.size() + 4 + 4 + 8;
        malformedLength[lengthOffset] = 0xff;
        malformedLength[lengthOffset + 1] = 0xff;
        malformedLength[lengthOffset + 2] = 0xff;
        malformedLength[lengthOffset + 3] = 0xff;
        Expect(!dspgcs::DecodeFrozenAssignmentChunk(malformedLength, decodedChunk, error),
            "malformed Chunk length prefix was accepted");
        auto truncated = bytes;
        truncated.pop_back();
        Expect(!dspgcs::DecodeFrozenAssignmentChunk(truncated, decodedChunk, error),
            "truncated Chunk was accepted");

        dspgcs::FrozenAssignmentChunkDTO overflowing = chunk;
        overflowing.offset = dspgcs::MaxFrozenAssignmentPayloadBytes;
        Expect(!dspgcs::EncodeFrozenAssignmentChunk(overflowing, bytes, error),
            "overflowing Chunk offset plus length was accepted");

        dspgcs::FrozenAssignmentCommitDTO commit;
        commit.transactionId = "TX-COMMIT";
        commit.identity = payload.identity;
        commit.canonicalPayloadDigest = digest;
        Expect(dspgcs::EncodeFrozenAssignmentCommit(commit, bytes, error),
            "Commit encode: " + error);
        dspgcs::FrozenAssignmentCommitDTO decodedCommit;
        Expect(dspgcs::DecodeFrozenAssignmentCommit(bytes, decodedCommit, error),
            "Commit decode: " + error);
        Expect(decodedCommit.canonicalPayloadDigest == digest,
            "Commit round-trip changed digest");

        dspgcs::FrozenAssignmentAckDTO ack;
        ack.transactionId = "TX-ACK";
        ack.identity = payload.identity;
        ack.result = dspgcs::FrozenAssignmentAckResult::Accepted;
        ack.errorCode = dspgcs::FrozenAssignmentErrorCode::None;
        ack.acceptedDigest = digest;
        ack.acceptedMatchedCount = payload.matchedCount;
        Expect(dspgcs::EncodeFrozenAssignmentAck(ack, bytes, error),
            "ACK encode: " + error);
        dspgcs::FrozenAssignmentAckDTO decodedAck;
        Expect(dspgcs::DecodeFrozenAssignmentAck(bytes, decodedAck, error),
            "ACK decode: " + error);
        Expect(decodedAck.result == dspgcs::FrozenAssignmentAckResult::Accepted &&
            decodedAck.acceptedMatchedCount == payload.matchedCount,
            "ACK round-trip changed fields");

        begin.totalPayloadBytes = dspgcs::MaxFrozenAssignmentPayloadBytes + 1;
        Expect(!dspgcs::EncodeFrozenAssignmentBegin(begin, bytes, error),
            "oversized declared canonical payload was accepted");

        const auto relation = dspgcs::CompareFrozenAssignmentRevision(
            payload.identity, digest, payload.identity, digest);
        Expect(relation == dspgcs::FrozenAssignmentRevisionRelation::Idempotent,
            "equal revision and digest were not classified idempotent");
    }

    void VerifyChunkPlanner(
        const std::size_t thousandBytes,
        const std::size_t tenThousandBytes,
        std::size_t& thousandChunks,
        std::size_t& tenThousandChunks,
        std::size_t& maxChunkPayloadBytes)
    {
        std::string error;
        const auto envelope = MakeChunkEnvelope();
        const std::string transactionId = "TX-PLAN";
        dspgcs::FrozenAssignmentChunkPlan probe;
        Expect(dspgcs::PlanFrozenAssignmentChunks(
            std::vector<std::uint8_t>{ 1 },
            dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
            envelope, transactionId, probe, error), "planner probe: " + error);
        maxChunkPayloadBytes = probe.maxChunkPayloadBytes;

        dspgcs::FrozenAssignmentChunkPlan exact;
        Expect(dspgcs::PlanFrozenAssignmentChunks(
            std::vector<std::uint8_t>(maxChunkPayloadBytes, 1),
            dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
            envelope, transactionId, exact, error), "exact boundary plan: " + error);
        Expect(exact.chunks.size() == 1, "exact budget boundary did not use one chunk");

        dspgcs::FrozenAssignmentChunkPlan over;
        Expect(dspgcs::PlanFrozenAssignmentChunks(
            std::vector<std::uint8_t>(maxChunkPayloadBytes + 1, 1),
            dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
            envelope, transactionId, over, error), "over-boundary plan: " + error);
        Expect(over.chunks.size() == 2, "one byte over budget did not use two chunks");

        for (const auto item :
            { std::pair<std::size_t, std::size_t*>{ thousandBytes, &thousandChunks },
              std::pair<std::size_t, std::size_t*>{ tenThousandBytes, &tenThousandChunks } })
        {
            std::vector<std::uint8_t> source(item.first);
            for (std::size_t index = 0; index < source.size(); ++index)
                source[index] = static_cast<std::uint8_t>(index & 0xff);
            dspgcs::FrozenAssignmentChunkPlan plan;
            Expect(dspgcs::PlanFrozenAssignmentChunks(
                source, dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
                envelope, transactionId, plan, error), "scale chunk plan: " + error);
            *item.second = plan.chunks.size();
            std::vector<std::uint8_t> reassembled;
            std::size_t expectedOffset = 0;
            for (std::size_t index = 0; index < plan.chunks.size(); ++index)
            {
                const auto& planned = plan.chunks[index];
                Expect(planned.offset == expectedOffset, "chunk plan has a gap or overlap");
                Expect(planned.encodedDatagramBytes <=
                    dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes,
                    "planned datagram exceeds configured budget");
                dspgcs::FrozenAssignmentChunkDTO chunk;
                Expect(dspgcs::BuildFrozenAssignmentChunk(
                    source, plan, index, transactionId, chunk, error),
                    "build planned Chunk: " + error);
                std::vector<std::uint8_t> chunkPayload;
                Expect(dspgcs::EncodeFrozenAssignmentChunk(chunk, chunkPayload, error),
                    "encode planned Chunk: " + error);
                const auto frame = dspgcs::MakeFrame(envelope, chunkPayload);
                Expect(frame.size() == planned.encodedDatagramBytes,
                    "planned and actual datagram sizes differ");
                reassembled.insert(reassembled.end(),
                    chunk.chunkPayloadBytes.begin(), chunk.chunkPayloadBytes.end());
                expectedOffset += planned.length;
            }
            Expect(reassembled == source, "chunk reassembly differs from canonical bytes");
        }
    }
}

int RunDSPFrozenAssignmentProtocolTests()
{
    try
    {
        std::size_t thousandBytes = 0;
        std::size_t tenThousandBytes = 0;
        std::size_t thousandChunks = 0;
        std::size_t tenThousandChunks = 0;
        std::size_t maxChunkPayloadBytes = 0;
        VerifyExistingDSPCompatibility();
        VerifyCanonicalCodecAndDigest(thousandBytes, tenThousandBytes);
        VerifyMessageCodecs();
        VerifyChunkPlanner(thousandBytes, tenThousandBytes,
            thousandChunks, tenThousandChunks, maxChunkPayloadBytes);
        std::cout
            << "Frozen Assignment protocol PASS\n"
            << "mappings1000Bytes=" << thousandBytes
            << " mappings1000Chunks=" << thousandChunks << '\n'
            << "mappings10000Bytes=" << tenThousandBytes
            << " mappings10000Chunks=" << tenThousandChunks << '\n'
            << "datagramBudget=" << dspgcs::DefaultFrozenAssignmentDatagramBudgetBytes
            << " maxChunkPayloadBytes=" << maxChunkPayloadBytes << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Frozen Assignment protocol FAIL: " << error.what() << '\n';
        return 1;
    }
}
