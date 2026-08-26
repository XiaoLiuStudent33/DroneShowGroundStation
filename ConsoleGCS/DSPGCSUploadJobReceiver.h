#pragma once

#include "ConsoleUploadJobAuthority.h"
#include "DSPGCSMavFtpRuntimeConfigProtocol.h"
#include "IDSPGCSTransport.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

struct DSPGCSUploadJobProcessStats
{
    std::size_t framesProcessed = 0;
    std::size_t framesRejected = 0;
    std::size_t acknowledgementsSent = 0;
    std::size_t snapshotsSent = 0;
    std::size_t pagesSent = 0;
};

class DSPGCSUploadJobReceiver final
{
public:
    static constexpr std::size_t MaxQueuedFrames = 4096;
    static bool IsIngressMessageType(const dspgcs::MessageType messageType)
    {
        return dspgcs::IsUploadJobMessageType(messageType) ||
            messageType == dspgcs::MavFtpRuntimeConfigMessageTypes::Request;
    }
    using AckSender = std::function<bool(const dspgcs::UploadJobCommandAckDTO&)>;
    using SnapshotSender = std::function<bool(const dspgcs::UploadJobSnapshotDTO&)>;
    using PageSender = std::function<bool(const dspgcs::UploadJobRouteStatePageDTO&)>;
    using RuntimeConfigAckSender = std::function<bool(
        const dspgcs::MavFtpRuntimeConfigAckDTO&)>;
    using RuntimeConfigSnapshotSender = std::function<bool(
        const dspgcs::MavFtpRuntimeConfigSnapshotDTO&)>;

    void EnqueueFrame(dspgcs::TransportFrame frame);
    DSPGCSUploadJobProcessStats ProcessPending(
        ConsoleUploadJobAuthority& authority,
        const AckSender& sendAck,
        const SnapshotSender& sendSnapshot,
        const PageSender& sendPage,
        const RuntimeConfigAckSender& sendRuntimeConfigAck = {},
        const RuntimeConfigSnapshotSender& sendRuntimeConfigSnapshot = {});
    std::size_t GetQueuedFrameCount() const;
    std::size_t GetDroppedFrameCount() const;

private:
    mutable std::mutex mutex_;
    std::vector<dspgcs::TransportFrame> pendingFrames_;
    std::size_t droppedFrameCount_ = 0;
};
