#include "MissionProtocolTransport.h"

#include <utility>

const std::string MissionProtocolTransport::EmptyDiagnostic{};

MissionProtocolTransport::MissionProtocolTransport(
    IMissionProtocolTransport& wireTransport,
    MissionProtocolPeerIdentity peer,
    const MissionUploadTransactionConfig config)
    : wireTransport_(wireTransport)
    , peer_(std::move(peer))
    , config_(config)
{
}

bool MissionProtocolTransport::Prepare(
    MissionTransportIdentity identity,
    std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
    std::string& outDiagnostic)
{
    outDiagnostic.clear();
    if (prepared_ || transaction_)
    {
        outDiagnostic = "Mission Transport is already prepared.";
        return false;
    }

    MissionUploadTransactionIdentity transactionIdentity;
    transactionIdentity.uploadJobId = std::move(identity.uploadJobId);
    transactionIdentity.droneId = std::move(identity.droneId);
    transactionIdentity.routeSlotIndex = identity.routeSlotIndex;
    transactionIdentity.peer = peer_;
    transactionIdentity.transactionGeneration = identity.generation;
    transaction_ = std::make_unique<MissionUploadTransaction>(
        std::move(transactionIdentity), std::move(mission), wireTransport_, config_);
    prepared_ = true;
    return true;
}

bool MissionProtocolTransport::Upload(
    const std::chrono::steady_clock::time_point now)
{
    return prepared_ && transaction_ && transaction_->Start(now);
}

bool MissionProtocolTransport::Verify() const
{
    return transaction_ && transaction_->Succeeded();
}

void MissionProtocolTransport::Tick(
    const std::chrono::steady_clock::time_point now)
{
    if (transaction_) transaction_->Tick(now);
}

void MissionProtocolTransport::Cancel(
    const std::chrono::steady_clock::time_point now)
{
    if (transaction_) transaction_->Cancel(now);
}

MissionTransportStatus MissionProtocolTransport::GetStatus() const
{
    if (!transaction_)
        return prepared_ ? MissionTransportStatus::Prepared
                         : MissionTransportStatus::Idle;
    switch (transaction_->GetPhase())
    {
    case MissionUploadPhase::Idle:
        return prepared_ ? MissionTransportStatus::Prepared
                         : MissionTransportStatus::Idle;
    case MissionUploadPhase::AwaitingRequest:
        return MissionTransportStatus::Uploading;
    case MissionUploadPhase::AwaitingAck:
        return MissionTransportStatus::WaitingAck;
    case MissionUploadPhase::VerifyingCount:
    case MissionUploadPhase::VerifyingFirst:
    case MissionUploadPhase::VerifyingLast:
        return MissionTransportStatus::Verifying;
    case MissionUploadPhase::Succeeded:
        return MissionTransportStatus::Succeeded;
    case MissionUploadPhase::Failed:
        return MissionTransportStatus::Failed;
    case MissionUploadPhase::Cancelled:
        return MissionTransportStatus::Cancelled;
    }
    return MissionTransportStatus::Failed;
}

bool MissionProtocolTransport::IsTerminal() const
{
    return transaction_ && transaction_->IsTerminal();
}

bool MissionProtocolTransport::ConsumeProgressChanged()
{
    return transaction_ && transaction_->ConsumeProgressChanged();
}

MissionTransportMetrics MissionProtocolTransport::GetMetrics() const
{
    MissionTransportMetrics result;
    if (!transaction_) return result;
    const MissionUploadTransactionMetrics& source = transaction_->GetMetrics();
    result.logicalBytesCompleted = source.logicalBytesCompleted;
    result.artifactRouteItems = source.artifactRouteItems;
    result.wireMissionItems = source.wireMissionItems;
    result.uniqueItemsServed = source.uniqueItemsServed;
    result.wireBytesSent = source.wireBytesSent;
    result.missionMessagesSent = source.missionMessagesSent;
    result.missionRequestsReceived = source.missionRequestsReceived;
    result.duplicateRequests = source.duplicateRequests;
    result.retryCount = source.retryCount;
    result.uploadDurationMs = source.uploadDurationMs;
    result.verificationDurationMs = source.verificationDurationMs;
    return result;
}

const std::string& MissionProtocolTransport::GetSafeDiagnostic() const
{
    return transaction_ ? transaction_->GetSafeDiagnostic() : EmptyDiagnostic;
}

const std::string&
MissionProtocolTransport::GetVerificationMismatchDiagnostic() const
{
    return transaction_ ? transaction_->GetVerificationMismatchDiagnostic()
                        : EmptyDiagnostic;
}

const char* MissionProtocolTransport::GetTerminalPhaseName() const
{
    return transaction_
        ? MissionUploadPhaseName(transaction_->GetTerminalSourcePhase())
        : MissionUploadPhaseName(MissionUploadPhase::Idle);
}

bool MissionProtocolTransport::HandleMissionMessage(
    const mavlink_message_t& message,
    const MissionProtocolPeerIdentity& peer,
    const std::chrono::steady_clock::time_point now)
{
    return transaction_ && transaction_->HandleMissionMessage(message, peer, now);
}

std::unique_ptr<IMissionTransport> CreateMissionProtocolTransport(
    IMissionProtocolTransport& wireTransport,
    MissionProtocolPeerIdentity peer,
    const MissionUploadTransactionConfig config)
{
    return std::make_unique<MissionProtocolTransport>(
        wireTransport, std::move(peer), config);
}
