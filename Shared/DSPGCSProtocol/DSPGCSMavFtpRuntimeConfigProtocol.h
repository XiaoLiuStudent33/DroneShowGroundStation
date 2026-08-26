#pragma once

#include "DSPGCSProtocol.h"
#include "RouteArtifactContract.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace dspgcs
{
    inline constexpr std::uint16_t MavFtpRuntimeConfigSchemaVersion = 1;
    inline constexpr std::size_t MavFtpRuntimeConfigDigestBytes = 32;
    inline constexpr std::size_t MavFtpRuntimeConfigMaxDiagnosticBytes = 512;
    inline constexpr std::uint32_t MavFtpRuntimeConfigMinimumGlobalCap = 1;
    inline constexpr std::uint32_t MavFtpRuntimeConfigMaximumGlobalCap = 800;
    inline constexpr std::uint32_t MavFtpRuntimeConfigWirePayloadBytes = 239;

    namespace MavFtpRuntimeConfigMessageTypes
    {
        // Upload Job currently owns 9..14. These additions never renumber it.
        inline constexpr MessageType Request = static_cast<MessageType>(15);
        inline constexpr MessageType Ack = static_cast<MessageType>(16);
        inline constexpr MessageType Snapshot = static_cast<MessageType>(17);
    }

    enum class MavFtpRuntimeConfigOperation : std::uint8_t
    {
        Get = 1,
        Apply = 2
    };

    enum class MavFtpRuntimeConfigAckStatus : std::uint8_t
    {
        Accepted = 1,
        Rejected = 2,
        DuplicateReplayed = 3
    };

    enum class MavFtpRuntimeConfigActivationState : std::uint8_t
    {
        BootDefaults = 1,
        RuntimeAccepted = 2
    };

    enum class MavFtpRuntimeConfigErrorCode : std::uint16_t
    {
        None = 0,
        UnsupportedProtocolVersion = 1,
        MalformedMessage = 2,
        InvalidRequest = 3,
        InvalidGlobalWriteOutstandingCap = 4,
        StaleRevision = 5,
        AuthoritySessionMismatch = 6,
        IdempotencyConflict = 7,
        InternalError = 8
    };

    enum MavFtpRuntimeConfigFieldMask : std::uint32_t
    {
        RuntimeConfigGlobalWriteCap = 1U << 0,
        RuntimeConfigWriteWindow = 1U << 1,
        RuntimeConfigWritePayload = 1U << 2,
        RuntimeConfigSegmentItems = 1U << 3,
        RuntimeConfigRequestTimeout = 1U << 4,
        RuntimeConfigTerminateTimeout = 1U << 5,
        RuntimeConfigRetryCount = 1U << 6
    };

    inline constexpr std::uint32_t MavFtpRuntimeConfigEditableMask =
        RuntimeConfigGlobalWriteCap;
    inline constexpr std::uint32_t MavFtpRuntimeConfigReadOnlyMask =
        RuntimeConfigWriteWindow | RuntimeConfigWritePayload |
        RuntimeConfigSegmentItems | RuntimeConfigRequestTimeout |
        RuntimeConfigTerminateTimeout | RuntimeConfigRetryCount;

    using MavFtpRuntimeConfigDigest =
        std::array<std::uint8_t, MavFtpRuntimeConfigDigestBytes>;

    struct MavFtpRuntimeConfigRequestDTO
    {
        std::uint16_t schemaVersion = MavFtpRuntimeConfigSchemaVersion;
        std::string clientRequestId;
        std::string authoritySessionId;
        std::uint64_t expectedConfigRevision = 0;
        MavFtpRuntimeConfigOperation operation = MavFtpRuntimeConfigOperation::Get;
        std::uint32_t candidateGlobalWriteOutstandingCap = 0;
    };

    struct MavFtpRuntimeConfigAckDTO
    {
        std::uint16_t schemaVersion = MavFtpRuntimeConfigSchemaVersion;
        std::string clientRequestId;
        MavFtpRuntimeConfigAckStatus status =
            MavFtpRuntimeConfigAckStatus::Rejected;
        MavFtpRuntimeConfigErrorCode resultCode =
            MavFtpRuntimeConfigErrorCode::None;
        std::string authoritySessionId;
        std::uint64_t authoritativeRevision = 0;
        MavFtpRuntimeConfigDigest configDigest{};
        std::string safeDiagnostic;
    };

    struct MavFtpRuntimeConfigSnapshotDTO
    {
        std::uint16_t schemaVersion = MavFtpRuntimeConfigSchemaVersion;
        std::string clientRequestId;
        std::string authoritySessionId;
        std::uint64_t configRevision = 0;
        MavFtpRuntimeConfigDigest configDigest{};

        std::uint32_t bootDefaultGlobalWriteCap = 0;
        std::uint32_t runtimeAcceptedGlobalWriteCap = 0;
        std::uint32_t writeWindow = 0;
        std::uint32_t writePayload = MavFtpRuntimeConfigWirePayloadBytes;
        std::uint32_t segmentItems = 0;
        std::uint32_t requestTimeoutMs = 0;
        std::uint32_t terminateTimeoutMs = 0;
        std::uint32_t retryCount = 0;
        std::uint32_t editableMask = MavFtpRuntimeConfigEditableMask;
        std::uint32_t readOnlyMask = MavFtpRuntimeConfigReadOnlyMask;
        MavFtpRuntimeConfigActivationState activationState =
            MavFtpRuntimeConfigActivationState::RuntimeAccepted;
    };

    inline bool IsMavFtpRuntimeConfigMessageType(const MessageType type)
    {
        return type == MavFtpRuntimeConfigMessageTypes::Request ||
            type == MavFtpRuntimeConfigMessageTypes::Ack ||
            type == MavFtpRuntimeConfigMessageTypes::Snapshot;
    }

    inline bool IsCanonicalMavFtpRuntimeConfigId(const std::string& value)
    {
        if (value.size() != 32) return false;
        return std::all_of(value.begin(), value.end(), [](const char item)
        {
            return (item >= '0' && item <= '9') ||
                (item >= 'a' && item <= 'f');
        });
    }

    namespace mavftp_runtime_config_detail
    {
        inline bool Fail(std::string& error, const char* text)
        {
            error = text;
            return false;
        }

        inline void Digest(
            detail::Writer& writer,
            const MavFtpRuntimeConfigDigest& digest)
        {
            for (const std::uint8_t byte : digest) writer.U8(byte);
        }

        inline bool Digest(
            detail::Reader& reader,
            MavFtpRuntimeConfigDigest& digest)
        {
            for (std::uint8_t& byte : digest)
                if (!reader.U8(byte)) return false;
            return true;
        }

        inline bool DigestIsSet(const MavFtpRuntimeConfigDigest& digest)
        {
            return std::any_of(digest.begin(), digest.end(),
                [](const std::uint8_t byte) { return byte != 0; });
        }

        inline bool Schema(const std::uint16_t value, std::string& error)
        {
            return value == MavFtpRuntimeConfigSchemaVersion ||
                Fail(error, "Unsupported MAVFTP Runtime Config schema version.");
        }

        inline bool ValidCap(const std::uint32_t value)
        {
            return value >= MavFtpRuntimeConfigMinimumGlobalCap &&
                value <= MavFtpRuntimeConfigMaximumGlobalCap;
        }

        inline bool ValidSnapshotValues(
            const MavFtpRuntimeConfigSnapshotDTO& value,
            std::string& error)
        {
            if (!ValidCap(value.bootDefaultGlobalWriteCap) ||
                !ValidCap(value.runtimeAcceptedGlobalWriteCap))
                return Fail(error, "MAVFTP Global Write cap is outside [1,800].");
            if (value.writeWindow < 1 || value.writeWindow > 16)
                return Fail(error, "MAVFTP Write Window is outside [1,16].");
            if (value.writePayload != MavFtpRuntimeConfigWirePayloadBytes)
                return Fail(error, "MAVFTP Write Payload is not the frozen wire value 239.");
            if (value.segmentItems < 1 || value.segmentItems > 65535)
                return Fail(error, "MAVFTP Segment Items is outside [1,65535].");
            if (value.requestTimeoutMs < 100 || value.requestTimeoutMs > 60000 ||
                value.terminateTimeoutMs < 100 || value.terminateTimeoutMs > 60000)
                return Fail(error, "MAVFTP timeout is outside [100,60000].");
            if (value.retryCount > 10)
                return Fail(error, "MAVFTP Retry Count is outside [0,10].");
            if (value.editableMask != MavFtpRuntimeConfigEditableMask ||
                value.readOnlyMask != MavFtpRuntimeConfigReadOnlyMask)
                return Fail(error, "MAVFTP Runtime Config field masks are invalid.");
            if (value.activationState != MavFtpRuntimeConfigActivationState::BootDefaults &&
                value.activationState != MavFtpRuntimeConfigActivationState::RuntimeAccepted)
                return Fail(error, "MAVFTP Runtime Config activation state is invalid.");
            return true;
        }
    }

    inline bool ComputeMavFtpRuntimeConfigDigest(
        const MavFtpRuntimeConfigSnapshotDTO& value,
        MavFtpRuntimeConfigDigest& output,
        std::string& error)
    {
        detail::Writer writer;
        writer.U16(value.schemaVersion);
        writer.U32(value.bootDefaultGlobalWriteCap);
        writer.U32(value.runtimeAcceptedGlobalWriteCap);
        writer.U32(value.writeWindow);
        writer.U32(value.writePayload);
        writer.U32(value.segmentItems);
        writer.U32(value.requestTimeoutMs);
        writer.U32(value.terminateTimeoutMs);
        writer.U32(value.retryCount);
        writer.U32(value.editableMask);
        writer.U32(value.readOnlyMask);
        writer.U8(static_cast<std::uint8_t>(value.activationState));
        const std::vector<std::uint8_t> bytes = writer.Take();
        dronegcs::route_artifact::Digest256 digest;
        if (!dronegcs::route_artifact::Sha256::Hash(
                bytes.data(), bytes.size(), digest))
            return mavftp_runtime_config_detail::Fail(
                error, "Unable to hash MAVFTP Runtime Config.");
        std::copy(digest.Bytes.begin(), digest.Bytes.end(), output.begin());
        error.clear();
        return true;
    }

    inline bool ValidateMavFtpRuntimeConfigRequest(
        const MavFtpRuntimeConfigRequestDTO& value,
        std::string& error)
    {
        using namespace mavftp_runtime_config_detail;
        if (!Schema(value.schemaVersion, error) ||
            !IsCanonicalMavFtpRuntimeConfigId(value.clientRequestId))
            return Fail(error, "MAVFTP Runtime Config request identity is invalid.");
        if (value.operation == MavFtpRuntimeConfigOperation::Get)
        {
            if ((!value.authoritySessionId.empty() &&
                 !IsCanonicalMavFtpRuntimeConfigId(value.authoritySessionId)) ||
                value.expectedConfigRevision != 0 ||
                value.candidateGlobalWriteOutstandingCap != 0)
                return Fail(error, "MAVFTP Runtime Config Get request is invalid.");
        }
        else if (value.operation == MavFtpRuntimeConfigOperation::Apply)
        {
            if (!IsCanonicalMavFtpRuntimeConfigId(value.authoritySessionId) ||
                value.expectedConfigRevision == 0 ||
                !ValidCap(value.candidateGlobalWriteOutstandingCap))
                return Fail(error, "MAVFTP Runtime Config Apply request is invalid.");
        }
        else return Fail(error, "MAVFTP Runtime Config operation is invalid.");
        error.clear();
        return true;
    }

    inline bool EncodeMavFtpRuntimeConfigRequest(
        const MavFtpRuntimeConfigRequestDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        if (!ValidateMavFtpRuntimeConfigRequest(value, error)) return false;
        detail::Writer writer;
        writer.U16(value.schemaVersion);
        writer.String(value.clientRequestId);
        writer.String(value.authoritySessionId);
        writer.U64(value.expectedConfigRevision);
        writer.U8(static_cast<std::uint8_t>(value.operation));
        writer.U32(value.candidateGlobalWriteOutstandingCap);
        output = writer.Take();
        return true;
    }

    inline bool DecodeMavFtpRuntimeConfigRequest(
        const std::vector<std::uint8_t>& input,
        MavFtpRuntimeConfigRequestDTO& value,
        std::string& error)
    {
        detail::Reader reader(input);
        std::uint8_t operation = 0;
        if (!reader.U16(value.schemaVersion) ||
            !reader.String(value.clientRequestId) ||
            !reader.String(value.authoritySessionId) ||
            !reader.U64(value.expectedConfigRevision) ||
            !reader.U8(operation) ||
            !reader.U32(value.candidateGlobalWriteOutstandingCap) || !reader.Done())
            return mavftp_runtime_config_detail::Fail(
                error, "Malformed MAVFTP Runtime Config request.");
        value.operation = static_cast<MavFtpRuntimeConfigOperation>(operation);
        return ValidateMavFtpRuntimeConfigRequest(value, error);
    }

    inline bool ValidateMavFtpRuntimeConfigAck(
        const MavFtpRuntimeConfigAckDTO& value,
        std::string& error)
    {
        using namespace mavftp_runtime_config_detail;
        if (!Schema(value.schemaVersion, error) ||
            !IsCanonicalMavFtpRuntimeConfigId(value.clientRequestId) ||
            !IsCanonicalMavFtpRuntimeConfigId(value.authoritySessionId) ||
            value.authoritativeRevision == 0 || !DigestIsSet(value.configDigest) ||
            value.safeDiagnostic.size() > MavFtpRuntimeConfigMaxDiagnosticBytes ||
            value.status < MavFtpRuntimeConfigAckStatus::Accepted ||
            value.status > MavFtpRuntimeConfigAckStatus::DuplicateReplayed)
            return Fail(error, "MAVFTP Runtime Config ACK is invalid.");
        error.clear();
        return true;
    }

    inline bool EncodeMavFtpRuntimeConfigAck(
        const MavFtpRuntimeConfigAckDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        if (!ValidateMavFtpRuntimeConfigAck(value, error)) return false;
        detail::Writer writer;
        writer.U16(value.schemaVersion);
        writer.String(value.clientRequestId);
        writer.U8(static_cast<std::uint8_t>(value.status));
        writer.U16(static_cast<std::uint16_t>(value.resultCode));
        writer.String(value.authoritySessionId);
        writer.U64(value.authoritativeRevision);
        mavftp_runtime_config_detail::Digest(writer, value.configDigest);
        writer.String(value.safeDiagnostic);
        output = writer.Take();
        return true;
    }

    inline bool DecodeMavFtpRuntimeConfigAck(
        const std::vector<std::uint8_t>& input,
        MavFtpRuntimeConfigAckDTO& value,
        std::string& error)
    {
        detail::Reader reader(input);
        std::uint8_t status = 0;
        std::uint16_t code = 0;
        if (!reader.U16(value.schemaVersion) ||
            !reader.String(value.clientRequestId) || !reader.U8(status) ||
            !reader.U16(code) || !reader.String(value.authoritySessionId) ||
            !reader.U64(value.authoritativeRevision) ||
            !mavftp_runtime_config_detail::Digest(reader, value.configDigest) ||
            !reader.String(value.safeDiagnostic) || !reader.Done())
            return mavftp_runtime_config_detail::Fail(
                error, "Malformed MAVFTP Runtime Config ACK.");
        value.status = static_cast<MavFtpRuntimeConfigAckStatus>(status);
        value.resultCode = static_cast<MavFtpRuntimeConfigErrorCode>(code);
        return ValidateMavFtpRuntimeConfigAck(value, error);
    }

    inline bool ValidateMavFtpRuntimeConfigSnapshot(
        const MavFtpRuntimeConfigSnapshotDTO& value,
        std::string& error)
    {
        using namespace mavftp_runtime_config_detail;
        if (!Schema(value.schemaVersion, error) ||
            (!value.clientRequestId.empty() &&
             !IsCanonicalMavFtpRuntimeConfigId(value.clientRequestId)) ||
            !IsCanonicalMavFtpRuntimeConfigId(value.authoritySessionId) ||
            value.configRevision == 0 || !DigestIsSet(value.configDigest) ||
            !ValidSnapshotValues(value, error))
            return false;
        MavFtpRuntimeConfigDigest expected{};
        if (!ComputeMavFtpRuntimeConfigDigest(value, expected, error) ||
            expected != value.configDigest)
            return Fail(error, "MAVFTP Runtime Config digest mismatch.");
        error.clear();
        return true;
    }

    inline bool EncodeMavFtpRuntimeConfigSnapshot(
        const MavFtpRuntimeConfigSnapshotDTO& value,
        std::vector<std::uint8_t>& output,
        std::string& error)
    {
        if (!ValidateMavFtpRuntimeConfigSnapshot(value, error)) return false;
        detail::Writer writer;
        writer.U16(value.schemaVersion);
        writer.String(value.clientRequestId);
        writer.String(value.authoritySessionId);
        writer.U64(value.configRevision);
        mavftp_runtime_config_detail::Digest(writer, value.configDigest);
        writer.U32(value.bootDefaultGlobalWriteCap);
        writer.U32(value.runtimeAcceptedGlobalWriteCap);
        writer.U32(value.writeWindow);
        writer.U32(value.writePayload);
        writer.U32(value.segmentItems);
        writer.U32(value.requestTimeoutMs);
        writer.U32(value.terminateTimeoutMs);
        writer.U32(value.retryCount);
        writer.U32(value.editableMask);
        writer.U32(value.readOnlyMask);
        writer.U8(static_cast<std::uint8_t>(value.activationState));
        output = writer.Take();
        return true;
    }

    inline bool DecodeMavFtpRuntimeConfigSnapshot(
        const std::vector<std::uint8_t>& input,
        MavFtpRuntimeConfigSnapshotDTO& value,
        std::string& error)
    {
        detail::Reader reader(input);
        std::uint8_t activationState = 0;
        if (!reader.U16(value.schemaVersion) ||
            !reader.String(value.clientRequestId) ||
            !reader.String(value.authoritySessionId) ||
            !reader.U64(value.configRevision) ||
            !mavftp_runtime_config_detail::Digest(reader, value.configDigest) ||
            !reader.U32(value.bootDefaultGlobalWriteCap) ||
            !reader.U32(value.runtimeAcceptedGlobalWriteCap) ||
            !reader.U32(value.writeWindow) || !reader.U32(value.writePayload) ||
            !reader.U32(value.segmentItems) || !reader.U32(value.requestTimeoutMs) ||
            !reader.U32(value.terminateTimeoutMs) || !reader.U32(value.retryCount) ||
            !reader.U32(value.editableMask) || !reader.U32(value.readOnlyMask) ||
            !reader.U8(activationState) || !reader.Done())
            return mavftp_runtime_config_detail::Fail(
                error, "Malformed MAVFTP Runtime Config snapshot.");
        value.activationState =
            static_cast<MavFtpRuntimeConfigActivationState>(activationState);
        return ValidateMavFtpRuntimeConfigSnapshot(value, error);
    }
}
