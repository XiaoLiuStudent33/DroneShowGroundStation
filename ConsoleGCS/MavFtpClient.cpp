#include "MavFtpClient.h"

#include <algorithm>
#include <limits>
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
        if (config_.writeWindow == 0 || config_.writeWindow > 16)
            config_.writeWindow = 1;
        nextRequestSequence_ = config_.initialRequestSequence;
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
        createRetriesAtStart_ = stats_.createRetries;
        writeRetriesAtStart_ = stats_.writeRetries;
        terminateRetriesAtStart_ = stats_.terminateRetries;
        duplicateAcksAtStart_ = stats_.duplicateWriteAcks;
        staleAcksAtStart_ = stats_.staleOrMismatchedWriteAcks;
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
        request.sequence = ReserveRequestSequence();
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
            if (pendingRequest_.opcode == Opcode::CreateFile)
                ++stats_.createRetries;
            else if (pendingRequest_.opcode == Opcode::WriteFile)
                ++stats_.writeRetries;
            else if (pendingRequest_.opcode == Opcode::TerminateSession)
                ++stats_.terminateRetries;
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
        if (phase_ == Phase::AwaitWrite)
        {
            for (std::size_t index = 0; index < pendingWrites_.size(); ++index)
            {
                if (now < pendingWrites_[index].deadline) continue;
                if (pendingWrites_[index].retriesUsed >= config_.maximumRetries)
                {
                    pendingRequest_ = pendingWrites_[index].request;
                    FailWriteOperation("MAVFTP WriteFile retry limit exhausted.");
                    return;
                }
                std::string error;
                if (!RetryWriteRequest(index, error))
                {
                    pendingRequest_ = pendingWrites_[index].request;
                    FailWriteOperation(error);
                    return;
                }
            }
            return;
        }
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
        if (operation_ == OperationKind::WriteFile &&
            (phase_ == Phase::AwaitWrite ||
                response.requestOpcode == Opcode::WriteFile))
        {
            return HandleWriteResponse(response);
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
        if (response.opcode == Opcode::Ack) HandleAck(response);
        else HandleNak(response);
        return true;
    }

    std::uint16_t Client::ReserveRequestSequence()
    {
        for (;;)
        {
            const std::uint16_t reserved = nextRequestSequence_;
            nextRequestSequence_ = NextSequence(
                NextSequence(nextRequestSequence_));
            const bool alreadyPending = std::any_of(
                pendingWrites_.begin(), pendingWrites_.end(),
                [reserved](const PendingWrite& pending)
                {
                    return pending.request.sequence == reserved;
                });
            if (!alreadyPending) return reserved;
        }
    }

    bool Client::SendWriteRequest(
        const std::size_t offset,
        const std::size_t size,
        std::string& error)
    {
        if (size == 0 || offset > operationSize_ ||
            size > operationSize_ - offset ||
            pendingWrites_.size() >= config_.writeWindow)
        {
            error = "MAVFTP WriteFile window range is invalid.";
            return false;
        }
        std::vector<std::uint8_t> chunk(size);
        if (writeChunkReader_)
        {
            if (!writeChunkReader_(offset, chunk.data(), chunk.size(),
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
            std::copy_n(operationBytes_.begin() + offset, size, chunk.begin());
        }

        PendingWrite pending;
        pending.request.sequence = ReserveRequestSequence();
        pending.request.session = activeSession_;
        pending.request.opcode = Opcode::WriteFile;
        pending.request.requestOpcode = Opcode::None;
        pending.request.offset = static_cast<std::uint32_t>(offset);
        pending.request.data = std::move(chunk);
        Error protocolError;
        if (!EncodePayload(pending.request, pending.wire, protocolError))
        {
            error = protocolError.detail;
            return false;
        }
        if (!transport_.SendMavFtpPayload(pending.wire))
        {
            error = "Endpoint-bound MAVFTP WriteFile send failed.";
            return false;
        }
        const auto sentAt = std::chrono::steady_clock::now();
        pending.firstSentAt = sentAt;
        pending.lastSentAt = sentAt;
        pending.deadline = sentAt + config_.requestTimeout;
        pendingWrites_.push_back(std::move(pending));
        ++stats_.requestsSent;
        ++writeBlocksSent_;
        if (firstWriteSentAt_ == std::chrono::steady_clock::time_point{})
            firstWriteSentAt_ = sentAt;
        maxObservedOutstanding_ = (std::max)(maxObservedOutstanding_,
            static_cast<std::uint32_t>(pendingWrites_.size()));
        error.clear();
        return true;
    }

    bool Client::FillWriteWindow(std::string& error)
    {
        while (pendingWrites_.size() < config_.writeWindow &&
            nextWriteOffset_ < operationSize_)
        {
            const std::size_t remaining = operationSize_ - nextWriteOffset_;
            const std::size_t size = (std::min)(
                remaining, maximumWriteChunkBytes_);
            if (!SendWriteRequest(nextWriteOffset_, size, error)) return false;
            nextWriteOffset_ += size;
        }
        return true;
    }

    bool Client::RetryWriteRequest(
        const std::size_t index,
        std::string& error)
    {
        if (index >= pendingWrites_.size() ||
            !transport_.SendMavFtpPayload(pendingWrites_[index].wire))
        {
            error = "Endpoint-bound MAVFTP WriteFile retry failed.";
            return false;
        }
        auto& pending = pendingWrites_[index];
        const auto sentAt = std::chrono::steady_clock::now();
        pending.lastSentAt = sentAt;
        pending.deadline = sentAt + config_.requestTimeout;
        ++pending.retriesUsed;
        ++stats_.retriesSent;
        ++stats_.writeRetries;
        error.clear();
        return true;
    }

    bool Client::HandleWriteResponse(const Payload& response)
    {
        if (phase_ != Phase::AwaitWrite)
        {
            ++stats_.duplicateOrStaleResponses;
            ++stats_.staleOrMismatchedWriteAcks;
            return false;
        }
        if (response.requestOpcode != Opcode::WriteFile)
        {
            ++stats_.wrongRequestOpcodeResponses;
            ++stats_.staleOrMismatchedWriteAcks;
            return false;
        }
        const auto sequenceMatch = std::find_if(
            pendingWrites_.begin(), pendingWrites_.end(),
            [&response](const PendingWrite& candidate)
            {
                return response.sequence == NextSequence(
                    candidate.request.sequence);
            });
        const auto pending = std::find_if(
            pendingWrites_.begin(), pendingWrites_.end(),
            [&response](const PendingWrite& candidate)
            {
                return response.sequence == NextSequence(
                        candidate.request.sequence) &&
                    response.session == candidate.request.session &&
                    response.offset == candidate.request.offset;
            });
        if (pending == pendingWrites_.end())
        {
            const bool duplicate = std::any_of(
                recentWriteAcks_.begin(), recentWriteAcks_.end(),
                [&response](const WriteAckIdentity& identity)
                {
                    return response.sequence == identity.responseSequence &&
                        response.session == identity.session &&
                        response.offset == identity.offset;
                });
            ++stats_.duplicateOrStaleResponses;
            if (duplicate)
                ++stats_.duplicateWriteAcks;
            else
            {
                ++stats_.staleOrMismatchedWriteAcks;
                if (sequenceMatch != pendingWrites_.end() &&
                    response.session != sequenceMatch->request.session)
                    ++stats_.wrongSessionResponses;
            }
            if (!duplicate && pendingWrites_.size() == 1 &&
                sequenceMatch != pendingWrites_.end() &&
                response.session == sequenceMatch->request.session &&
                response.offset != sequenceMatch->request.offset)
            {
                pendingRequest_ = sequenceMatch->request;
                FailWriteOperation(
                    "OffsetMismatch: MAVFTP WriteFile ACK offset differs from the pending block.");
                return true;
            }
            return false;
        }
        if (response.opcode != Opcode::Ack && response.opcode != Opcode::Nak)
        {
            ++stats_.corruptResponses;
            ++stats_.staleOrMismatchedWriteAcks;
            return false;
        }
        if (response.opcode == Opcode::Nak)
        {
            const std::uint8_t nak = response.data.empty() ? 0 : response.data[0];
            pendingRequest_ = pending->request;
            FailWriteOperation(
                std::string("WriteFile returned MAVFTP NAK ") +
                    std::to_string(nak) + " (" + NakErrorName(nak) + ").",
                nak);
            return true;
        }
        const std::size_t blockSize = pending->request.data.size();
        if (!response.data.empty() &&
            (response.data.size() != 4 ||
                ReadU32LittleEndian(response.data.data()) != blockSize))
        {
            pendingRequest_ = pending->request;
            FailWriteOperation("MAVFTP WriteFile ACK byte count is invalid.");
            return true;
        }

        const auto acknowledgedAt = std::chrono::steady_clock::now();
        const std::uint64_t rtt = ElapsedMilliseconds(
            pending->lastSentAt, acknowledgedAt);
        ackRttSumMs_ += rtt;
        ++ackRttCount_;
        if (ackRttSamples_.size() < MaximumAckTimingSamples)
        {
            ackRttSamples_.push_back(rtt);
            ackRttSampleCount_ = ackRttSamples_.size();
        }
        else
        {
            ackRttSamples_[ackRttSampleCursor_] = rtt;
            ackRttSampleCursor_ = (ackRttSampleCursor_ + 1) %
                MaximumAckTimingSamples;
        }
        if (previousWriteAckAt_ != std::chrono::steady_clock::time_point{})
        {
            interAckSumMs_ += ElapsedMilliseconds(
                previousWriteAckAt_, acknowledgedAt);
            ++interAckCount_;
        }
        previousWriteAckAt_ = acknowledgedAt;
        lastWriteAckedAt_ = acknowledgedAt;
        transferred_ += blockSize;
        ++writeBlocksCompleted_;
        ++stats_.responsesAccepted;
        recentWriteAcks_.push_back({ response.sequence, response.session,
            response.offset });
        if (recentWriteAcks_.size() > MaximumRecentWriteAcks)
            recentWriteAcks_.pop_front();
        pendingWrites_.erase(pending);

        std::string error;
        if (!FillWriteWindow(error))
        {
            if (!pendingWrites_.empty()) pendingRequest_ = pendingWrites_.front().request;
            FailWriteOperation(error);
            return true;
        }
        if (nextWriteOffset_ == operationSize_ && pendingWrites_.empty())
        {
            terminateStartedAt_ = acknowledgedAt;
            if (!SendNewRequest(Phase::AwaitTerminateWrite,
                Opcode::TerminateSession, activeSession_, 0, {}, error))
            {
                FailWriteOperation(error);
            }
        }
        return true;
    }

    void Client::FailWriteOperation(
        std::string diagnostic,
        const std::uint8_t nakError)
    {
        pendingWrites_.clear();
        BestEffortTerminate();
        CompleteFailure(std::move(diagnostic), nakError);
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
            nextWriteOffset_ = 0;
            createConfirmedAt_ = std::chrono::steady_clock::now();
            phase_ = Phase::AwaitWrite;
            if (!FillWriteWindow(error))
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
            CompleteFailure("Unexpected control ACK while Write window is active.");
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
        result.writeBlocksSent = writeBlocksSent_;
        result.writeBlocksAcked = writeBlocksCompleted_;
        result.retryCount = stats_.retriesSent - operationRetriesAtStart_;
        result.createRetries = stats_.createRetries - createRetriesAtStart_;
        result.writeRetries = stats_.writeRetries - writeRetriesAtStart_;
        result.terminateRetries = stats_.terminateRetries - terminateRetriesAtStart_;
        result.duplicateAcks = stats_.duplicateWriteAcks - duplicateAcksAtStart_;
        result.staleOrMismatchedAcks =
            stats_.staleOrMismatchedWriteAcks - staleAcksAtStart_;
        result.configuredWriteWindow = config_.writeWindow;
        result.maxObservedOutstanding = maxObservedOutstanding_;
        result.writeDataDurationMs = ElapsedMilliseconds(
            firstWriteSentAt_, lastWriteAckedAt_);
        result.createDurationMs = ElapsedMilliseconds(
            operationStartedAt_, createConfirmedAt_);
        result.writeDurationMs = ElapsedMilliseconds(
            createConfirmedAt_, terminateStartedAt_);
        result.terminateDurationMs = ElapsedMilliseconds(
            terminateStartedAt_, completedAt);
        result.totalDurationMs = ElapsedMilliseconds(
            operationStartedAt_, completedAt);
        result.ackRttAverageMs = ackRttCount_ == 0
            ? 0 : ackRttSumMs_ / ackRttCount_;
        result.interAckAverageMs = interAckCount_ == 0
            ? 0 : interAckSumMs_ / interAckCount_;
        if (ackRttSampleCount_ > 0)
        {
            std::vector<std::uint64_t> samples = ackRttSamples_;
            result.boundedAckRttSamples = samples;
            std::sort(samples.begin(), samples.end());
            const auto percentile = [&samples](const std::size_t percent)
            {
                const std::size_t index = (percent * (samples.size() - 1) + 99) / 100;
                return samples[(std::min)(index, samples.size() - 1)];
            };
            result.ackRttMinMs = samples.front();
            result.ackRttP50Ms = percentile(50);
            result.ackRttP95Ms = percentile(95);
            result.ackRttP99Ms = percentile(99);
            result.maxAckRttMs = samples.back();
        }
    }

    void Client::ResetWriteSource()
    {
        writeChunkReader_ = nullptr;
        writeChunkReaderContext_ = nullptr;
        operationSize_ = 0;
        maximumWriteChunkBytes_ = MaximumDataBytes;
        nextWriteOffset_ = 0;
        pendingWrites_.clear();
        recentWriteAcks_.clear();
        std::vector<std::uint64_t>().swap(ackRttSamples_);
        ackRttSampleCount_ = 0;
        ackRttSampleCursor_ = 0;
        ackRttSumMs_ = 0;
        ackRttCount_ = 0;
        interAckSumMs_ = 0;
        interAckCount_ = 0;
        writeBlocksSent_ = 0;
        writeBlocksCompleted_ = 0;
        maxObservedOutstanding_ = 0;
        createConfirmedAt_ = {};
        firstWriteSentAt_ = {};
        lastWriteAckedAt_ = {};
        previousWriteAckAt_ = {};
        terminateStartedAt_ = {};
    }

    std::optional<OperationResult> Client::ConsumeResult()
    {
        std::optional<OperationResult> result = std::move(completed_);
        completed_.reset();
        return result;
    }
}
