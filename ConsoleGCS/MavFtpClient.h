#pragma once

#include "MavFtpProtocol.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dronegcs::mavftp
{
    struct PeerIdentity
    {
        std::string droneId;
        std::string remoteIp;
        std::uint16_t remotePort = 0;
        std::uint8_t systemId = 0;
        std::uint8_t componentId = 0;
    };

    bool SamePeer(const PeerIdentity& left, const PeerIdentity& right);

    class IRequestTransport
    {
    public:
        virtual ~IRequestTransport() = default;
        virtual bool SendMavFtpPayload(
            const std::array<std::uint8_t, PayloadBytes>& payload) = 0;
    };

    struct ClientConfig
    {
        std::chrono::milliseconds requestTimeout{ 750 };
        std::uint32_t maximumRetries = 3;
        std::uint32_t maximumFileBytes = 64U * 1024U * 1024U;
    };

    enum class OperationKind : std::uint8_t
    {
        None,
        ResetSessions,
        ReadFile,
        CalculateCrc32,
        WriteFile
    };

    struct OperationResult
    {
        OperationKind kind = OperationKind::None;
        bool success = false;
        bool cancelled = false;
        bool unsupported = false;
        std::uint8_t nakError = 0;
        std::uint32_t remoteFileSize = 0;
        std::optional<std::uint32_t> remoteCrc32;
        std::vector<std::uint8_t> bytes;
        std::string safeDiagnostic;
        Opcode terminalRequestOpcode = Opcode::None;
        std::uint64_t writeBytes = 0;
        std::uint64_t writeBlocks = 0;
        std::uint64_t retryCount = 0;
        std::uint64_t createDurationMs = 0;
        std::uint64_t writeDurationMs = 0;
        std::uint64_t terminateDurationMs = 0;
        std::uint64_t totalDurationMs = 0;
    };

    using WriteChunkReader = bool (*)(
        std::size_t offset,
        std::uint8_t* destination,
        std::size_t size,
        void* context,
        std::string& error);

    struct ClientStats
    {
        std::uint32_t requestsSent = 0;
        std::uint32_t retriesSent = 0;
        std::uint32_t responsesAccepted = 0;
        std::uint32_t duplicateOrStaleResponses = 0;
        std::uint32_t wrongPeerResponses = 0;
        std::uint32_t wrongSessionResponses = 0;
        std::uint32_t wrongRequestOpcodeResponses = 0;
        std::uint32_t corruptResponses = 0;
    };

    class Client final
    {
    public:
        Client(IRequestTransport& transport, PeerIdentity peer,
            ClientConfig config = {});
        ~Client() noexcept;

        Client(const Client&) = delete;
        Client& operator=(const Client&) = delete;

        bool RebindPeer(const PeerIdentity& peer);
        bool StartResetSessions(std::string& error);
        bool StartReadFile(const std::string& path, std::string& error);
        bool StartCalculateCrc32(const std::string& path, std::string& error);
        bool StartWriteFile(const std::string& path,
            std::vector<std::uint8_t> bytes, std::string& error);
        bool StartWriteFileStream(const std::string& path,
            std::uint32_t totalBytes,
            std::size_t maximumChunkBytes,
            WriteChunkReader reader,
            void* readerContext,
            std::string& error);

        void Tick(std::chrono::steady_clock::time_point now);
        bool HandleResponse(
            const PeerIdentity& source,
            const std::uint8_t* payload,
            std::size_t payloadSize);
        void Cancel();
        void Shutdown() noexcept;

        bool IsBusy() const;
        bool HasOpenSession() const;
        bool IsAwaitingWriteCommit() const;
        std::optional<OperationResult> ConsumeResult();
        const PeerIdentity& GetPeer() const { return peer_; }
        const ClientStats& GetStats() const { return stats_; }

    private:
        enum class Phase : std::uint8_t
        {
            Idle,
            AwaitReset,
            AwaitOpenRead,
            AwaitRead,
            AwaitTerminateRead,
            AwaitCrc,
            AwaitCreate,
            AwaitTruncate,
            AwaitOpenWrite,
            AwaitWrite,
            AwaitTerminateWrite
        };

        bool Begin(OperationKind kind, Phase phase, Opcode opcode,
            std::uint8_t session, std::uint32_t offset,
            std::vector<std::uint8_t> data, std::string& error);
        bool SendNewRequest(Phase phase, Opcode opcode,
            std::uint8_t session, std::uint32_t offset,
            std::vector<std::uint8_t> data, std::string& error);
        bool SendPending(bool retry, std::string& error);
        void HandleAck(const Payload& response);
        void HandleNak(const Payload& response);
        void CompleteSuccess();
        void CompleteFailure(std::string diagnostic,
            std::uint8_t nakError = 0, bool unsupported = false);
        void CompleteCancelled();
        void BestEffortTerminate() noexcept;
        bool StartNextRead(std::string& error);
        bool StartNextWrite(std::string& error);
        void PopulateOperationMetrics(OperationResult& result) const;
        void ResetWriteSource();

        IRequestTransport& transport_;
        PeerIdentity peer_;
        ClientConfig config_;
        ClientStats stats_;
        Phase phase_ = Phase::Idle;
        OperationKind operation_ = OperationKind::None;
        std::uint16_t nextRequestSequence_ = 0;
        Payload pendingRequest_;
        std::array<std::uint8_t, PayloadBytes> pendingWire_{};
        bool pending_ = false;
        std::uint32_t retriesUsed_ = 0;
        std::chrono::steady_clock::time_point deadline_{};
        std::uint8_t activeSession_ = 0;
        bool sessionOpen_ = false;
        std::string path_;
        std::uint32_t remoteFileSize_ = 0;
        std::vector<std::uint8_t> operationBytes_;
        WriteChunkReader writeChunkReader_ = nullptr;
        void* writeChunkReaderContext_ = nullptr;
        std::size_t operationSize_ = 0;
        std::size_t maximumWriteChunkBytes_ = MaximumDataBytes;
        std::size_t transferred_ = 0;
        std::size_t pendingChunkSize_ = 0;
        std::uint64_t writeBlocksCompleted_ = 0;
        std::uint64_t operationRetriesAtStart_ = 0;
        std::chrono::steady_clock::time_point operationStartedAt_{};
        std::chrono::steady_clock::time_point createConfirmedAt_{};
        std::chrono::steady_clock::time_point terminateStartedAt_{};
        std::optional<OperationResult> completed_;
    };
}
