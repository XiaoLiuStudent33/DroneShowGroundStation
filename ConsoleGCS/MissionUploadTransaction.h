#pragma once

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "RouteMissionCodec.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C"
{
#include "ardupilotmega/mavlink.h"
}

struct MissionProtocolPeerIdentity
{
    std::string droneId;
    std::string remoteIp;
    std::uint16_t remotePort = 0;
    std::uint8_t systemId = 0;
    std::uint8_t componentId = 0;

    bool operator==(const MissionProtocolPeerIdentity& other) const
    {
        return droneId == other.droneId && remoteIp == other.remoteIp &&
            remotePort == other.remotePort && systemId == other.systemId &&
            componentId == other.componentId;
    }
};

class IMissionProtocolTransport
{
public:
    virtual ~IMissionProtocolTransport() = default;
    virtual bool SendMissionCount(std::uint16_t count, std::size_t& wireBytes) = 0;
    virtual bool SendMissionItemInt(
        const dronegcs::route_mission::MissionItemIntV1& item,
        std::size_t& wireBytes) = 0;
    virtual bool SendMissionRequestList(std::size_t& wireBytes) = 0;
    virtual bool SendMissionRequestInt(
        std::uint16_t sequence,
        std::size_t& wireBytes) = 0;
};

class IMissionProtocolMessageSink
{
public:
    virtual ~IMissionProtocolMessageSink() = default;
    virtual bool HandleMissionMessage(
        const mavlink_message_t& message,
        const MissionProtocolPeerIdentity& peer,
        std::chrono::steady_clock::time_point now) = 0;
};

enum class MissionUploadPhase : std::uint8_t
{
    Idle,
    AwaitingRequest,
    AwaitingAck,
    VerifyingCount,
    VerifyingHome,
    VerifyingFirst,
    VerifyingLast,
    Succeeded,
    Failed,
    Cancelled
};

const char* MissionUploadPhaseName(MissionUploadPhase phase);

struct MissionUploadTransactionIdentity
{
    std::string uploadJobId;
    std::string droneId;
    std::int32_t routeSlotIndex = -1;
    MissionProtocolPeerIdentity peer;
    std::uint64_t transactionGeneration = 0;
};

struct MissionUploadTransactionMetrics
{
    std::uint64_t logicalBytesCompleted = 0;
	std::uint64_t artifactRouteItems = 0;
	std::uint64_t wireMissionItems = 0;
	std::uint64_t uniqueItemsServed = 0;
    std::uint64_t wireBytesSent = 0;
    std::uint64_t missionMessagesSent = 0;
    std::uint64_t missionRequestsReceived = 0;
    std::uint64_t duplicateRequests = 0;
    std::uint64_t retryCount = 0;
    std::uint64_t uploadDurationMs = 0;
    std::uint64_t verificationDurationMs = 0;
};

inline constexpr std::size_t MissionVerificationMismatchDiagnosticMaxBytes = 2048;

struct MissionUploadTransactionConfig
{
    std::chrono::milliseconds responseTimeout{ 750 };
    std::uint32_t maxRetries = 3;
};

/** Tick-driven, endpoint-bound standard MAVLink Mission upload transaction. */
class MissionUploadTransaction final : public IMissionProtocolMessageSink
{
public:
    MissionUploadTransaction(
        MissionUploadTransactionIdentity identity,
        std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
        IMissionProtocolTransport& transport,
        MissionUploadTransactionConfig config = {});

    bool Start(std::chrono::steady_clock::time_point now);
    /** Starts the shared production verifier without sending Mission upload data. */
    bool BeginVerification(
        std::chrono::steady_clock::time_point now,
        bool observeReservedHome);
    bool HandleMissionMessage(
        const mavlink_message_t& message,
        const MissionProtocolPeerIdentity& peer,
        std::chrono::steady_clock::time_point now) override;
    void Tick(std::chrono::steady_clock::time_point now);
    void Cancel(std::chrono::steady_clock::time_point now);

    MissionUploadPhase GetPhase() const { return phase_; }
	MissionUploadPhase GetTerminalSourcePhase() const { return terminalSourcePhase_; }
    bool IsTerminal() const;
    bool Succeeded() const { return phase_ == MissionUploadPhase::Succeeded; }
    bool TransportAccepted() const { return transportAccepted_; }
    bool ConsumeProgressChanged();
    const std::string& GetSafeDiagnostic() const { return safeDiagnostic_; }
    const std::string& GetVerificationMismatchDiagnostic() const
        { return verificationMismatchDiagnostic_; }
    const MissionUploadTransactionIdentity& GetIdentity() const { return identity_; }
    const MissionUploadTransactionMetrics& GetMetrics() const { return metrics_; }
    std::size_t GetMissionItemCount() const
        { return static_cast<std::size_t>(metrics_.artifactRouteItems); }
    std::size_t GetWireMissionItemCount() const { return mission_.size(); }

private:
    bool SendCount(std::chrono::steady_clock::time_point now);
    bool SendItem(std::uint16_t sequence,
        std::chrono::steady_clock::time_point now,
        bool countDuplicateRequest = true);
    bool SendVerificationList(std::chrono::steady_clock::time_point now);
    bool SendVerificationItem(
        std::uint16_t sequence,
        MissionUploadPhase phase,
        std::chrono::steady_clock::time_point now);
    bool RecordSend(bool sent, std::size_t wireBytes,
        std::chrono::steady_clock::time_point now);
    void ResetDeadline(std::chrono::steady_clock::time_point now);
    void ResetRetryBudget();
    void Fail(const char* diagnostic,
        std::chrono::steady_clock::time_point now);
    void FinalizeTiming(std::chrono::steady_clock::time_point now);
    bool VerifyItem(const mavlink_mission_item_int_t& received,
        const dronegcs::route_mission::MissionItemIntV1& expected) const;
    bool MatchesTarget(std::uint8_t targetSystem,
        std::uint8_t targetComponent, std::uint8_t missionType) const;

    MissionUploadTransactionIdentity identity_;
    std::vector<dronegcs::route_mission::MissionItemIntV1> mission_;
    IMissionProtocolTransport& transport_;
    MissionUploadTransactionConfig config_;
    bool wireMissionValid_ = false;
    MissionUploadPhase phase_ = MissionUploadPhase::Idle;
	MissionUploadPhase terminalSourcePhase_ = MissionUploadPhase::Idle;
    std::vector<bool> served_;
    std::size_t uniqueServed_ = 0;
    std::uint16_t lastRequestedSequence_ = 0;
    std::uint32_t retriesInWait_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point uploadStartedAt_{};
    std::chrono::steady_clock::time_point verificationStartedAt_{};
    MissionUploadTransactionMetrics metrics_;
    std::string safeDiagnostic_;
    std::string verificationMismatchDiagnostic_;
    bool transportAccepted_ = false;
    bool observeReservedHome_ = false;
    bool progressChanged_ = false;
};
