#include "MavFtpClient.h"

#include <algorithm>
#include <utility>

namespace dronegcs::mavftp
{
    namespace
    {
        constexpr std::uint8_t NakUnknownCommand = 7;
        constexpr std::uint8_t NakFileExists = 8;

        bool ValidPeer(const PeerIdentity& peer)
        {
            return !peer.droneId.empty() && !peer.remoteIp.empty() &&
                peer.remotePort != 0 && peer.systemId != 0 &&
                peer.componentId != 0;
        }

        std::uint64_t ElapsedMilliseconds(
            const std::chrono::steady_clock::time_point begin,
            const std::chrono::steady_clock::time_point end)
        {
            if (begin == std::chrono::steady_clock::time_point{} || end < begin)
                return 0;
            return static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    end - begin).count());
        }
    }

    bool SamePeer(const PeerIdentity& left, const PeerIdentity& right)
    {
        return left.droneId == right.droneId &&
            left.remoteIp == right.remoteIp &&
            left.remotePort == right.remotePort &&
            left.systemId == right.systemId &&
            left.componentId == right.componentId;
    }

    Client::Client(
        IRequestTransport& transport,
        PeerIdentity peer,
        ClientConfig config)
        : transport_(transport), peer_(std::move(peer)), config_(config)
    {
        if (config_.requestTimeout.count() <= 0)
            config_.requestTimeout = std::chrono::milliseconds(750);
        if (config_.maximumFileBytes == 0)
            config_.maximumFileBytes = 64U * 1024U * 1024U;
    }

    Client::~Client() noexcept
    {
        Shutdown();
    }

    bool Client::RebindPeer(const PeerIdentity& peer)
    {
        if (IsBusy() || !ValidPeer(peer)) return false;
        peer_ = peer;
        return true;
    }

    bool Client::Begin(
        const OperationKind kind,
        const Phase phase,
        const Opcode opcode,
        const std::uint8_t session,
        const std::uint32_t offset,
        std::vector<std::uint8_t> data,
        std::string& error)
    {
        if (IsBusy())
        {
            error = "MAVFTP client already has an active operation.";
            return false;
        }
        if (!ValidPeer(peer_))
        {
            error = "MAVFTP peer identity is incomplete.";
            return false;
        }
        completed_.reset();
        operation_ = kind;
        operationBytes_.clear();
        ResetWriteSource();
        transferred_ = 0;
        pendingChunkSize_ = 0;
        remoteFileSize_ = 0;
        sessionOpen_ = false;
        activeSession_ = 0;
        operationRetriesAtStart_ = stats_.retriesSent;
        operationStartedAt_ = std::chrono::steady_clock::now();
        if (!SendNewRequest(phase, opcode, session, offset,
            std::move(data), error))
        {
            operation_ = OperationKind::None;
            phase_ = Phase::Idle;
            pending_ = false;
            return false;
        }
        return true;
    }

    bool Client::StartResetSessions(std::string& error)
    {
        path_.clear();
        return Begin(OperationKind::ResetSessions, Phase::AwaitReset,
            Opcode::ResetSessions, 0, 0, {}, error);
    }

    bool Client::StartReadFile(const std::string& path, std::string& error)
    {
        std::vector<std::uint8_t> pathData;
        Error protocolError;
        if (!MakePathData(path, pathData, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        path_ = path;
        return Begin(OperationKind::ReadFile, Phase::AwaitOpenRead,
            Opcode::OpenFileRO, 0, 0, std::move(pathData), error);
    }

    bool Client::StartCalculateCrc32(
        const std::string& path,
        std::string& error)
    {
        std::vector<std::uint8_t> pathData;
        Error protocolError;
        if (!MakePathData(path, pathData, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        path_ = path;
        return Begin(OperationKind::CalculateCrc32, Phase::AwaitCrc,
            Opcode::CalcFileCRC32, 0, 0, std::move(pathData), error);
    }

    bool Client::StartWriteFile(
        const std::string& path,
        std::vector<std::uint8_t> bytes,
        std::string& error)
    {
        if (bytes.empty() || bytes.size() > config_.maximumFileBytes)
        {
            error = "MAVFTP write bytes must be non-empty and within the configured file bound.";
            return false;
        }
        std::vector<std::uint8_t> pathData;
        Error protocolError;
        if (!MakePathData(path, pathData, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        path_ = path;
        if (!Begin(OperationKind::WriteFile, Phase::AwaitCreate,
            Opcode::CreateFile, 0, 0, std::move(pathData), error))
        {
            return false;
        }
        operationBytes_ = std::move(bytes);
        operationSize_ = operationBytes_.size();
        return true;
    }

    bool Client::StartWriteFileStream(
        const std::string& path,
        const std::uint32_t totalBytes,
        const std::size_t maximumChunkBytes,
        const WriteChunkReader reader,
        void* const readerContext,
        std::string& error)
    {
        if (totalBytes == 0 || totalBytes > config_.maximumFileBytes)
        {
            error = "MAVFTP stream size must be non-zero and within the configured file bound.";
            return false;
        }
        if (!reader || maximumChunkBytes == 0 ||
            maximumChunkBytes > MaximumDataBytes)
        {
            error = "MAVFTP stream reader or write block bound is invalid.";
            return false;
        }
        std::vector<std::uint8_t> pathData;
        Error protocolError;
        if (!MakePathData(path, pathData, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        path_ = path;
        if (!Begin(OperationKind::WriteFile, Phase::AwaitCreate,
            Opcode::CreateFile, 0, 0, std::move(pathData), error))
        {
            return false;
        }
        operationSize_ = totalBytes;
        maximumWriteChunkBytes_ = maximumChunkBytes;
        writeChunkReader_ = reader;
        writeChunkReaderContext_ = readerContext;
        return true;
    }

    bool Client::SendNewRequest(
        const Phase phase,
        const Opcode opcode,
        const std::uint8_t session,
        const std::uint32_t offset,
        std::vector<std::uint8_t> data,
        std::string& error)
    {
        Payload request;
        request.sequence = nextRequestSequence_;
        request.session = session;
        request.opcode = opcode;
        request.requestOpcode = Opcode::None;
        request.offset = offset;
        request.data = std::move(data);
        Error protocolError;
        if (!EncodePayload(request, pendingWire_, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        pendingRequest_ = std::move(request);
        phase_ = phase;
        pending_ = true;
        retriesUsed_ = 0;
        return SendPending(false, error);
    }

    bool Client::SendPending(const bool retry, std::string& error)
    {
        if (!pending_ || !transport_.SendMavFtpPayload(pendingWire_))
        {
            error = "Endpoint-bound MAVFTP send failed.";
            return false;
        }
        if (retry)
        {
            ++stats_.retriesSent;
            ++retriesUsed_;
        }
        else
        {
            ++stats_.requestsSent;
        }
        deadline_ = std::chrono::steady_clock::now() + config_.requestTimeout;
        error.clear();
        return true;
    }

    void Client::Tick(const std::chrono::steady_clock::time_point now)
    {
        if (!pending_ || now < deadline_) return;
        if (retriesUsed_ >= config_.maximumRetries)
        {
            BestEffortTerminate();
            CompleteFailure("MAVFTP request retry limit exhausted.");
            return;
        }
        std::string error;
        if (!SendPending(true, error))
        {
            BestEffortTerminate();
            CompleteFailure(error);
        }
    }

    bool Client::HandleResponse(
        const PeerIdentity& source,
        const std::uint8_t* payload,
        const std::size_t payloadSize)
    {
        if (!SamePeer(source, peer_))
        {
            ++stats_.wrongPeerResponses;
            return false;
        }
        Payload response;
        Error protocolError;
        if (!DecodePayload(payload, payloadSize, response, protocolError))
        {
            ++stats_.corruptResponses;
            return false;
        }
        if (!pending_ || response.sequence !=
            NextSequence(pendingRequest_.sequence))
        {
            ++stats_.duplicateOrStaleResponses;
            return false;
        }
        if (response.requestOpcode != pendingRequest_.opcode)
        {
            ++stats_.wrongRequestOpcodeResponses;
            return false;
        }
        const bool opensSession = phase_ == Phase::AwaitOpenRead ||
            phase_ == Phase::AwaitCreate || phase_ == Phase::AwaitOpenWrite;
        if (!opensSession && response.session != pendingRequest_.session)
        {
            ++stats_.wrongSessionResponses;
            return false;
        }
        if (response.opcode != Opcode::Ack && response.opcode != Opcode::Nak)
        {
            ++stats_.corruptResponses;
            return false;
        }

        ++stats_.responsesAccepted;
        pending_ = false;
        nextRequestSequence_ = NextSequence(response.sequence);
        if (response.opcode == Opcode::Ack) HandleAck(response);
        else HandleNak(response);
        return true;
    }

    void Client::HandleAck(const Payload& response)
    {
        std::string error;
        switch (phase_)
        {
        case Phase::AwaitReset:
            CompleteSuccess();
            return;
        case Phase::AwaitOpenRead:
            if (response.data.size() != 4)
            {
                CompleteFailure("OpenFileRO ACK did not contain a uint32 file size.");
                return;
            }
            activeSession_ = response.session;
            sessionOpen_ = true;
            remoteFileSize_ = ReadU32LittleEndian(response.data.data());
            if (remoteFileSize_ > config_.maximumFileBytes)
            {
                BestEffortTerminate();
                CompleteFailure("OpenFileRO reported a file larger than the configured bound.");
                return;
            }
            operationBytes_.clear();
            operationBytes_.reserve(remoteFileSize_);
            transferred_ = 0;
            if (remoteFileSize_ == 0)
            {
                if (!SendNewRequest(Phase::AwaitTerminateRead,
                    Opcode::TerminateSession, activeSession_, 0, {}, error))
                    CompleteFailure(error);
            }
            else if (!StartNextRead(error)) CompleteFailure(error);
            return;
        case Phase::AwaitRead:
            if (response.offset != transferred_ || response.data.empty() ||
                response.data.size() > remoteFileSize_ - transferred_)
            {
                BestEffortTerminate();
                CompleteFailure("ReadFile ACK range is inconsistent with the opened file.");
                return;
            }
            operationBytes_.insert(operationBytes_.end(),
                response.data.begin(), response.data.end());
            transferred_ += response.data.size();
            if (transferred_ == remoteFileSize_)
            {
                if (!SendNewRequest(Phase::AwaitTerminateRead,
                    Opcode::TerminateSession, activeSession_, 0, {}, error))
                    CompleteFailure(error);
            }
            else if (!StartNextRead(error)) CompleteFailure(error);
            return;
        case Phase::AwaitTerminateRead:
            sessionOpen_ = false;
            activeSession_ = 0;
            CompleteSuccess();
            return;
        case Phase::AwaitCrc:
            if (response.data.size() != 4)
            {
                CompleteFailure("CalcFileCRC32 ACK did not contain uint32 CRC.");
                return;
            }
            remoteFileSize_ = ReadU32LittleEndian(response.data.data());
            CompleteSuccess();
            return;
        case Phase::AwaitCreate:
        case Phase::AwaitOpenWrite:
            activeSession_ = response.session;
            sessionOpen_ = true;
            transferred_ = 0;
            createConfirmedAt_ = std::chrono::steady_clock::now();
            if (!StartNextWrite(error))
            {
                BestEffortTerminate();
                CompleteFailure(error);
            }
            return;
        case Phase::AwaitTruncate:
        {
            std::vector<std::uint8_t> pathData;
            Error protocolError;
            if (!MakePathData(path_, pathData, protocolError) ||
                !SendNewRequest(Phase::AwaitOpenWrite, Opcode::OpenFileWO,
                    0, 0, std::move(pathData), error))
                CompleteFailure(protocolError.detail.empty() ? error : protocolError.detail);
            return;
        }
        case Phase::AwaitWrite:
            if (response.offset != transferred_)
            {
                BestEffortTerminate();
                CompleteFailure("OffsetMismatch: MAVFTP WriteFile ACK offset differs from the pending block.");
                return;
            }
            // ArduPilot's canonical WriteFile ACK has no data payload. Retain
            // compatibility with servers that return a uint32 written count.
            if (!response.data.empty() &&
                (response.data.size() != 4 ||
                    ReadU32LittleEndian(response.data.data()) != pendingChunkSize_))
            {
                BestEffortTerminate();
                CompleteFailure("MAVFTP WriteFile ACK byte count is invalid.");
                return;
            }
            transferred_ += pendingChunkSize_;
            ++writeBlocksCompleted_;
            if (transferred_ == operationSize_)
            {
                terminateStartedAt_ = std::chrono::steady_clock::now();
                if (!SendNewRequest(Phase::AwaitTerminateWrite,
                    Opcode::TerminateSession, activeSession_, 0, {}, error))
                {
                    BestEffortTerminate();
                    CompleteFailure(error);
                }
            }
            else if (!StartNextWrite(error))
            {
                BestEffortTerminate();
                CompleteFailure(error);
            }
            return;
        case Phase::AwaitTerminateWrite:
            sessionOpen_ = false;
            activeSession_ = 0;
            CompleteSuccess();
            return;
        case Phase::Idle:
            return;
        }
    }

    void Client::HandleNak(const Payload& response)
    {
        const std::uint8_t nak = response.data.empty() ? 0 : response.data[0];
        if (phase_ == Phase::AwaitCreate && nak == NakFileExists)
        {
            std::vector<std::uint8_t> pathData;
            Error protocolError;
            std::string error;
            if (!MakePathData(path_, pathData, protocolError) ||
                !SendNewRequest(Phase::AwaitTruncate, Opcode::TruncateFile,
                    0, 0, std::move(pathData), error))
                CompleteFailure(protocolError.detail.empty() ? error : protocolError.detail);
            return;
        }
        BestEffortTerminate();
        CompleteFailure(
            std::string(OpcodeName(response.requestOpcode)) +
                " returned MAVFTP NAK " + std::to_string(nak) + " (" +
                NakErrorName(nak) + ").",
            nak,
            phase_ == Phase::AwaitCrc && nak == NakUnknownCommand);
    }

    bool Client::StartNextRead(std::string& error)
    {
        const std::size_t remaining = remoteFileSize_ - transferred_;
        const std::size_t requestSize = (std::min)(remaining, MaximumDataBytes);
        std::vector<std::uint8_t> requestData;
        // ReadFile length is carried by header.size; use zero-filled bytes only
        // to make the pure payload encoder emit the requested size.
        requestData.resize(requestSize, 0);
        return SendNewRequest(Phase::AwaitRead, Opcode::ReadFile,
            activeSession_, static_cast<std::uint32_t>(transferred_),
            std::move(requestData), error);
    }

    bool Client::StartNextWrite(std::string& error)
    {
        if (transferred_ >= operationSize_)
        {
            error = "MAVFTP write source has no remaining bytes.";
            return false;
        }
        const std::size_t remaining = operationSize_ - transferred_;
        pendingChunkSize_ = (std::min)(remaining, maximumWriteChunkBytes_);
        std::vector<std::uint8_t> chunk(pendingChunkSize_);
        if (writeChunkReader_)
        {
            if (!writeChunkReader_(transferred_, chunk.data(), chunk.size(),
                writeChunkReaderContext_, error))
            {
                if (error.empty())
                    error = "MAVFTP write stream rejected the requested range.";
                return false;
            }
        }
        else
        {
            if (operationBytes_.size() != operationSize_)
            {
                error = "MAVFTP write source size changed during the operation.";
                return false;
            }
            std::copy_n(operationBytes_.begin() + transferred_,
                pendingChunkSize_, chunk.begin());
        }
        return SendNewRequest(Phase::AwaitWrite, Opcode::WriteFile,
            activeSession_, static_cast<std::uint32_t>(transferred_),
            std::move(chunk), error);
    }

    void Client::CompleteSuccess()
    {
        OperationResult result;
        result.kind = operation_;
        result.success = true;
        result.remoteFileSize = operation_ == OperationKind::ReadFile
            ? remoteFileSize_ : static_cast<std::uint32_t>(operationSize_);
        if (operation_ == OperationKind::ReadFile)
            result.bytes = operationBytes_;
        if (operation_ == OperationKind::CalculateCrc32)
            result.remoteCrc32 = remoteFileSize_;
        PopulateOperationMetrics(result);
        completed_ = std::move(result);
        pending_ = false;
        phase_ = Phase::Idle;
        operation_ = OperationKind::None;
        ResetWriteSource();
    }

    void Client::CompleteFailure(
        std::string diagnostic,
        const std::uint8_t nakError,
        const bool unsupported)
    {
        OperationResult result;
        result.kind = operation_;
        result.nakError = nakError;
        result.unsupported = unsupported;
        result.safeDiagnostic = std::move(diagnostic);
        result.terminalRequestOpcode = pendingRequest_.opcode;
        PopulateOperationMetrics(result);
        completed_ = std::move(result);
        pending_ = false;
        phase_ = Phase::Idle;
        operation_ = OperationKind::None;
        sessionOpen_ = false;
        activeSession_ = 0;
        ResetWriteSource();
    }

    void Client::CompleteCancelled()
    {
        OperationResult result;
        result.kind = operation_;
        result.cancelled = true;
        result.safeDiagnostic = "MAVFTP operation cancelled.";
        result.terminalRequestOpcode = pendingRequest_.opcode;
        PopulateOperationMetrics(result);
        completed_ = std::move(result);
        pending_ = false;
        phase_ = Phase::Idle;
        operation_ = OperationKind::None;
        sessionOpen_ = false;
        activeSession_ = 0;
        ResetWriteSource();
    }

    void Client::BestEffortTerminate() noexcept
    {
        if (!sessionOpen_) return;
        Payload terminate;
        terminate.sequence = nextRequestSequence_;
        terminate.session = activeSession_;
        terminate.opcode = Opcode::TerminateSession;
        std::array<std::uint8_t, PayloadBytes> wire{};
        Error error;
        if (EncodePayload(terminate, wire, error))
            (void)transport_.SendMavFtpPayload(wire);
        sessionOpen_ = false;
        activeSession_ = 0;
    }

    void Client::Cancel()
    {
        if (!IsBusy()) return;
        BestEffortTerminate();
        CompleteCancelled();
    }

    void Client::Shutdown() noexcept
    {
        if (IsBusy())
        {
            BestEffortTerminate();
            pending_ = false;
            phase_ = Phase::Idle;
            operation_ = OperationKind::None;
        }
    }

    bool Client::IsBusy() const
    {
        return phase_ != Phase::Idle;
    }

    bool Client::HasOpenSession() const
    {
        return sessionOpen_;
    }

    bool Client::IsAwaitingWriteCommit() const
    {
        return phase_ == Phase::AwaitTerminateWrite;
    }

    void Client::PopulateOperationMetrics(OperationResult& result) const
    {
        const auto completedAt = std::chrono::steady_clock::now();
        result.writeBytes = transferred_;
        result.writeBlocks = writeBlocksCompleted_;
        result.retryCount = stats_.retriesSent - operationRetriesAtStart_;
        result.createDurationMs = ElapsedMilliseconds(
            operationStartedAt_, createConfirmedAt_);
        result.writeDurationMs = ElapsedMilliseconds(
            createConfirmedAt_, terminateStartedAt_);
        result.terminateDurationMs = ElapsedMilliseconds(
            terminateStartedAt_, completedAt);
        result.totalDurationMs = ElapsedMilliseconds(
            operationStartedAt_, completedAt);
    }

    void Client::ResetWriteSource()
    {
        writeChunkReader_ = nullptr;
        writeChunkReaderContext_ = nullptr;
        operationSize_ = 0;
        maximumWriteChunkBytes_ = MaximumDataBytes;
        writeBlocksCompleted_ = 0;
        createConfirmedAt_ = {};
        terminateStartedAt_ = {};
    }

    std::optional<OperationResult> Client::ConsumeResult()
    {
        std::optional<OperationResult> result = std::move(completed_);
        completed_.reset();
        return result;
    }
}
