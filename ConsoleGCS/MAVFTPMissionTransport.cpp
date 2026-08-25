#include "MAVFTPMissionTransport.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace dronegcs::mission_ftp
{
    namespace
    {
        class CodecSegmentRangeSource final : public ISegmentRangeSource
        {
        public:
            bool ReadRange(
                const CompiledMissionSnapshot& snapshot,
                const SegmentDescriptor& segment,
                std::size_t offset,
                std::uint8_t* destination,
                std::size_t size,
                std::string& error) const override
            {
                if (!destination || size == 0 ||
                    offset > segment.encodedBytes ||
                    size > segment.encodedBytes - offset)
                {
                    error = "Mission FTP segment range is outside the immutable snapshot.";
                    return false;
                }

                const auto& items = snapshot.GetWireMissionItems();
                const std::size_t itemBegin = segment.startIndex;
                if (itemBegin + segment.itemCount > items.size())
                {
                    error = "Mission FTP segment descriptor exceeds the immutable wire items.";
                    return false;
                }

                std::array<route_mission::Byte,
                    route_mission::LegacyMissionFtpContainerBytesV1> header{};
                route_mission::Error codecError;
                if (!route_mission::EncodeLegacyMissionFtpContainerV1(
                    segment.startIndex, segment.itemCount, segment.options,
                    header, codecError))
                {
                    error = codecError.Detail.empty()
                        ? "Mission FTP segment header encoding failed."
                        : codecError.Detail;
                    return false;
                }

                std::size_t sourceOffset = offset;
                std::size_t written = 0;
                while (written < size)
                {
                    if (sourceOffset < header.size())
                    {
                        const std::size_t count = (std::min)(
                            size - written, header.size() - sourceOffset);
                        std::copy_n(header.data() + sourceOffset, count,
                            destination + written);
                        sourceOffset += count;
                        written += count;
                        continue;
                    }

                    const std::size_t itemPayloadOffset =
                        sourceOffset - header.size();
                    const std::size_t relativeItem = itemPayloadOffset /
                        route_mission::MissionItemRecordBytesV1;
                    const std::size_t withinItem = itemPayloadOffset %
                        route_mission::MissionItemRecordBytesV1;
                    if (relativeItem >= segment.itemCount)
                    {
                        error = "Mission FTP segment range crossed the planned item count.";
                        return false;
                    }
                    std::array<route_mission::Byte,
                        route_mission::MissionItemRecordBytesV1> itemBytes{};
                    route_mission::EncodeMissionItemRecordV1(
                        items[itemBegin + relativeItem], itemBytes);
                    const std::size_t count = (std::min)(
                        size - written, itemBytes.size() - withinItem);
                    std::copy_n(itemBytes.data() + withinItem, count,
                        destination + written);
                    sourceOffset += count;
                    written += count;
                }
                error.clear();
                return true;
            }
        };

        const ISegmentRangeSource& DefaultSegmentSource()
        {
            static const CodecSegmentRangeSource source;
            return source;
        }

        struct StreamCountContext
        {
            std::uint64_t bytes = 0;
        };

        bool CountStreamBytes(
            const std::uint8_t*,
            const std::size_t size,
            void* const context)
        {
            auto& count = *static_cast<StreamCountContext*>(context);
            count.bytes += size;
            return true;
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

    MAVFTPMissionTransport::MAVFTPMissionTransport(
        mavftp::Client& client,
        MAVFTPMissionTransportConfig config,
        const ISegmentRangeSource* const segmentSource)
        : client_(client)
        , config_(std::move(config))
        , segmentSource_(segmentSource ? segmentSource : &DefaultSegmentSource())
    {
    }

    MAVFTPMissionTransport::MAVFTPMissionTransport(
        mavftp::Client& client,
        IMissionVerificationTransport& verificationTransport,
        MissionVerificationPeerIdentity verificationPeer,
        MAVFTPMissionTransportConfig config,
        const ISegmentRangeSource* const segmentSource)
        : client_(client)
        , verificationTransport_(&verificationTransport)
        , verificationPeer_(std::move(verificationPeer))
        , config_(std::move(config))
        , segmentSource_(segmentSource ? segmentSource : &DefaultSegmentSource())
    {
    }

    bool MAVFTPMissionTransport::Prepare(
        MissionTransportIdentity identity,
        std::vector<route_mission::MissionItemIntV1> mission,
        std::string& outDiagnostic)
    {
        const CompileResult compiled = MissionFtpCompiler::Compile(
            mission, config_.compileConfig);
        if (!compiled.IsValid())
        {
            outDiagnostic = compiled.validation.safeDiagnostic;
            return false;
        }
        if (!PrepareCompiled(identity, compiled.snapshot, outDiagnostic))
            return false;
        if (verificationTransport_)
        {
            MissionUploadTransactionIdentity verificationIdentity;
            verificationIdentity.uploadJobId = identity.uploadJobId;
            verificationIdentity.droneId = identity.droneId;
            verificationIdentity.routeSlotIndex = identity.routeSlotIndex;
            verificationIdentity.peer = verificationPeer_;
            verificationIdentity.transactionGeneration = identity.generation;
            verifier_ = std::make_unique<MissionUploadTransaction>(
                std::move(verificationIdentity), std::move(mission),
                *verificationTransport_, config_.verificationConfig);
        }
        return true;
    }

    bool MAVFTPMissionTransport::PrepareCompiled(
        MissionTransportIdentity identity,
        std::shared_ptr<const CompiledMissionSnapshot> snapshot,
        std::string& outDiagnostic)
    {
        outDiagnostic.clear();
        if (status_ != MissionTransportStatus::Idle || snapshot_)
        {
            outDiagnostic = "MAVFTP Mission Transport is already prepared.";
            return false;
        }
        if (!snapshot || snapshot->GetSegmentDescriptors().empty())
        {
            outDiagnostic = "MAVFTP Mission Transport requires a compiled segment snapshot.";
            return false;
        }
        if (identity.droneId.empty() ||
            identity.droneId != client_.GetPeer().droneId)
        {
            outDiagnostic = "MAVFTP Mission Transport Drone identity differs from the endpoint-bound client.";
            return false;
        }
        if (config_.writeBlockBytes == 0 ||
            config_.writeBlockBytes > mavftp::MaximumDataBytes)
        {
            outDiagnostic = "MAVFTP Mission write block size is outside the protocol bound.";
            return false;
        }

        const auto& segments = snapshot->GetSegmentDescriptors();
        for (std::size_t index = 0; index < segments.size(); ++index)
        {
            StreamCountContext count;
            const ValidationResult validation = MissionFtpCompiler::StreamSegment(
                *snapshot, index, &CountStreamBytes, &count);
            if (!validation.IsValid() || count.bytes != segments[index].encodedBytes)
            {
                outDiagnostic = validation.safeDiagnostic.empty()
                    ? "Compiled Mission stream length differs from its segment descriptor."
                    : validation.safeDiagnostic;
                return false;
            }
            if (segments[index].encodedBytes == 0 ||
                segments[index].encodedBytes >
                    (std::numeric_limits<std::uint32_t>::max)())
            {
                outDiagnostic = "Compiled Mission segment exceeds the MAVFTP writer size bound.";
                return false;
            }
        }

        identity_ = std::move(identity);
        snapshot_ = std::move(snapshot);
        metrics_ = {};
        metrics_.wireMissionItems = snapshot_->GetTotalWireMissionItems();
        metrics_.artifactRouteItems = metrics_.wireMissionItems > 0
            ? metrics_.wireMissionItems - 1 : 0;
        mavftpMetrics_ = {};
        mavftpMetrics_.segmentsPlanned =
            snapshot_->GetSegmentDescriptors().size();
        mavftpMetrics_.segmentCapacity = config_.compileConfig.segmentCapacity;
        ackRttSamples_.clear();
        ackRttSamples_.reserve(MaximumAckTimingSamples);
        ackRttSampleCursor_ = 0;
        ackRttWeightedSumMs_ = 0;
        ackRttCount_ = 0;
        interAckWeightedSumMs_ = 0;
        interAckCount_ = 0;
        currentSegmentIndex_ = 0;
        safeDiagnostic_.clear();
        verificationMismatchDiagnostic_.clear();
        terminalPhase_ = "Prepared";
        status_ = MissionTransportStatus::Prepared;
        progressChanged_ = true;
        return true;
    }

    bool MAVFTPMissionTransport::Upload(
        const std::chrono::steady_clock::time_point now)
    {
        if (status_ != MissionTransportStatus::Prepared || !snapshot_)
        {
            safeDiagnostic_ = "MAVFTP Mission Transport is not prepared.";
            return false;
        }
        if (client_.IsBusy())
        {
            safeDiagnostic_ =
                "MAVFTP Client is busy with another endpoint-bound operation.";
            return false;
        }
        uploadStartedAt_ = now;
        currentSegmentIndex_ = 0;
        status_ = MissionTransportStatus::Uploading;
        terminalPhase_ = "Create";
        progressChanged_ = true;
        if (!StartCurrentSegment())
        {
            Fail("CreateFailed", safeDiagnostic_.empty()
                ? "CreateFailed: MAVFTP Mission segment could not start."
                : safeDiagnostic_, now);
            return false;
        }
        return true;
    }

    bool MAVFTPMissionTransport::StartCurrentSegment()
    {
        const auto& segments = snapshot_->GetSegmentDescriptors();
        if (currentSegmentIndex_ >= segments.size())
        {
            safeDiagnostic_ = "CreateFailed: Mission segment index is out of range.";
            return false;
        }
        const SegmentDescriptor& segment = segments[currentSegmentIndex_];
        mavftpMetrics_.activeSegmentIndex = currentSegmentIndex_;
        std::string error;
        if (!client_.StartWriteFileStream(
            MissionPath,
            static_cast<std::uint32_t>(segment.encodedBytes),
            config_.writeBlockBytes,
            &MAVFTPMissionTransport::ReadChunk,
            this,
            error))
        {
            safeDiagnostic_ = "CreateFailed: " + error;
            return false;
        }
        status_ = MissionTransportStatus::Uploading;
        terminalPhase_ = "Create";
        return true;
    }

    bool MAVFTPMissionTransport::ReadChunk(
        const std::size_t offset,
        std::uint8_t* const destination,
        const std::size_t size,
        void* const context,
        std::string& error)
    {
        auto& self = *static_cast<MAVFTPMissionTransport*>(context);
        if (!self.snapshot_ || self.currentSegmentIndex_ >=
            self.snapshot_->GetSegmentDescriptors().size())
        {
            error = "StreamRejected: immutable Mission segment is unavailable.";
            return false;
        }
        const SegmentDescriptor& segment =
            self.snapshot_->GetSegmentDescriptors()[self.currentSegmentIndex_];
        if (!self.segmentSource_->ReadRange(
            *self.snapshot_, segment, offset, destination, size, error))
        {
            error = "StreamRejected: " + (error.empty()
                ? std::string("Mission segment source rejected a block.")
                : error);
            return false;
        }
        return true;
    }

    void MAVFTPMissionTransport::Tick(
        const std::chrono::steady_clock::time_point now)
    {
        if (status_ == MissionTransportStatus::Verifying)
        {
            verifier_->Tick(now);
            if (!verifier_->IsTerminal()) return;
            const MissionUploadTransactionMetrics& verificationMetrics =
                verifier_->GetMetrics();
            metrics_.wireBytesSent += verificationMetrics.wireBytesSent;
            metrics_.missionMessagesSent += verificationMetrics.missionMessagesSent;
            metrics_.missionRequestsReceived +=
                verificationMetrics.missionRequestsReceived;
            metrics_.retryCount += verificationMetrics.retryCount;
            metrics_.verificationDurationMs =
                verificationMetrics.verificationDurationMs;
            verificationMismatchDiagnostic_ =
                verifier_->GetVerificationMismatchDiagnostic();
            if (verifier_->Succeeded())
            {
                status_ = MissionTransportStatus::Succeeded;
                terminalPhase_ = "Verified";
                safeDiagnostic_.clear();
                progressChanged_ = true;
            }
            else if (verifier_->GetPhase() == MissionUploadPhase::Cancelled)
            {
                status_ = MissionTransportStatus::Cancelled;
                terminalPhase_ = "Cancelled";
                safeDiagnostic_ = verifier_->GetSafeDiagnostic();
            }
            else
            {
                Fail(MissionUploadPhaseName(verifier_->GetTerminalSourcePhase()),
                    verifier_->GetSafeDiagnostic(), now);
            }
            return;
        }
        if (status_ != MissionTransportStatus::Uploading &&
            status_ != MissionTransportStatus::WaitingAck)
        {
            return;
        }

        if (config_.driveClientTick) client_.Tick(now);
        UpdateLiveLogicalProgress();
        status_ = client_.IsAwaitingWriteCommit()
            ? MissionTransportStatus::WaitingAck
            : MissionTransportStatus::Uploading;
        if (status_ == MissionTransportStatus::WaitingAck)
            terminalPhase_ = "TerminatePending";

        std::optional<mavftp::OperationResult> result = client_.ConsumeResult();
        if (!result) return;
        ApplyOperationMetrics(*result);
        if (!result->success)
        {
            std::string phase = "WriteFailed";
            if (result->safeDiagnostic.find("StreamRejected:") != std::string::npos)
                phase = "StreamRejected";
            else if (result->terminalRequestOpcode == mavftp::Opcode::CreateFile ||
                result->terminalRequestOpcode == mavftp::Opcode::TruncateFile ||
                result->terminalRequestOpcode == mavftp::Opcode::OpenFileWO)
                phase = "CreateFailed";
            else if (result->terminalRequestOpcode == mavftp::Opcode::TerminateSession)
                phase = "TerminateFailed";
            else if (result->safeDiagnostic.find("OffsetMismatch:") != std::string::npos)
                phase = "OffsetMismatch";
            Fail(phase, phase + ": " + result->safeDiagnostic, now);
            return;
        }

        ++mavftpMetrics_.segmentsCommitted;
        const SegmentDescriptor& committed =
            snapshot_->GetSegmentDescriptors()[currentSegmentIndex_];
        const std::uint64_t wireItemsCommitted =
            static_cast<std::uint64_t>(committed.startIndex) +
            committed.itemCount;
        metrics_.uniqueItemsServed = wireItemsCommitted;
        const std::uint64_t artifactItemsCommitted = wireItemsCommitted > 0
            ? wireItemsCommitted - 1 : 0;
        metrics_.logicalBytesCompleted = (std::min)(
            artifactItemsCommitted, metrics_.artifactRouteItems) *
            route_mission::MissionItemRecordBytesV1;
        progressChanged_ = true;

        ++currentSegmentIndex_;
        if (currentSegmentIndex_ < snapshot_->GetSegmentDescriptors().size())
        {
            if (!StartCurrentSegment())
            {
                Fail("CreateFailed", safeDiagnostic_, now);
            }
            return;
        }

        UpdateElapsed(now);
        if (verifier_)
        {
            status_ = MissionTransportStatus::Verifying;
            terminalPhase_ = "VerifyingCount";
            progressChanged_ = true;
            if (!verifier_->BeginVerification(now, true))
            {
                Fail("VerifyingCount", verifier_->GetSafeDiagnostic(), now);
            }
            return;
        }
        status_ = MissionTransportStatus::Succeeded;
        terminalPhase_ = "CommitConfirmed";
        safeDiagnostic_.clear();
    }

    void MAVFTPMissionTransport::ApplyOperationMetrics(
        const mavftp::OperationResult& result)
    {
        mavftpMetrics_.createDurationMs += result.createDurationMs;
        mavftpMetrics_.writeBytes += result.writeBytes;
        mavftpMetrics_.writeBlocks += result.writeBlocks;
        mavftpMetrics_.writeBlocksSent += result.writeBlocksSent;
        mavftpMetrics_.writeBlocksAcked += result.writeBlocksAcked;
        mavftpMetrics_.retryCount += result.retryCount;
        mavftpMetrics_.createRetries += result.createRetries;
        mavftpMetrics_.writeRetries += result.writeRetries;
        mavftpMetrics_.terminateRetries += result.terminateRetries;
        mavftpMetrics_.duplicateAcks += result.duplicateAcks;
        mavftpMetrics_.staleOrMismatchedAcks += result.staleOrMismatchedAcks;
        mavftpMetrics_.permitGrantCount += result.permitGrantCount;
        mavftpMetrics_.permitWaitCount += result.permitWaitCount;
        mavftpMetrics_.configuredWriteWindow = result.configuredWriteWindow;
        mavftpMetrics_.maxObservedOutstanding = (std::max)(
            mavftpMetrics_.maxObservedOutstanding,
            result.maxObservedOutstanding);
        mavftpMetrics_.writeDataDurationMs += result.writeDataDurationMs;
        mavftpMetrics_.terminateDurationMs += result.terminateDurationMs;
        mavftpMetrics_.commitDurationMs += result.terminateDurationMs;
        metrics_.wireBytesSent = mavftpMetrics_.writeBytes;
        metrics_.retryCount = mavftpMetrics_.retryCount;

        ackRttWeightedSumMs_ += result.ackRttAverageMs *
            result.writeBlocksAcked;
        ackRttCount_ += result.writeBlocksAcked;
        const std::uint64_t segmentInterAckCount = result.writeBlocksAcked > 0
            ? result.writeBlocksAcked - 1 : 0;
        interAckWeightedSumMs_ += result.interAckAverageMs *
            segmentInterAckCount;
        interAckCount_ += segmentInterAckCount;
        for (const std::uint64_t sample : result.boundedAckRttSamples)
        {
            if (ackRttSamples_.size() < MaximumAckTimingSamples)
                ackRttSamples_.push_back(sample);
            else
            {
                ackRttSamples_[ackRttSampleCursor_] = sample;
                ackRttSampleCursor_ = (ackRttSampleCursor_ + 1) %
                    MaximumAckTimingSamples;
            }
        }
        mavftpMetrics_.ackRttAverageMs = ackRttCount_ == 0
            ? 0 : ackRttWeightedSumMs_ / ackRttCount_;
        mavftpMetrics_.interAckAverageMs = interAckCount_ == 0
            ? 0 : interAckWeightedSumMs_ / interAckCount_;
        if (!ackRttSamples_.empty())
        {
            std::vector<std::uint64_t> sorted = ackRttSamples_;
            std::sort(sorted.begin(), sorted.end());
            const auto percentile = [&sorted](const std::size_t percent)
            {
                const std::size_t index =
                    (percent * (sorted.size() - 1) + 99) / 100;
                return sorted[(std::min)(index, sorted.size() - 1)];
            };
            mavftpMetrics_.ackRttMinMs = sorted.front();
            mavftpMetrics_.ackRttP50Ms = percentile(50);
            mavftpMetrics_.ackRttP95Ms = percentile(95);
            mavftpMetrics_.ackRttP99Ms = percentile(99);
            mavftpMetrics_.maxAckRttMs = sorted.back();
        }
    }

    void MAVFTPMissionTransport::UpdateLiveLogicalProgress()
    {
        if (!snapshot_ || currentSegmentIndex_ >=
            snapshot_->GetSegmentDescriptors().size())
        {
            return;
        }

        const SegmentDescriptor& segment =
            snapshot_->GetSegmentDescriptors()[currentSegmentIndex_];
        const std::uint64_t acknowledgedEncodedBytes = (std::min)(
            client_.GetAcknowledgedWriteBytes(), segment.encodedBytes);
        const ByteLayoutInfo& layout = snapshot_->GetByteLayoutInfo();
        const std::uint64_t acknowledgedItems = layout.itemBytes == 0 ||
            acknowledgedEncodedBytes <= layout.headerBytes
            ? 0
            : (std::min<std::uint64_t>)(
                segment.itemCount,
                (acknowledgedEncodedBytes - layout.headerBytes) /
                    layout.itemBytes);
        const std::uint64_t acknowledgedWireItems =
            static_cast<std::uint64_t>(segment.startIndex) + acknowledgedItems;
        const std::uint64_t acknowledgedArtifactItems = acknowledgedWireItems > 0
            ? acknowledgedWireItems - 1 : 0;
        const std::uint64_t logicalBytes = (std::min)(
            acknowledgedArtifactItems, metrics_.artifactRouteItems) *
            layout.itemBytes;
        if (logicalBytes > metrics_.logicalBytesCompleted)
        {
            metrics_.logicalBytesCompleted = logicalBytes;
            progressChanged_ = true;
        }
    }

    void MAVFTPMissionTransport::Fail(
        std::string phase,
        std::string diagnostic,
        const std::chrono::steady_clock::time_point now)
    {
        const bool verificationWasActive =
            status_ == MissionTransportStatus::Verifying;
        status_ = MissionTransportStatus::Failed;
        terminalPhase_ = std::move(phase);
        safeDiagnostic_ = std::move(diagnostic);
        progressChanged_ = true;
        if (!verificationWasActive) UpdateElapsed(now);
    }

    void MAVFTPMissionTransport::UpdateElapsed(
        const std::chrono::steady_clock::time_point now)
    {
        const std::uint64_t elapsed = ElapsedMilliseconds(uploadStartedAt_, now);
        mavftpMetrics_.totalMavftpDurationMs = elapsed;
        metrics_.uploadDurationMs = elapsed;
    }

    bool MAVFTPMissionTransport::Verify() const
    {
        // C.3.C.1 commit policy: every segment requires a TerminateSession ACK.
        return status_ == MissionTransportStatus::Succeeded;
    }

    void MAVFTPMissionTransport::Cancel(
        const std::chrono::steady_clock::time_point now)
    {
        if (IsTerminal()) return;
        UpdateLiveLogicalProgress();
        const bool verificationWasActive =
            status_ == MissionTransportStatus::Verifying;
        if (verifier_) verifier_->Cancel(now);
        if (client_.IsBusy()) client_.Cancel();
        if (const auto result = client_.ConsumeResult())
            ApplyOperationMetrics(*result);
        status_ = MissionTransportStatus::Cancelled;
        terminalPhase_ = "Cancelled";
        safeDiagnostic_ = "MAVFTP Mission upload cancelled.";
        progressChanged_ = true;
        if (!verificationWasActive) UpdateElapsed(now);
    }

    bool MAVFTPMissionTransport::IsTerminal() const
    {
        return status_ == MissionTransportStatus::Succeeded ||
            status_ == MissionTransportStatus::Failed ||
            status_ == MissionTransportStatus::Cancelled;
    }

    bool MAVFTPMissionTransport::ConsumeProgressChanged()
    {
        const bool result = progressChanged_;
        progressChanged_ = false;
        return result;
    }

    MissionTransportMetrics MAVFTPMissionTransport::GetMetrics() const
    {
        return metrics_;
    }

    bool MAVFTPMissionTransport::HandleMissionMessage(
        const mavlink_message_t& message,
        const MissionVerificationPeerIdentity& peer,
        const std::chrono::steady_clock::time_point now)
    {
        return verifier_ && verifier_->HandleMissionMessage(message, peer, now);
    }
}
