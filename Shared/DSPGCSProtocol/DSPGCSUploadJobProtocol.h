#pragma once

#include "DSPGCSProtocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dspgcs
{
    inline constexpr std::uint16_t UploadJobSchemaVersion = 1;
    inline constexpr std::size_t UploadJobCanonicalIdBytes = 32;
    inline constexpr std::size_t UploadJobDigestBytes = 32;
    inline constexpr std::size_t UploadJobMaxSafeDiagnosticBytes = 512;
    inline constexpr std::size_t UploadJobDefaultDatagramBudgetBytes = 1200;
    inline constexpr std::uint32_t UploadJobMaxRouteRecordsPerMessage = 20000;

    namespace UploadJobMessageTypes
    {
        inline constexpr MessageType Command = static_cast<MessageType>(9);
        inline constexpr MessageType CommandAck = static_cast<MessageType>(10);
        inline constexpr MessageType SnapshotRequest = static_cast<MessageType>(11);
        inline constexpr MessageType Snapshot = static_cast<MessageType>(12);
        inline constexpr MessageType RouteStatePage = static_cast<MessageType>(13);
        inline constexpr MessageType RouteStateDelta = static_cast<MessageType>(14);
    }

    using UploadJobDigest = std::array<std::uint8_t, UploadJobDigestBytes>;

    enum class UploadJobCommandType : std::uint8_t
    {
        Start = 1,
        Pause = 2,
        Resume = 3,
        Cancel = 4,
        RetryFailed = 5,
        SetMaxConcurrentUploads = 6
    };

    enum class UploadJobCommandAckStatus : std::uint8_t
    {
        Accepted = 1,
        Rejected = 2,
        DuplicateReplayed = 3
    };

    enum class UploadJobState : std::uint8_t
    {
        Created = 1,
        Validating = 2,
        Ready = 3,
        Running = 4,
        Paused = 5,
        Cancelling = 6,
        Completed = 7,
        Failed = 8,
        Cancelled = 9
    };

    enum class UploadRouteState : std::uint8_t
    {
        Waiting = 1,
        Preparing = 2,
        Uploading = 3,
        RetryWaiting = 4,
        Succeeded = 5,
        Failed = 6,
        Cancelled = 7
    };

    enum class UploadArtifactGateState : std::uint8_t
    {
        NotChecked = 1,
        Validating = 2,
        Validated = 3,
        Rejected = 4
    };

    enum class UploadJobSnapshotRequestKind : std::uint8_t
    {
        Aggregate = 1,
        RoutePage = 2
    };

    enum class UploadJobErrorCode : std::uint16_t
    {
        None = 0,
        UnsupportedProtocolVersion = 1,
        MalformedMessage = 2,
        InvalidCommand = 3,
        UnknownJob = 4,
        JobIdCollision = 5,
        IdempotencyConflict = 6,
        StaleJobRevision = 7,
        InvalidJobState = 8,
        InvalidMaxConcurrency = 9,
        ArtifactNotValidated = 10,
        ArtifactValidationFailed = 11,
        ArtifactIdentityMismatch = 12,
        CurrentReplaced = 13,
        ArtifactLeaseUnavailable = 14,
        FrozenAssignmentMismatch = 15,
        NoFailedRoutes = 16,
        InternalError = 17
    };

    enum class RouteUploadErrorCode : std::uint16_t
    {
        None = 0,
        RouteDescriptorMissing = 1,
        RouteFileUnavailable = 2,
        RouteIdentityMismatch = 3,
        WaypointValidationFailed = 4,
        MissionEncodingFailed = 5,
        TransportUnavailable = 6,
        UploadRejected = 7,
        UploadTimedOut = 8,
        Cancelled = 9,
        InternalError = 10
    };

    struct UploadArtifactIdentityDTO
    {
        std::string artifactSetId;
        UploadJobDigest artifactSetDigest{};
        std::string fleetSessionId;
        std::string assignmentAuthoritySessionId;
        std::uint64_t assignmentRevision = 0;
        UploadJobDigest assignmentDigest{};
    };

    struct UploadJobFailureDTO
    {
        UploadJobErrorCode jobErrorCode = UploadJobErrorCode::None;
        RouteUploadErrorCode routeErrorCode = RouteUploadErrorCode::None;
        std::int32_t routeSlotIndex = -1;
        std::string droneId;
        bool retryable = false;
        std::int64_t occurredAtUtcMs = 0;
        std::string safeSummary;
    };

    struct UploadJobCommandDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string clientCommandId;
        std::string uploadJobId;
        UploadJobCommandType commandType = UploadJobCommandType::Start;
        std::uint64_t expectedJobRevision = 0;
        std::uint32_t requestedMaxConcurrentUploads = 0;
        UploadArtifactIdentityDTO artifactIdentity;
    };

    struct UploadJobCommandAckDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string clientCommandId;
        std::string uploadJobId;
        UploadJobCommandType commandType = UploadJobCommandType::Start;
        UploadJobCommandAckStatus status = UploadJobCommandAckStatus::Rejected;
        UploadJobErrorCode resultCode = UploadJobErrorCode::None;
        std::uint64_t authoritativeJobRevision = 0;
        std::uint32_t effectiveMaxConcurrentUploads = 0;
        bool retryable = false;
        std::string safeDiagnostic;
    };

    struct UploadJobSnapshotRequestDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string clientRequestId;
        std::string uploadJobId;
        UploadJobSnapshotRequestKind requestKind = UploadJobSnapshotRequestKind::Aggregate;
        std::string snapshotToken;
        std::uint32_t routePageStart = 0;
        std::uint32_t maximumRouteEntries = 0;
    };

    struct UploadJobSnapshotDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string clientRequestId;
        std::string uploadJobId;
        std::string snapshotToken;
        UploadJobErrorCode resultCode = UploadJobErrorCode::None;
        std::string safeDiagnostic;
        UploadArtifactIdentityDTO artifactIdentity;
        UploadArtifactGateState artifactGateState = UploadArtifactGateState::NotChecked;
        UploadJobState jobState = UploadJobState::Created;
        std::uint64_t jobRevision = 0;
        std::uint64_t snapshotSequence = 0;
        std::uint64_t routeDeltaSequence = 0;
        std::int64_t updatedAtUtcMs = 0;
        std::uint32_t matchedCount = 0;
        std::uint32_t vacantCount = 0;
        std::uint32_t spareCount = 0;
        std::uint32_t excludedCount = 0;
        std::uint32_t waitingCount = 0;
        std::uint32_t preparingCount = 0;
        std::uint32_t uploadingCount = 0;
        std::uint32_t retryWaitingCount = 0;
        std::uint32_t succeededCount = 0;
        std::uint32_t failedCount = 0;
        std::uint32_t cancelledCount = 0;
        std::uint64_t totalBytes = 0;
        std::uint64_t bytesCompleted = 0;
        std::uint64_t throughputBytesPerSecond = 0;
        std::uint32_t throughputWindowMs = 0;
        std::uint64_t elapsedMs = 0;
        std::uint32_t effectiveMaxConcurrentUploads = 0;
        UploadJobFailureDTO firstFailure;
    };

    struct UploadRouteStateDTO
    {
        std::int32_t routeSlotIndex = -1;
        std::string droneId;
        UploadRouteState state = UploadRouteState::Waiting;
        std::uint64_t totalBytes = 0;
        std::uint64_t bytesCompleted = 0;
        std::uint32_t attemptCount = 0;
        RouteUploadErrorCode errorCode = RouteUploadErrorCode::None;
        bool retryable = false;
        std::int64_t updatedAtUtcMs = 0;
        std::string safeFailureSummary;
    };

    struct UploadJobRouteStatePageDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string clientRequestId;
        std::string uploadJobId;
        std::string snapshotToken;
        std::uint64_t jobRevision = 0;
        std::uint32_t totalRouteCount = 0;
        std::uint32_t pageStart = 0;
        std::uint32_t nextPageStart = 0;
        bool hasMore = false;
        std::vector<UploadRouteStateDTO> routes;
    };

    struct UploadJobRouteStateDeltaDTO
    {
        std::uint16_t payloadSchemaVersion = UploadJobSchemaVersion;
        std::string uploadJobId;
        std::uint64_t jobRevision = 0;
        std::uint64_t deltaSequence = 0;
        std::vector<UploadRouteStateDTO> routes;
    };

    inline bool IsUploadJobMessageType(const MessageType type)
    {
        return type == UploadJobMessageTypes::Command ||
            type == UploadJobMessageTypes::CommandAck ||
            type == UploadJobMessageTypes::SnapshotRequest ||
            type == UploadJobMessageTypes::Snapshot ||
            type == UploadJobMessageTypes::RouteStatePage ||
            type == UploadJobMessageTypes::RouteStateDelta;
    }

    inline bool IsCanonicalUploadJobId(const std::string& value)
    {
        if (value.size() != UploadJobCanonicalIdBytes) return false;
        for (const char item : value)
        {
            if (!((item >= '0' && item <= '9') || (item >= 'a' && item <= 'f')))
                return false;
        }
        return true;
    }

    inline bool IsCanonicalArtifactSetId(const std::string& value)
    {
        if (value.size() != 64) return false;
        for (const char item : value)
        {
            if (!((item >= '0' && item <= '9') || (item >= 'a' && item <= 'f')))
                return false;
        }
        return true;
    }

    inline std::size_t UploadRouteStateEncodedBytes(
        const UploadRouteStateDTO& value)
    {
        // Fixed integer/bool fields plus two u32-length-prefixed strings.
        return 44 + value.droneId.size() + value.safeFailureSummary.size();
    }

    namespace upload_job_detail
    {
        inline bool Fail(std::string& error, const char* text)
        {
            error = text;
            return false;
        }

        inline bool ValidCommandType(const UploadJobCommandType value)
        {
            return value >= UploadJobCommandType::Start &&
                value <= UploadJobCommandType::SetMaxConcurrentUploads;
        }
        inline bool ValidJobState(const UploadJobState value)
        {
            return value >= UploadJobState::Created && value <= UploadJobState::Cancelled;
        }
        inline bool ValidRouteState(const UploadRouteState value)
        {
            return value >= UploadRouteState::Waiting && value <= UploadRouteState::Cancelled;
        }
        inline bool ValidArtifactGate(const UploadArtifactGateState value)
        {
            return value >= UploadArtifactGateState::NotChecked &&
                value <= UploadArtifactGateState::Rejected;
        }

        inline void Digest(detail::Writer& writer, const UploadJobDigest& digest)
        {
            for (const std::uint8_t byte : digest) writer.U8(byte);
        }
        inline bool Digest(detail::Reader& reader, UploadJobDigest& digest)
        {
            for (std::uint8_t& byte : digest) if (!reader.U8(byte)) return false;
            return true;
        }
        inline void Identity(detail::Writer& writer, const UploadArtifactIdentityDTO& value)
        {
            writer.String(value.artifactSetId);
            Digest(writer, value.artifactSetDigest);
            writer.String(value.fleetSessionId);
            writer.String(value.assignmentAuthoritySessionId);
            writer.U64(value.assignmentRevision);
            Digest(writer, value.assignmentDigest);
        }
        inline bool Identity(detail::Reader& reader, UploadArtifactIdentityDTO& value)
        {
            return reader.String(value.artifactSetId) && Digest(reader, value.artifactSetDigest) &&
                reader.String(value.fleetSessionId) &&
                reader.String(value.assignmentAuthoritySessionId) &&
                reader.U64(value.assignmentRevision) && Digest(reader, value.assignmentDigest);
        }
        inline bool ValidateIdentity(const UploadArtifactIdentityDTO& value, std::string& error)
        {
            if (!IsCanonicalArtifactSetId(value.artifactSetId))
                return Fail(error, "ArtifactSetID is not canonical lowercase hex.");
            if (value.fleetSessionId.empty() || value.fleetSessionId.size() > MaxStringBytes ||
                value.assignmentAuthoritySessionId.empty() ||
                value.assignmentAuthoritySessionId.size() > MaxStringBytes ||
                value.assignmentRevision == 0)
                return Fail(error, "Artifact Assignment identity is incomplete or oversized.");
            return true;
        }
        inline void Failure(detail::Writer& writer, const UploadJobFailureDTO& value)
        {
            writer.U16(static_cast<std::uint16_t>(value.jobErrorCode));
            writer.U16(static_cast<std::uint16_t>(value.routeErrorCode));
            writer.I32(value.routeSlotIndex); writer.String(value.droneId);
            writer.Bool(value.retryable); writer.I64(value.occurredAtUtcMs);
            writer.String(value.safeSummary);
        }
        inline bool Failure(detail::Reader& reader, UploadJobFailureDTO& value)
        {
            std::uint16_t jobCode = 0, routeCode = 0;
            if (!reader.U16(jobCode) || !reader.U16(routeCode) ||
                !reader.I32(value.routeSlotIndex) || !reader.String(value.droneId) ||
                !reader.Bool(value.retryable) || !reader.I64(value.occurredAtUtcMs) ||
                !reader.String(value.safeSummary)) return false;
            value.jobErrorCode = static_cast<UploadJobErrorCode>(jobCode);
            value.routeErrorCode = static_cast<RouteUploadErrorCode>(routeCode);
            return value.safeSummary.size() <= UploadJobMaxSafeDiagnosticBytes;
        }
        inline void Route(detail::Writer& writer, const UploadRouteStateDTO& value)
        {
            writer.I32(value.routeSlotIndex); writer.String(value.droneId);
            writer.U8(static_cast<std::uint8_t>(value.state));
            writer.U64(value.totalBytes); writer.U64(value.bytesCompleted);
            writer.U32(value.attemptCount);
            writer.U16(static_cast<std::uint16_t>(value.errorCode));
            writer.Bool(value.retryable); writer.I64(value.updatedAtUtcMs);
            writer.String(value.safeFailureSummary);
        }
        inline bool Route(detail::Reader& reader, UploadRouteStateDTO& value)
        {
            std::uint8_t state = 0; std::uint16_t errorCode = 0;
            if (!reader.I32(value.routeSlotIndex) || !reader.String(value.droneId) ||
                !reader.U8(state) || !reader.U64(value.totalBytes) ||
                !reader.U64(value.bytesCompleted) || !reader.U32(value.attemptCount) ||
                !reader.U16(errorCode) || !reader.Bool(value.retryable) ||
                !reader.I64(value.updatedAtUtcMs) || !reader.String(value.safeFailureSummary))
                return false;
            value.state = static_cast<UploadRouteState>(state);
            value.errorCode = static_cast<RouteUploadErrorCode>(errorCode);
            return value.routeSlotIndex >= 0 && !value.droneId.empty() &&
                ValidRouteState(value.state) && value.bytesCompleted <= value.totalBytes &&
                value.safeFailureSummary.size() <= UploadJobMaxSafeDiagnosticBytes;
        }
        inline bool Schema(const std::uint16_t value, std::string& error)
        {
            return value == UploadJobSchemaVersion ||
                Fail(error, "Unsupported Upload Job payload schema version.");
        }
    }

    inline bool ValidateUploadJobCommand(const UploadJobCommandDTO& value, std::string& error)
    {
        using namespace upload_job_detail;
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.clientCommandId) ||
            !IsCanonicalUploadJobId(value.uploadJobId) || !ValidCommandType(value.commandType))
            return Fail(error, "Upload Job command identity or type is invalid.");
        if (value.commandType == UploadJobCommandType::Start)
        {
            if (value.expectedJobRevision != 0 || value.requestedMaxConcurrentUploads == 0 ||
                !ValidateIdentity(value.artifactIdentity, error))
                return false;
        }
        else
        {
            if (value.expectedJobRevision == 0)
                return Fail(error, "Non-Start command requires ExpectedJobRevision.");
            if (value.commandType == UploadJobCommandType::SetMaxConcurrentUploads)
            {
                if (value.requestedMaxConcurrentUploads == 0)
                    return Fail(error, "SetMaxConcurrentUploads requires a positive value.");
            }
            else if (value.requestedMaxConcurrentUploads != 0)
                return Fail(error, "Only Start and SetMaxConcurrentUploads carry concurrency.");
        }
        error.clear(); return true;
    }

    inline bool EncodeUploadJobCommand(const UploadJobCommandDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        if (!ValidateUploadJobCommand(value, error)) return false;
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.clientCommandId); writer.String(value.uploadJobId);
        writer.U8(static_cast<std::uint8_t>(value.commandType));
        writer.U64(value.expectedJobRevision); writer.U32(value.requestedMaxConcurrentUploads);
        upload_job_detail::Identity(writer, value.artifactIdentity);
        output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobCommand(const std::vector<std::uint8_t>& input,
        UploadJobCommandDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint8_t type = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.clientCommandId) ||
            !reader.String(value.uploadJobId) || !reader.U8(type) ||
            !reader.U64(value.expectedJobRevision) ||
            !reader.U32(value.requestedMaxConcurrentUploads) ||
            !upload_job_detail::Identity(reader, value.artifactIdentity) || !reader.Done())
            return upload_job_detail::Fail(error, "Malformed Upload Job command payload.");
        value.commandType = static_cast<UploadJobCommandType>(type);
        return ValidateUploadJobCommand(value, error);
    }

    inline bool EncodeUploadJobCommandAck(const UploadJobCommandAckDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        using namespace upload_job_detail;
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.clientCommandId) ||
            !IsCanonicalUploadJobId(value.uploadJobId) || !ValidCommandType(value.commandType) ||
            value.safeDiagnostic.size() > UploadJobMaxSafeDiagnosticBytes)
            return Fail(error, "Upload Job command ACK is invalid.");
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.clientCommandId); writer.String(value.uploadJobId);
        writer.U8(static_cast<std::uint8_t>(value.commandType));
        writer.U8(static_cast<std::uint8_t>(value.status));
        writer.U16(static_cast<std::uint16_t>(value.resultCode));
        writer.U64(value.authoritativeJobRevision);
        writer.U32(value.effectiveMaxConcurrentUploads); writer.Bool(value.retryable);
        writer.String(value.safeDiagnostic); output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobCommandAck(const std::vector<std::uint8_t>& input,
        UploadJobCommandAckDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint8_t type = 0, status = 0;
        std::uint16_t code = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.clientCommandId) ||
            !reader.String(value.uploadJobId) || !reader.U8(type) || !reader.U8(status) ||
            !reader.U16(code) || !reader.U64(value.authoritativeJobRevision) ||
            !reader.U32(value.effectiveMaxConcurrentUploads) || !reader.Bool(value.retryable) ||
            !reader.String(value.safeDiagnostic) || !reader.Done())
            return upload_job_detail::Fail(error, "Malformed Upload Job command ACK payload.");
        value.commandType = static_cast<UploadJobCommandType>(type);
        value.status = static_cast<UploadJobCommandAckStatus>(status);
        value.resultCode = static_cast<UploadJobErrorCode>(code);
        if (!upload_job_detail::Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.clientCommandId) ||
            !IsCanonicalUploadJobId(value.uploadJobId) ||
            !upload_job_detail::ValidCommandType(value.commandType) ||
            status < 1 || status > 3 || value.safeDiagnostic.size() > UploadJobMaxSafeDiagnosticBytes)
            return upload_job_detail::Fail(error, "Upload Job command ACK is invalid.");
        return true;
    }

    inline bool EncodeUploadJobSnapshotRequest(const UploadJobSnapshotRequestDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        using namespace upload_job_detail;
        const bool validToken = value.snapshotToken.empty() || IsCanonicalUploadJobId(value.snapshotToken);
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.clientRequestId) ||
            !IsCanonicalUploadJobId(value.uploadJobId) || !validToken ||
            (value.requestKind != UploadJobSnapshotRequestKind::Aggregate &&
             value.requestKind != UploadJobSnapshotRequestKind::RoutePage) ||
            (value.requestKind == UploadJobSnapshotRequestKind::Aggregate &&
             (!value.snapshotToken.empty() || value.routePageStart != 0 || value.maximumRouteEntries != 0)) ||
            (value.requestKind == UploadJobSnapshotRequestKind::RoutePage &&
             (value.snapshotToken.empty() || value.maximumRouteEntries == 0 ||
              value.maximumRouteEntries > UploadJobMaxRouteRecordsPerMessage)))
            return Fail(error, "Upload Job snapshot request is invalid.");
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.clientRequestId); writer.String(value.uploadJobId);
        writer.U8(static_cast<std::uint8_t>(value.requestKind));
        writer.String(value.snapshotToken); writer.U32(value.routePageStart);
        writer.U32(value.maximumRouteEntries); output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobSnapshotRequest(const std::vector<std::uint8_t>& input,
        UploadJobSnapshotRequestDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint8_t kind = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.clientRequestId) ||
            !reader.String(value.uploadJobId) || !reader.U8(kind) ||
            !reader.String(value.snapshotToken) || !reader.U32(value.routePageStart) ||
            !reader.U32(value.maximumRouteEntries) || !reader.Done())
            return upload_job_detail::Fail(error, "Malformed Upload Job snapshot request.");
        value.requestKind = static_cast<UploadJobSnapshotRequestKind>(kind);
        std::vector<std::uint8_t> ignored;
        return EncodeUploadJobSnapshotRequest(value, ignored, error);
    }

    inline bool EncodeUploadJobSnapshot(const UploadJobSnapshotDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        using namespace upload_job_detail;
        const std::uint64_t stateTotal = static_cast<std::uint64_t>(value.waitingCount) +
            value.preparingCount + value.uploadingCount + value.retryWaitingCount +
            value.succeededCount + value.failedCount + value.cancelledCount;
        const bool errorSnapshot = value.resultCode != UploadJobErrorCode::None;
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.uploadJobId) ||
            (!value.clientRequestId.empty() && !IsCanonicalUploadJobId(value.clientRequestId)) ||
            value.safeDiagnostic.size() > UploadJobMaxSafeDiagnosticBytes ||
            (!errorSnapshot && (!IsCanonicalUploadJobId(value.snapshotToken) ||
             !ValidateIdentity(value.artifactIdentity, error) ||
             !ValidArtifactGate(value.artifactGateState) || !ValidJobState(value.jobState) ||
             value.jobRevision == 0 || value.snapshotSequence == 0 ||
             stateTotal != value.matchedCount || value.bytesCompleted > value.totalBytes ||
             value.effectiveMaxConcurrentUploads == 0 ||
             value.firstFailure.safeSummary.size() > UploadJobMaxSafeDiagnosticBytes)) ||
            (errorSnapshot && (!value.snapshotToken.empty() || value.jobRevision != 0 ||
             value.snapshotSequence != 0 || value.matchedCount != 0)))
            return Fail(error, "Upload Job aggregate snapshot is invalid.");
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.clientRequestId); writer.String(value.uploadJobId);
        writer.String(value.snapshotToken);
        writer.U16(static_cast<std::uint16_t>(value.resultCode));
        writer.String(value.safeDiagnostic);
        Identity(writer, value.artifactIdentity);
        writer.U8(static_cast<std::uint8_t>(value.artifactGateState));
        writer.U8(static_cast<std::uint8_t>(value.jobState));
        writer.U64(value.jobRevision); writer.U64(value.snapshotSequence);
        writer.U64(value.routeDeltaSequence);
        writer.I64(value.updatedAtUtcMs);
        writer.U32(value.matchedCount); writer.U32(value.vacantCount);
        writer.U32(value.spareCount); writer.U32(value.excludedCount);
        writer.U32(value.waitingCount); writer.U32(value.preparingCount);
        writer.U32(value.uploadingCount); writer.U32(value.retryWaitingCount);
        writer.U32(value.succeededCount); writer.U32(value.failedCount);
        writer.U32(value.cancelledCount); writer.U64(value.totalBytes);
        writer.U64(value.bytesCompleted); writer.U64(value.throughputBytesPerSecond);
        writer.U32(value.throughputWindowMs); writer.U64(value.elapsedMs);
        writer.U32(value.effectiveMaxConcurrentUploads); Failure(writer, value.firstFailure);
        output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobSnapshot(const std::vector<std::uint8_t>& input,
        UploadJobSnapshotDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint8_t gate = 0, state = 0;
        std::uint16_t resultCode = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.clientRequestId) ||
            !reader.String(value.uploadJobId) || !reader.String(value.snapshotToken) ||
            !reader.U16(resultCode) || !reader.String(value.safeDiagnostic) ||
            !upload_job_detail::Identity(reader, value.artifactIdentity) || !reader.U8(gate) ||
            !reader.U8(state) || !reader.U64(value.jobRevision) ||
            !reader.U64(value.snapshotSequence) ||
            !reader.U64(value.routeDeltaSequence) || !reader.I64(value.updatedAtUtcMs) ||
            !reader.U32(value.matchedCount) || !reader.U32(value.vacantCount) ||
            !reader.U32(value.spareCount) || !reader.U32(value.excludedCount) ||
            !reader.U32(value.waitingCount) || !reader.U32(value.preparingCount) ||
            !reader.U32(value.uploadingCount) || !reader.U32(value.retryWaitingCount) ||
            !reader.U32(value.succeededCount) || !reader.U32(value.failedCount) ||
            !reader.U32(value.cancelledCount) || !reader.U64(value.totalBytes) ||
            !reader.U64(value.bytesCompleted) || !reader.U64(value.throughputBytesPerSecond) ||
            !reader.U32(value.throughputWindowMs) || !reader.U64(value.elapsedMs) ||
            !reader.U32(value.effectiveMaxConcurrentUploads) ||
            !upload_job_detail::Failure(reader, value.firstFailure) || !reader.Done())
            return upload_job_detail::Fail(error, "Malformed Upload Job aggregate snapshot.");
        value.artifactGateState = static_cast<UploadArtifactGateState>(gate);
        value.jobState = static_cast<UploadJobState>(state);
        value.resultCode = static_cast<UploadJobErrorCode>(resultCode);
        std::vector<std::uint8_t> ignored;
        return EncodeUploadJobSnapshot(value, ignored, error);
    }

    inline bool EncodeUploadJobRouteStatePage(const UploadJobRouteStatePageDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        using namespace upload_job_detail;
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.clientRequestId) ||
            !IsCanonicalUploadJobId(value.uploadJobId) ||
            !IsCanonicalUploadJobId(value.snapshotToken) || value.jobRevision == 0 ||
            value.routes.empty() || value.routes.size() > UploadJobMaxRouteRecordsPerMessage ||
            value.pageStart >= value.totalRouteCount ||
            value.pageStart + value.routes.size() > value.totalRouteCount ||
            value.nextPageStart != value.pageStart + value.routes.size() ||
            value.hasMore != (value.nextPageStart < value.totalRouteCount))
            return Fail(error, "Upload Job route page metadata is invalid.");
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.clientRequestId); writer.String(value.uploadJobId);
        writer.String(value.snapshotToken); writer.U64(value.jobRevision);
        writer.U32(value.totalRouteCount); writer.U32(value.pageStart);
        writer.U32(value.nextPageStart); writer.Bool(value.hasMore);
        writer.U32(static_cast<std::uint32_t>(value.routes.size()));
        for (const auto& route : value.routes) Route(writer, route);
        output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobRouteStatePage(const std::vector<std::uint8_t>& input,
        UploadJobRouteStatePageDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint32_t count = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.clientRequestId) ||
            !reader.String(value.uploadJobId) || !reader.String(value.snapshotToken) ||
            !reader.U64(value.jobRevision) || !reader.U32(value.totalRouteCount) ||
            !reader.U32(value.pageStart) || !reader.U32(value.nextPageStart) ||
            !reader.Bool(value.hasMore) || !reader.U32(count) || count == 0 ||
            count > UploadJobMaxRouteRecordsPerMessage)
            return upload_job_detail::Fail(error, "Malformed Upload Job route page.");
        value.routes.resize(count);
        for (auto& route : value.routes) if (!upload_job_detail::Route(reader, route))
            return upload_job_detail::Fail(error, "Invalid Upload Job route page record.");
        if (!reader.Done()) return upload_job_detail::Fail(error, "Upload Job route page has trailing bytes.");
        std::vector<std::uint8_t> ignored;
        return EncodeUploadJobRouteStatePage(value, ignored, error);
    }

    inline bool EncodeUploadJobRouteStateDelta(const UploadJobRouteStateDeltaDTO& value,
        std::vector<std::uint8_t>& output, std::string& error)
    {
        using namespace upload_job_detail;
        if (!Schema(value.payloadSchemaVersion, error) ||
            !IsCanonicalUploadJobId(value.uploadJobId) || value.jobRevision == 0 ||
            value.deltaSequence == 0 || value.routes.empty() ||
            value.routes.size() > UploadJobMaxRouteRecordsPerMessage)
            return Fail(error, "Upload Job route delta metadata is invalid.");
        detail::Writer writer; writer.U16(value.payloadSchemaVersion);
        writer.String(value.uploadJobId); writer.U64(value.jobRevision);
        writer.U64(value.deltaSequence);
        writer.U32(static_cast<std::uint32_t>(value.routes.size()));
        for (const auto& route : value.routes) Route(writer, route);
        output = writer.Take(); return true;
    }
    inline bool DecodeUploadJobRouteStateDelta(const std::vector<std::uint8_t>& input,
        UploadJobRouteStateDeltaDTO& value, std::string& error)
    {
        detail::Reader reader(input); std::uint32_t count = 0;
        if (!reader.U16(value.payloadSchemaVersion) || !reader.String(value.uploadJobId) ||
            !reader.U64(value.jobRevision) || !reader.U64(value.deltaSequence) ||
            !reader.U32(count) || count == 0 || count > UploadJobMaxRouteRecordsPerMessage)
            return upload_job_detail::Fail(error, "Malformed Upload Job route delta.");
        value.routes.resize(count);
        for (auto& route : value.routes) if (!upload_job_detail::Route(reader, route))
            return upload_job_detail::Fail(error, "Invalid Upload Job route delta record.");
        if (!reader.Done()) return upload_job_detail::Fail(error, "Upload Job route delta has trailing bytes.");
        std::vector<std::uint8_t> ignored;
        return EncodeUploadJobRouteStateDelta(value, ignored, error);
    }

    inline bool UploadArtifactIdentityEqual(const UploadArtifactIdentityDTO& left,
        const UploadArtifactIdentityDTO& right)
    {
        return left.artifactSetId == right.artifactSetId &&
            left.artifactSetDigest == right.artifactSetDigest &&
            left.fleetSessionId == right.fleetSessionId &&
            left.assignmentAuthoritySessionId == right.assignmentAuthoritySessionId &&
            left.assignmentRevision == right.assignmentRevision &&
            left.assignmentDigest == right.assignmentDigest;
    }
}
