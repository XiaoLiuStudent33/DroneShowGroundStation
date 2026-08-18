#include "DSPGCSUploadJobReceiver.h"

#include "DSPGCSUploadJobProtocol.h"

void DSPGCSUploadJobReceiver::EnqueueFrame(dspgcs::TransportFrame frame)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (pendingFrames_.size() >= MaxQueuedFrames)
    {
        ++droppedFrameCount_;
        return;
    }
    pendingFrames_.push_back(std::move(frame));
}

DSPGCSUploadJobProcessStats DSPGCSUploadJobReceiver::ProcessPending(
    ConsoleUploadJobAuthority& authority,
    const AckSender& sendAck,
    const SnapshotSender& sendSnapshot,
    const PageSender& sendPage)
{
    std::vector<dspgcs::TransportFrame> frames;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames.swap(pendingFrames_);
    }
    DSPGCSUploadJobProcessStats stats;
    for (auto& frame : frames)
    {
        ++stats.framesProcessed;
        dspgcs::MessageEnvelope envelope;
        std::vector<std::uint8_t> payload;
        if (!dspgcs::DecodeEnvelope(frame.bytes, envelope, payload) ||
            !dspgcs::IsUploadJobMessageType(envelope.messageType))
        {
            ++stats.framesRejected;
            continue;
        }
        if (envelope.messageType == dspgcs::UploadJobMessageTypes::Command)
        {
            dspgcs::UploadJobCommandDTO command;
            std::string error;
            if (!dspgcs::DecodeUploadJobCommand(payload, command, error))
            {
                ++stats.framesRejected;
                continue;
            }
            const auto acknowledgement = authority.HandleCommand(
                envelope.sessionId, command, payload);
            if (sendAck && sendAck(acknowledgement)) ++stats.acknowledgementsSent;
            continue;
        }
        if (envelope.messageType == dspgcs::UploadJobMessageTypes::SnapshotRequest)
        {
            dspgcs::UploadJobSnapshotRequestDTO request;
            std::string error;
            if (!dspgcs::DecodeUploadJobSnapshotRequest(payload, request, error))
            {
                ++stats.framesRejected;
                continue;
            }
            if (request.requestKind == dspgcs::UploadJobSnapshotRequestKind::Aggregate)
            {
                const auto snapshot = authority.BuildSnapshot(request);
                if (sendSnapshot && sendSnapshot(snapshot)) ++stats.snapshotsSent;
            }
            else
            {
                dspgcs::UploadJobRouteStatePageDTO page;
                dspgcs::UploadJobErrorCode errorCode;
                if (authority.BuildRoutePage(request, page, errorCode))
                {
                    if (sendPage && sendPage(page)) ++stats.pagesSent;
                }
                else
                {
                    dspgcs::UploadJobSnapshotDTO errorSnapshot;
                    errorSnapshot.clientRequestId = request.clientRequestId;
                    errorSnapshot.uploadJobId = request.uploadJobId;
                    errorSnapshot.resultCode = errorCode;
                    errorSnapshot.safeDiagnostic =
                        "Route page snapshot is unavailable or stale.";
                    if (sendSnapshot && sendSnapshot(errorSnapshot)) ++stats.snapshotsSent;
                }
            }
            continue;
        }
        ++stats.framesRejected;
    }
    return stats;
}

std::size_t DSPGCSUploadJobReceiver::GetQueuedFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pendingFrames_.size();
}

std::size_t DSPGCSUploadJobReceiver::GetDroppedFrameCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedFrameCount_;
}
