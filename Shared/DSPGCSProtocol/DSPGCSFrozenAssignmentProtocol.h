#pragma once

#include "DSPGCSProtocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#if __has_include("Windows/WindowsHWrapper.h")
#include "Windows/WindowsHWrapper.h"
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

namespace dspgcs
{
    inline constexpr std::uint16_t FrozenAssignmentSchemaVersion = 1;

    // These values extend MessageType without changing the existing 1/2/3 wire values.
    // They remain constants until the legacy protocol header is normalized to UTF-8.
    namespace FrozenAssignmentMessageTypes
    {
        inline constexpr MessageType Begin = static_cast<MessageType>(4);
        inline constexpr MessageType Chunk = static_cast<MessageType>(5);
        inline constexpr MessageType Commit = static_cast<MessageType>(6);
        inline constexpr MessageType Ack = static_cast<MessageType>(7);
        inline constexpr MessageType Abort = static_cast<MessageType>(8);
    }

    static_assert(static_cast<std::uint16_t>(MessageType::TelemetryBatch) == 1);
    static_assert(static_cast<std::uint16_t>(MessageType::CommandRequest) == 2);
    static_assert(static_cast<std::uint16_t>(MessageType::CommandResultBatch) == 3);

    inline constexpr std::size_t FrozenAssignmentDigestBytes = 32;
    inline constexpr std::size_t DefaultFrozenAssignmentDatagramBudgetBytes = 1200;
    inline constexpr std::size_t MaxUdpDatagramPayloadBytes = 65507;

    // Wire abuse ceilings only. They are deliberately well above the 10K scale
    // target and are not fleet-size or route-size business limits.
    inline constexpr std::size_t MaxFrozenAssignmentMappings = 1000000;
    inline constexpr std::size_t MaxFrozenAssignmentPayloadBytes = 64 * 1024 * 1024;
    inline constexpr std::size_t MaxFrozenAssignmentChunkBytes = MaxUdpDatagramPayloadBytes;
    inline constexpr std::uint32_t MaxFrozenAssignmentChunks = 100000;
    inline constexpr std::size_t MaxFrozenAssignmentDiagnosticBytes = 512;

    enum class FrozenAssignmentCompletionMode : std::uint8_t
    {
        Full = 1,
        Degraded = 2
    };

    enum class FrozenAssignmentFreezePolicy : std::uint8_t
    {
        RequireFull = 1,
        AllowDegraded = 2
    };

    enum class FrozenAssignmentAckResult : std::uint8_t
    {
        Accepted = 1,
        Rejected = 2
    };

    enum class FrozenAssignmentErrorCode : std::uint16_t
    {
        None = 0,
        UnsupportedProtocolVersion = 1,
        FleetSessionMismatch = 2,
        MalformedAssignment = 3,
        IncompleteTransaction = 4,
        HashMismatch = 5,
        DuplicateDroneID = 6,
        DuplicateRouteSlot = 7,
        RouteSlotOutOfRange = 8,
        UnknownDroneID = 9,
        StaleRevision = 10,
        RevisionCollision = 11,
        InternalError = 12,
        StaleAuthoritySession = 13
    };

    enum class FrozenAssignmentRevisionRelation : std::uint8_t
    {
        DifferentFleetSession,
        DifferentAuthoritySession,
        Stale,
        Idempotent,
        Collision,
        Higher
    };

    using FrozenAssignmentDigest =
        std::array<std::uint8_t, FrozenAssignmentDigestBytes>;

    struct FrozenAssignmentIdentityDTO
    {
        std::string fleetSessionId;
        std::string assignmentAuthoritySessionId;
        std::uint64_t assignmentRevision = 0;
    };

    struct FrozenAssignmentMappingDTO
    {
        std::string droneId;
        std::int32_t routeSlotIndex = -1;
    };

    /** Canonical assignment content. Transaction identity is intentionally absent. */
    struct FrozenAssignmentPayloadDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        FrozenAssignmentIdentityDTO identity;
        std::uint64_t fleetRevisionAtCapture = 0;
        std::uint64_t routeDatasetGeneration = 0;
        FrozenAssignmentCompletionMode completionMode =
            FrozenAssignmentCompletionMode::Full;
        FrozenAssignmentFreezePolicy freezePolicy =
            FrozenAssignmentFreezePolicy::RequireFull;
        std::uint32_t routeSlotCount = 0;
        std::uint32_t matchedCount = 0;
        std::uint32_t spareCount = 0;
        std::uint32_t excludedCount = 0;
        std::uint32_t vacantCount = 0;
        std::int64_t frozenAtUtcMs = 0;
        std::vector<FrozenAssignmentMappingDTO> mappings;
    };

    struct FrozenAssignmentBeginDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        std::string transactionId;
        FrozenAssignmentIdentityDTO identity;
        std::uint64_t fleetRevisionAtCapture = 0;
        std::uint64_t routeDatasetGeneration = 0;
        FrozenAssignmentCompletionMode completionMode =
            FrozenAssignmentCompletionMode::Full;
        FrozenAssignmentFreezePolicy freezePolicy =
            FrozenAssignmentFreezePolicy::RequireFull;
        std::uint32_t routeSlotCount = 0;
        std::uint32_t matchedCount = 0;
        std::uint32_t spareCount = 0;
        std::uint32_t excludedCount = 0;
        std::uint32_t vacantCount = 0;
        std::int64_t frozenAtUtcMs = 0;
        std::uint64_t totalPayloadBytes = 0;
        std::uint32_t chunkCount = 0;
        FrozenAssignmentDigest canonicalPayloadDigest{};
    };

    struct FrozenAssignmentChunkDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        std::string transactionId;
        std::uint32_t chunkIndex = 0;
        std::uint32_t chunkCount = 0;
        std::uint64_t offset = 0;
        std::vector<std::uint8_t> chunkPayloadBytes;
    };

    struct FrozenAssignmentCommitDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        std::string transactionId;
        FrozenAssignmentIdentityDTO identity;
        FrozenAssignmentDigest canonicalPayloadDigest{};
    };

    struct FrozenAssignmentAckDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        std::string transactionId;
        FrozenAssignmentIdentityDTO identity;
        FrozenAssignmentAckResult result = FrozenAssignmentAckResult::Rejected;
        FrozenAssignmentErrorCode errorCode = FrozenAssignmentErrorCode::None;
        FrozenAssignmentDigest acceptedDigest{};
        std::uint32_t acceptedMatchedCount = 0;
        std::string diagnostic;
    };

    struct FrozenAssignmentAbortDTO
    {
        std::uint16_t payloadSchemaVersion = FrozenAssignmentSchemaVersion;
        std::string transactionId;
    };

    struct FrozenAssignmentChunkPlanEntry
    {
        std::uint32_t chunkIndex = 0;
        std::uint64_t offset = 0;
        std::uint32_t length = 0;
        std::size_t encodedDatagramBytes = 0;
    };

    struct FrozenAssignmentChunkPlan
    {
        std::size_t maxDatagramPayloadBudget = 0;
        std::size_t dspEnvelopeBytes = 0;
        std::size_t frozenChunkHeaderBytes = 0;
        std::size_t maxChunkPayloadBytes = 0;
        std::vector<FrozenAssignmentChunkPlanEntry> chunks;
    };

    namespace frozen_assignment_detail
    {
        inline bool Fail(std::string& error, const char* message)
        {
            error = message;
            return false;
        }

        inline bool IsValidUtf8(const std::string& value)
        {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(value.data());
            std::size_t index = 0;
            while (index < value.size())
            {
                const std::uint8_t first = bytes[index];
                std::size_t continuationCount = 0;
                std::uint32_t codePoint = 0;
                if (first <= 0x7f)
                {
                    ++index;
                    continue;
                }
                if ((first & 0xe0) == 0xc0)
                {
                    continuationCount = 1;
                    codePoint = first & 0x1f;
                    if (codePoint < 2) return false;
                }
                else if ((first & 0xf0) == 0xe0)
                {
                    continuationCount = 2;
                    codePoint = first & 0x0f;
                }
                else if ((first & 0xf8) == 0xf0)
                {
                    continuationCount = 3;
                    codePoint = first & 0x07;
                }
                else
                {
                    return false;
                }
                if (continuationCount > value.size() - index - 1) return false;
                for (std::size_t offset = 1; offset <= continuationCount; ++offset)
                {
                    const std::uint8_t next = bytes[index + offset];
                    if ((next & 0xc0) != 0x80) return false;
                    codePoint = (codePoint << 6) | (next & 0x3f);
                }
                if ((continuationCount == 2 && codePoint < 0x800) ||
                    (continuationCount == 3 && codePoint < 0x10000) ||
                    codePoint > 0x10ffff ||
                    (codePoint >= 0xd800 && codePoint <= 0xdfff))
                {
                    return false;
                }
                index += continuationCount + 1;
            }
            return true;
        }

        class Writer
        {
        public:
            bool U8(const std::uint8_t value) { return Integral(value); }
            bool U16(const std::uint16_t value) { return Integral(value); }
            bool U32(const std::uint32_t value) { return Integral(value); }
            bool I32(const std::int32_t value)
            {
                return U32(static_cast<std::uint32_t>(value));
            }
            bool U64(const std::uint64_t value) { return Integral(value); }
            bool I64(const std::int64_t value)
            {
                return U64(static_cast<std::uint64_t>(value));
            }
            bool String(const std::string& value, const std::size_t limit = MaxStringBytes)
            {
                if (value.size() > limit ||
                    value.size() > std::numeric_limits<std::uint32_t>::max() ||
                    !CanAppend(sizeof(std::uint32_t) + value.size()))
                {
                    return false;
                }
                U32(static_cast<std::uint32_t>(value.size()));
                data.insert(data.end(), value.begin(), value.end());
                return true;
            }
            bool Bytes(const std::vector<std::uint8_t>& value, const std::size_t limit)
            {
                if (value.size() > limit ||
                    value.size() > std::numeric_limits<std::uint32_t>::max() ||
                    !CanAppend(sizeof(std::uint32_t) + value.size()))
                {
                    return false;
                }
                U32(static_cast<std::uint32_t>(value.size()));
                data.insert(data.end(), value.begin(), value.end());
                return true;
            }
            bool Digest(const FrozenAssignmentDigest& value)
            {
                if (!CanAppend(value.size())) return false;
                data.insert(data.end(), value.begin(), value.end());
                return true;
            }
            const std::vector<std::uint8_t>& Data() const { return data; }
            std::vector<std::uint8_t> Take() { return std::move(data); }

        private:
            template<typename T>
            bool Integral(const T value)
            {
                if (!CanAppend(sizeof(T))) return false;
                for (int shift = static_cast<int>(sizeof(T) - 1) * 8;
                     shift >= 0;
                     shift -= 8)
                {
                    data.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
                }
                return true;
            }
            bool CanAppend(const std::size_t count) const
            {
                return count <= MaxFrozenAssignmentPayloadBytes - data.size();
            }
            std::vector<std::uint8_t> data;
        };

        class Reader
        {
        public:
            explicit Reader(const std::vector<std::uint8_t>& value) : data(value) {}
            bool U8(std::uint8_t& value) { return Integral(value); }
            bool U16(std::uint16_t& value) { return Integral(value); }
            bool U32(std::uint32_t& value) { return Integral(value); }
            bool I32(std::int32_t& value)
            {
                std::uint32_t raw = 0;
                if (!U32(raw)) return false;
                value = static_cast<std::int32_t>(raw);
                return true;
            }
            bool U64(std::uint64_t& value) { return Integral(value); }
            bool I64(std::int64_t& value)
            {
                std::uint64_t raw = 0;
                if (!U64(raw)) return false;
                value = static_cast<std::int64_t>(raw);
                return true;
            }
            bool String(std::string& value, const std::size_t limit = MaxStringBytes)
            {
                std::uint32_t length = 0;
                if (!U32(length) || length > limit || !Need(length)) return false;
                value.assign(reinterpret_cast<const char*>(data.data() + offset), length);
                offset += length;
                return true;
            }
            bool Bytes(std::vector<std::uint8_t>& value, const std::size_t limit)
            {
                std::uint32_t length = 0;
                if (!U32(length) || length > limit || !Need(length)) return false;
                value.assign(data.begin() + offset, data.begin() + offset + length);
                offset += length;
                return true;
            }
            bool Digest(FrozenAssignmentDigest& value)
            {
                if (!Need(value.size())) return false;
                std::copy_n(data.begin() + offset, value.size(), value.begin());
                offset += value.size();
                return true;
            }
            bool Done() const { return offset == data.size(); }
            std::size_t Remaining() const { return data.size() - offset; }

        private:
            bool Need(const std::size_t count) const
            {
                return count <= data.size() - offset;
            }
            template<typename T>
            bool Integral(T& value)
            {
                if (!Need(sizeof(T))) return false;
                value = 0;
                for (std::size_t index = 0; index < sizeof(T); ++index)
                {
                    value = static_cast<T>((value << 8) | data[offset++]);
                }
                return true;
            }
            const std::vector<std::uint8_t>& data;
            std::size_t offset = 0;
        };

        inline bool WriteIdentity(Writer& writer, const FrozenAssignmentIdentityDTO& identity)
        {
            return writer.String(identity.fleetSessionId) &&
                writer.String(identity.assignmentAuthoritySessionId) &&
                writer.U64(identity.assignmentRevision);
        }

        inline bool ReadIdentity(Reader& reader, FrozenAssignmentIdentityDTO& identity)
        {
            return reader.String(identity.fleetSessionId) &&
                reader.String(identity.assignmentAuthoritySessionId) &&
                reader.U64(identity.assignmentRevision);
        }

        inline bool ValidateIdentity(
            const FrozenAssignmentIdentityDTO& identity,
            std::string& error)
        {
            if (identity.fleetSessionId.empty() || identity.assignmentAuthoritySessionId.empty())
                return Fail(error, "Assignment identity contains an empty session ID.");
            if (identity.fleetSessionId.size() > MaxStringBytes ||
                identity.assignmentAuthoritySessionId.size() > MaxStringBytes)
                return Fail(error, "Assignment identity exceeds the string safety ceiling.");
            if (!IsValidUtf8(identity.fleetSessionId) ||
                !IsValidUtf8(identity.assignmentAuthoritySessionId))
                return Fail(error, "Assignment identity is not valid UTF-8.");
            if (identity.assignmentRevision == 0)
                return Fail(error, "Assignment revision must be greater than zero.");
            return true;
        }

        inline bool ValidateVersion(const std::uint16_t version, std::string& error)
        {
            return version == FrozenAssignmentSchemaVersion ||
                Fail(error, "Unsupported Frozen Assignment payload schema version.");
        }

        inline bool ValidateTransactionId(const std::string& value, std::string& error)
        {
            if (value.empty() || value.size() > MaxStringBytes || !IsValidUtf8(value))
                return Fail(error, "TransactionID is empty, oversized, or not valid UTF-8.");
            return true;
        }

        inline bool ValidateAuditEnums(
            const FrozenAssignmentCompletionMode completion,
            const FrozenAssignmentFreezePolicy policy,
            std::string& error)
        {
            if (completion != FrozenAssignmentCompletionMode::Full &&
                completion != FrozenAssignmentCompletionMode::Degraded)
                return Fail(error, "Frozen Assignment completion mode is invalid.");
            if (policy != FrozenAssignmentFreezePolicy::RequireFull &&
                policy != FrozenAssignmentFreezePolicy::AllowDegraded)
                return Fail(error, "Frozen Assignment freeze policy is invalid.");
            if (completion == FrozenAssignmentCompletionMode::Degraded &&
                policy != FrozenAssignmentFreezePolicy::AllowDegraded)
                return Fail(error, "Degraded completion requires AllowDegraded policy.");
            return true;
        }
    }

    inline bool ValidateFrozenAssignmentMappings(
        const FrozenAssignmentPayloadDTO& payload,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        error.clear();
        if (!ValidateVersion(payload.payloadSchemaVersion, error) ||
            !ValidateIdentity(payload.identity, error) ||
            !ValidateAuditEnums(payload.completionMode, payload.freezePolicy, error))
            return false;
        if (payload.routeDatasetGeneration == 0)
            return Fail(error, "RouteDatasetGeneration must be greater than zero.");
        if (payload.mappings.size() > MaxFrozenAssignmentMappings ||
            payload.mappings.size() > std::numeric_limits<std::uint32_t>::max())
            return Fail(error, "Mapping count exceeds the wire safety ceiling.");
        if (payload.matchedCount != payload.mappings.size())
            return Fail(error, "MatchedCount does not equal the mapping array size.");
        if (payload.matchedCount > payload.routeSlotCount ||
            payload.vacantCount > payload.routeSlotCount ||
            static_cast<std::uint64_t>(payload.matchedCount) + payload.vacantCount !=
                payload.routeSlotCount)
            return Fail(error, "Matched and vacant counts do not cover RouteSlotCount.");
        if (payload.completionMode == FrozenAssignmentCompletionMode::Full &&
            payload.vacantCount != 0)
            return Fail(error, "Full completion cannot contain vacant route slots.");
        if (payload.completionMode == FrozenAssignmentCompletionMode::Degraded &&
            payload.vacantCount == 0)
            return Fail(error, "Degraded completion requires at least one vacant route slot.");

        std::unordered_set<std::string> droneIds;
        std::unordered_set<std::int32_t> routeSlots;
        droneIds.reserve(payload.mappings.size());
        routeSlots.reserve(payload.mappings.size());
        for (const FrozenAssignmentMappingDTO& mapping : payload.mappings)
        {
            if (mapping.droneId.empty() || mapping.droneId.size() > MaxStringBytes ||
                !IsValidUtf8(mapping.droneId))
                return Fail(error, "A DroneID is empty, oversized, or not valid UTF-8.");
            if (mapping.routeSlotIndex < 0 ||
                static_cast<std::uint64_t>(mapping.routeSlotIndex) >= payload.routeSlotCount)
                return Fail(error, "A RouteSlotIndex is outside the route domain.");
            if (!droneIds.insert(mapping.droneId).second)
                return Fail(error, "Duplicate DroneID in Frozen Assignment mappings.");
            if (!routeSlots.insert(mapping.routeSlotIndex).second)
                return Fail(error, "Duplicate RouteSlotIndex in Frozen Assignment mappings.");
        }
        return true;
    }

    inline bool SerializeCanonicalFrozenAssignmentPayload(
        const FrozenAssignmentPayloadDTO& input,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear();
        FrozenAssignmentPayloadDTO payload = input;
        std::sort(payload.mappings.begin(), payload.mappings.end(),
            [](const FrozenAssignmentMappingDTO& left, const FrozenAssignmentMappingDTO& right)
            {
                if (left.routeSlotIndex != right.routeSlotIndex)
                    return left.routeSlotIndex < right.routeSlotIndex;
                return left.droneId < right.droneId;
            });
        if (!ValidateFrozenAssignmentMappings(payload, error)) return false;

        Writer writer;
        if (!writer.U16(payload.payloadSchemaVersion) ||
            !WriteIdentity(writer, payload.identity) ||
            !writer.U64(payload.fleetRevisionAtCapture) ||
            !writer.U64(payload.routeDatasetGeneration) ||
            !writer.U8(static_cast<std::uint8_t>(payload.completionMode)) ||
            !writer.U8(static_cast<std::uint8_t>(payload.freezePolicy)) ||
            !writer.U32(payload.routeSlotCount) ||
            !writer.U32(payload.matchedCount) ||
            !writer.U32(payload.spareCount) ||
            !writer.U32(payload.excludedCount) ||
            !writer.U32(payload.vacantCount) ||
            !writer.I64(payload.frozenAtUtcMs) ||
            !writer.U32(static_cast<std::uint32_t>(payload.mappings.size())))
            return Fail(error, "Canonical Frozen Assignment header exceeds safety bounds.");
        for (const FrozenAssignmentMappingDTO& mapping : payload.mappings)
        {
            if (!writer.String(mapping.droneId) || !writer.I32(mapping.routeSlotIndex))
                return Fail(error, "Canonical Frozen Assignment mapping exceeds safety bounds.");
        }
        output = writer.Take();
        return true;
    }

    inline bool DecodeCanonicalFrozenAssignmentPayload(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentPayloadDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {};
        error.clear();
        if (bytes.size() > MaxFrozenAssignmentPayloadBytes)
            return Fail(error, "Canonical Frozen Assignment payload exceeds its safety ceiling.");
        Reader reader(bytes);
        std::uint8_t completion = 0;
        std::uint8_t policy = 0;
        std::uint32_t mappingCount = 0;
        if (!reader.U16(output.payloadSchemaVersion))
            return Fail(error, "Canonical Frozen Assignment payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!ReadIdentity(reader, output.identity) ||
            !reader.U64(output.fleetRevisionAtCapture) ||
            !reader.U64(output.routeDatasetGeneration) ||
            !reader.U8(completion) || !reader.U8(policy) ||
            !reader.U32(output.routeSlotCount) || !reader.U32(output.matchedCount) ||
            !reader.U32(output.spareCount) || !reader.U32(output.excludedCount) ||
            !reader.U32(output.vacantCount) || !reader.I64(output.frozenAtUtcMs) ||
            !reader.U32(mappingCount))
            return Fail(error, "Canonical Frozen Assignment payload is truncated.");
        if (mappingCount > MaxFrozenAssignmentMappings ||
            mappingCount > reader.Remaining() / 8)
            return Fail(error, "Declared mapping count exceeds available bytes or safety bounds.");
        output.completionMode = static_cast<FrozenAssignmentCompletionMode>(completion);
        output.freezePolicy = static_cast<FrozenAssignmentFreezePolicy>(policy);
        output.mappings.reserve(mappingCount);
        for (std::uint32_t index = 0; index < mappingCount; ++index)
        {
            FrozenAssignmentMappingDTO mapping;
            if (!reader.String(mapping.droneId) || !reader.I32(mapping.routeSlotIndex))
                return Fail(error, "Canonical Frozen Assignment mapping is truncated or oversized.");
            output.mappings.push_back(std::move(mapping));
        }
        if (!reader.Done())
            return Fail(error, "Canonical Frozen Assignment payload has trailing bytes.");
        if (!ValidateFrozenAssignmentMappings(output, error)) return false;
        for (std::size_t index = 1; index < output.mappings.size(); ++index)
        {
            if (output.mappings[index - 1].routeSlotIndex >=
                output.mappings[index].routeSlotIndex)
                return Fail(error, "Canonical mapping order is not strictly RouteSlotIndex ascending.");
        }
        return true;
    }

    inline bool ComputeFrozenAssignmentSha256(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentDigest& digest,
        std::string& error)
    {
        digest.fill(0);
        error.clear();
        if (bytes.size() > MaxFrozenAssignmentPayloadBytes)
            return frozen_assignment_detail::Fail(error, "SHA-256 input exceeds the payload safety ceiling.");
#if defined(_WIN32)
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<std::uint8_t> hashObject;
        DWORD objectBytes = 0;
        DWORD hashBytes = 0;
        DWORD returnedBytes = 0;
        NTSTATUS status = BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status >= 0)
            status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returnedBytes, 0);
        if (status >= 0)
            status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes), &returnedBytes, 0);
        if (status >= 0 && hashBytes != FrozenAssignmentDigestBytes)
            status = static_cast<NTSTATUS>(-1);
        if (status >= 0)
        {
            hashObject.resize(objectBytes);
            status = BCryptCreateHash(algorithm, &hash,
                hashObject.data(), objectBytes, nullptr, 0, 0);
        }
        if (status >= 0 && !bytes.empty())
            status = BCryptHashData(hash,
                const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0);
        if (status >= 0)
            status = BCryptFinishHash(hash, digest.data(),
                static_cast<ULONG>(digest.size()), 0);
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status < 0)
            return frozen_assignment_detail::Fail(error, "Windows CNG SHA-256 failed.");
        return true;
#else
        return frozen_assignment_detail::Fail(
            error, "No production SHA-256 provider is configured for this platform.");
#endif
    }

    inline FrozenAssignmentRevisionRelation CompareFrozenAssignmentRevision(
        const FrozenAssignmentIdentityDTO& activeIdentity,
        const FrozenAssignmentDigest& activeDigest,
        const FrozenAssignmentIdentityDTO& candidateIdentity,
        const FrozenAssignmentDigest& candidateDigest)
    {
        if (activeIdentity.fleetSessionId != candidateIdentity.fleetSessionId)
            return FrozenAssignmentRevisionRelation::DifferentFleetSession;
        if (activeIdentity.assignmentAuthoritySessionId !=
            candidateIdentity.assignmentAuthoritySessionId)
            return FrozenAssignmentRevisionRelation::DifferentAuthoritySession;
        if (candidateIdentity.assignmentRevision < activeIdentity.assignmentRevision)
            return FrozenAssignmentRevisionRelation::Stale;
        if (candidateIdentity.assignmentRevision > activeIdentity.assignmentRevision)
            return FrozenAssignmentRevisionRelation::Higher;
        return activeDigest == candidateDigest
            ? FrozenAssignmentRevisionRelation::Idempotent
            : FrozenAssignmentRevisionRelation::Collision;
    }

    inline bool ValidateFrozenAssignmentBegin(
        const FrozenAssignmentBeginDTO& value,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        error.clear();
        if (!ValidateVersion(value.payloadSchemaVersion, error) ||
            !ValidateTransactionId(value.transactionId, error) ||
            !ValidateIdentity(value.identity, error) ||
            !ValidateAuditEnums(value.completionMode, value.freezePolicy, error)) return false;
        if (value.routeDatasetGeneration == 0)
            return Fail(error, "Begin RouteDatasetGeneration must be greater than zero.");
        if (value.matchedCount > MaxFrozenAssignmentMappings ||
            value.matchedCount > value.routeSlotCount ||
            value.vacantCount > value.routeSlotCount ||
            static_cast<std::uint64_t>(value.matchedCount) + value.vacantCount !=
                value.routeSlotCount)
            return Fail(error, "Begin matched and vacant counts do not cover RouteSlotCount.");
        if (value.completionMode == FrozenAssignmentCompletionMode::Full &&
            value.vacantCount != 0)
            return Fail(error, "Begin Full completion cannot contain vacant route slots.");
        if (value.completionMode == FrozenAssignmentCompletionMode::Degraded &&
            value.vacantCount == 0)
            return Fail(error, "Begin Degraded completion requires a vacant route slot.");
        if (value.totalPayloadBytes == 0 ||
            value.totalPayloadBytes > MaxFrozenAssignmentPayloadBytes ||
            value.chunkCount == 0 || value.chunkCount > MaxFrozenAssignmentChunks)
            return Fail(error, "Begin payload size or chunk count exceeds safety bounds.");
        return true;
    }

    inline bool EncodeFrozenAssignmentBegin(
        const FrozenAssignmentBeginDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear(); error.clear();
        if (!ValidateFrozenAssignmentBegin(value, error)) return false;
        Writer writer;
        if (!writer.U16(value.payloadSchemaVersion) || !writer.String(value.transactionId) ||
            !WriteIdentity(writer, value.identity) ||
            !writer.U64(value.fleetRevisionAtCapture) ||
            !writer.U64(value.routeDatasetGeneration) ||
            !writer.U8(static_cast<std::uint8_t>(value.completionMode)) ||
            !writer.U8(static_cast<std::uint8_t>(value.freezePolicy)) ||
            !writer.U32(value.routeSlotCount) || !writer.U32(value.matchedCount) ||
            !writer.U32(value.spareCount) || !writer.U32(value.excludedCount) ||
            !writer.U32(value.vacantCount) || !writer.I64(value.frozenAtUtcMs) ||
            !writer.U64(value.totalPayloadBytes) || !writer.U32(value.chunkCount) ||
            !writer.Digest(value.canonicalPayloadDigest))
            return Fail(error, "Begin payload exceeds safety bounds.");
        output = writer.Take();
        return true;
    }

    inline bool DecodeFrozenAssignmentBegin(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentBeginDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear(); Reader reader(bytes);
        std::uint8_t completion = 0, policy = 0;
        if (!reader.U16(output.payloadSchemaVersion)) return Fail(error, "Begin payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!reader.String(output.transactionId) || !ReadIdentity(reader, output.identity) ||
            !reader.U64(output.fleetRevisionAtCapture) ||
            !reader.U64(output.routeDatasetGeneration) ||
            !reader.U8(completion) || !reader.U8(policy) ||
            !reader.U32(output.routeSlotCount) || !reader.U32(output.matchedCount) ||
            !reader.U32(output.spareCount) || !reader.U32(output.excludedCount) ||
            !reader.U32(output.vacantCount) || !reader.I64(output.frozenAtUtcMs) ||
            !reader.U64(output.totalPayloadBytes) || !reader.U32(output.chunkCount) ||
            !reader.Digest(output.canonicalPayloadDigest) || !reader.Done())
            return Fail(error, "Begin payload is malformed or truncated.");
        output.completionMode = static_cast<FrozenAssignmentCompletionMode>(completion);
        output.freezePolicy = static_cast<FrozenAssignmentFreezePolicy>(policy);
        return ValidateFrozenAssignmentBegin(output, error);
    }

    inline bool EncodeFrozenAssignmentChunk(
        const FrozenAssignmentChunkDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear(); error.clear();
        if (!ValidateVersion(value.payloadSchemaVersion, error) ||
            !ValidateTransactionId(value.transactionId, error)) return false;
        if (value.chunkCount == 0 || value.chunkCount > MaxFrozenAssignmentChunks ||
            value.chunkIndex >= value.chunkCount ||
            value.chunkPayloadBytes.size() > MaxFrozenAssignmentChunkBytes ||
            value.offset > MaxFrozenAssignmentPayloadBytes ||
            value.chunkPayloadBytes.size() > MaxFrozenAssignmentPayloadBytes - value.offset)
            return Fail(error, "Chunk index, offset, or size exceeds safety bounds.");
        Writer writer;
        if (!writer.U16(value.payloadSchemaVersion) || !writer.String(value.transactionId) ||
            !writer.U32(value.chunkIndex) || !writer.U32(value.chunkCount) ||
            !writer.U64(value.offset) ||
            !writer.Bytes(value.chunkPayloadBytes, MaxFrozenAssignmentChunkBytes))
            return Fail(error, "Chunk payload exceeds safety bounds.");
        output = writer.Take();
        return true;
    }

    inline bool DecodeFrozenAssignmentChunk(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentChunkDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear(); Reader reader(bytes);
        if (!reader.U16(output.payloadSchemaVersion)) return Fail(error, "Chunk payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!reader.String(output.transactionId) || !reader.U32(output.chunkIndex) ||
            !reader.U32(output.chunkCount) || !reader.U64(output.offset) ||
            !reader.Bytes(output.chunkPayloadBytes, MaxFrozenAssignmentChunkBytes) || !reader.Done())
            return Fail(error, "Chunk payload is malformed or truncated.");
        if (!ValidateTransactionId(output.transactionId, error)) return false;
        if (output.chunkCount == 0 || output.chunkCount > MaxFrozenAssignmentChunks ||
            output.chunkIndex >= output.chunkCount ||
            output.offset > MaxFrozenAssignmentPayloadBytes ||
            output.chunkPayloadBytes.size() > MaxFrozenAssignmentPayloadBytes - output.offset)
            return Fail(error, "Chunk index, offset, or size exceeds safety bounds.");
        return true;
    }

    inline bool EncodeFrozenAssignmentCommit(
        const FrozenAssignmentCommitDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear(); error.clear();
        if (!ValidateVersion(value.payloadSchemaVersion, error) ||
            !ValidateTransactionId(value.transactionId, error) ||
            !ValidateIdentity(value.identity, error)) return false;
        Writer writer;
        if (!writer.U16(value.payloadSchemaVersion) || !writer.String(value.transactionId) ||
            !WriteIdentity(writer, value.identity) ||
            !writer.Digest(value.canonicalPayloadDigest))
            return Fail(error, "Commit payload exceeds safety bounds.");
        output = writer.Take(); return true;
    }

    inline bool DecodeFrozenAssignmentCommit(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentCommitDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear(); Reader reader(bytes);
        if (!reader.U16(output.payloadSchemaVersion)) return Fail(error, "Commit payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!reader.String(output.transactionId) || !ReadIdentity(reader, output.identity) ||
            !reader.Digest(output.canonicalPayloadDigest) || !reader.Done())
            return Fail(error, "Commit payload is malformed or truncated.");
        return ValidateTransactionId(output.transactionId, error) &&
            ValidateIdentity(output.identity, error);
    }

    inline bool EncodeFrozenAssignmentAck(
        const FrozenAssignmentAckDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear(); error.clear();
        if (!ValidateVersion(value.payloadSchemaVersion, error) ||
            !ValidateTransactionId(value.transactionId, error) ||
            !ValidateIdentity(value.identity, error)) return false;
        if ((value.result != FrozenAssignmentAckResult::Accepted &&
             value.result != FrozenAssignmentAckResult::Rejected) ||
            (value.result == FrozenAssignmentAckResult::Accepted &&
             value.errorCode != FrozenAssignmentErrorCode::None) ||
            (value.result == FrozenAssignmentAckResult::Rejected &&
             value.errorCode == FrozenAssignmentErrorCode::None) ||
            static_cast<std::uint16_t>(value.errorCode) >
                static_cast<std::uint16_t>(FrozenAssignmentErrorCode::StaleAuthoritySession) ||
            value.diagnostic.size() > MaxFrozenAssignmentDiagnosticBytes ||
            !IsValidUtf8(value.diagnostic))
            return Fail(error, "ACK result, error code, or diagnostic is invalid.");
        Writer writer;
        if (!writer.U16(value.payloadSchemaVersion) || !writer.String(value.transactionId) ||
            !WriteIdentity(writer, value.identity) ||
            !writer.U8(static_cast<std::uint8_t>(value.result)) ||
            !writer.U16(static_cast<std::uint16_t>(value.errorCode)) ||
            !writer.Digest(value.acceptedDigest) ||
            !writer.U32(value.acceptedMatchedCount) ||
            !writer.String(value.diagnostic, MaxFrozenAssignmentDiagnosticBytes))
            return Fail(error, "ACK payload exceeds safety bounds.");
        output = writer.Take(); return true;
    }

    inline bool DecodeFrozenAssignmentAck(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentAckDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear(); Reader reader(bytes);
        std::uint8_t result = 0; std::uint16_t errorCode = 0;
        if (!reader.U16(output.payloadSchemaVersion)) return Fail(error, "ACK payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!reader.String(output.transactionId) || !ReadIdentity(reader, output.identity) ||
            !reader.U8(result) || !reader.U16(errorCode) ||
            !reader.Digest(output.acceptedDigest) ||
            !reader.U32(output.acceptedMatchedCount) ||
            !reader.String(output.diagnostic, MaxFrozenAssignmentDiagnosticBytes) || !reader.Done())
            return Fail(error, "ACK payload is malformed or truncated.");
        output.result = static_cast<FrozenAssignmentAckResult>(result);
        output.errorCode = static_cast<FrozenAssignmentErrorCode>(errorCode);
        if (!ValidateTransactionId(output.transactionId, error) ||
            !ValidateIdentity(output.identity, error)) return false;
        if ((output.result != FrozenAssignmentAckResult::Accepted &&
             output.result != FrozenAssignmentAckResult::Rejected) ||
            (output.result == FrozenAssignmentAckResult::Accepted &&
             output.errorCode != FrozenAssignmentErrorCode::None) ||
            (output.result == FrozenAssignmentAckResult::Rejected &&
             output.errorCode == FrozenAssignmentErrorCode::None) ||
            static_cast<std::uint16_t>(output.errorCode) >
                static_cast<std::uint16_t>(FrozenAssignmentErrorCode::StaleAuthoritySession) ||
            !IsValidUtf8(output.diagnostic))
            return Fail(error, "ACK result, error code, or diagnostic is invalid.");
        return true;
    }

    inline bool EncodeFrozenAssignmentAbort(
        const FrozenAssignmentAbortDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output.clear(); error.clear();
        if (!ValidateVersion(value.payloadSchemaVersion, error) ||
            !ValidateTransactionId(value.transactionId, error)) return false;
        Writer writer;
        if (!writer.U16(value.payloadSchemaVersion) ||
            !writer.String(value.transactionId))
            return Fail(error, "Abort payload exceeds safety bounds.");
        output = writer.Take();
        return true;
    }

    inline bool DecodeFrozenAssignmentAbort(
        const std::vector<std::uint8_t>& bytes,
        FrozenAssignmentAbortDTO& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear(); Reader reader(bytes);
        if (!reader.U16(output.payloadSchemaVersion))
            return Fail(error, "Abort payload is truncated.");
        if (!ValidateVersion(output.payloadSchemaVersion, error)) return false;
        if (!reader.String(output.transactionId) || !reader.Done())
            return Fail(error, "Abort payload is malformed or truncated.");
        return ValidateTransactionId(output.transactionId, error);
    }

    inline bool PlanFrozenAssignmentChunks(
        const std::vector<std::uint8_t>& canonicalPayload,
        const std::size_t maxDatagramPayloadBudget,
        const MessageEnvelope& chunkEnvelope,
        const std::string& transactionId,
        FrozenAssignmentChunkPlan& output,
        std::string& error)
    {
        using namespace frozen_assignment_detail;
        output = {}; error.clear();
        if (canonicalPayload.size() > MaxFrozenAssignmentPayloadBytes)
            return Fail(error, "Canonical payload exceeds its safety ceiling.");
        if (maxDatagramPayloadBudget == 0 ||
            maxDatagramPayloadBudget > MaxUdpDatagramPayloadBytes)
            return Fail(error, "Datagram payload budget is outside UDP safety bounds.");
        if (!ValidateTransactionId(transactionId, error)) return false;

        FrozenAssignmentChunkDTO emptyChunk;
        emptyChunk.transactionId = transactionId;
        emptyChunk.chunkCount = 1;
        std::vector<std::uint8_t> emptyChunkPayload;
        if (!EncodeFrozenAssignmentChunk(emptyChunk, emptyChunkPayload, error)) return false;
        const std::size_t envelopeBytes = MakeFrame(chunkEnvelope, {}).size();
        if (envelopeBytes > maxDatagramPayloadBudget ||
            emptyChunkPayload.size() > maxDatagramPayloadBudget - envelopeBytes)
            return Fail(error, "DSP envelope and Frozen Chunk header exceed the datagram budget.");
        const std::size_t available =
            maxDatagramPayloadBudget - envelopeBytes - emptyChunkPayload.size();
        if (available == 0)
            return Fail(error, "Datagram budget leaves no room for chunk payload bytes.");

        const std::size_t chunkCount = canonicalPayload.empty()
            ? 1
            : 1 + (canonicalPayload.size() - 1) / available;
        if (chunkCount > MaxFrozenAssignmentChunks ||
            chunkCount > std::numeric_limits<std::uint32_t>::max())
            return Fail(error, "Planned chunk count exceeds the wire safety ceiling.");

        output.maxDatagramPayloadBudget = maxDatagramPayloadBudget;
        output.dspEnvelopeBytes = envelopeBytes;
        output.frozenChunkHeaderBytes = emptyChunkPayload.size();
        output.maxChunkPayloadBytes = available;
        output.chunks.reserve(chunkCount);
        std::size_t offset = 0;
        for (std::size_t index = 0; index < chunkCount; ++index)
        {
            const std::size_t length = std::min(available, canonicalPayload.size() - offset);
            FrozenAssignmentChunkPlanEntry entry;
            entry.chunkIndex = static_cast<std::uint32_t>(index);
            entry.offset = offset;
            entry.length = static_cast<std::uint32_t>(length);
            entry.encodedDatagramBytes = envelopeBytes + emptyChunkPayload.size() + length;
            output.chunks.push_back(entry);
            offset += length;
        }
        return offset == canonicalPayload.size() ||
            Fail(error, "Chunk plan did not cover the canonical payload exactly.");
    }

    inline bool BuildFrozenAssignmentChunk(
        const std::vector<std::uint8_t>& canonicalPayload,
        const FrozenAssignmentChunkPlan& plan,
        const std::size_t planIndex,
        const std::string& transactionId,
        FrozenAssignmentChunkDTO& output,
        std::string& error)
    {
        output = {}; error.clear();
        if (planIndex >= plan.chunks.size() ||
            plan.chunks.size() > std::numeric_limits<std::uint32_t>::max())
            return frozen_assignment_detail::Fail(error, "Chunk plan index is invalid.");
        const FrozenAssignmentChunkPlanEntry& entry = plan.chunks[planIndex];
        if (entry.offset > canonicalPayload.size() ||
            entry.length > canonicalPayload.size() - static_cast<std::size_t>(entry.offset))
            return frozen_assignment_detail::Fail(error, "Chunk plan range exceeds canonical payload bytes.");
        output.transactionId = transactionId;
        output.chunkIndex = entry.chunkIndex;
        output.chunkCount = static_cast<std::uint32_t>(plan.chunks.size());
        output.offset = entry.offset;
        output.chunkPayloadBytes.assign(
            canonicalPayload.begin() + static_cast<std::size_t>(entry.offset),
            canonicalPayload.begin() + static_cast<std::size_t>(entry.offset) + entry.length);
        return true;
    }
}
