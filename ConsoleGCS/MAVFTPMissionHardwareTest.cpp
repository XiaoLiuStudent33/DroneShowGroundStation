#include "MAVFTPMissionHardwareTest.h"

#include "AppConfig.h"
#include "ConsoleUI.h"
#include "RouteArtifactContractV2.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

extern "C"
{
#include "mdhk/mavlink.h"
}

namespace
{
    namespace fs = std::filesystem;

    constexpr std::uint64_t MaximumBootstrapManifestBytes =
        64ULL * 1024ULL * 1024ULL;
    constexpr std::chrono::milliseconds ResponseTimeout{ 1000 };
    constexpr std::uint32_t MaximumResponseRetries = 3;

    const char* YesNo(const bool value)
    {
        return value ? "yes" : "no";
    }
}

bool ResolveMAVFTPMissionTransportOptions(
    const AppConfig& appConfig,
    const bool segmentItemsProvidedByCli,
    const bool terminateTimeoutProvidedByCli,
    const bool writeWindowProvidedByCli,
    MAVFTPMissionHardwareTestOptions& options,
    std::string& error)
{
    error.clear();
    if (!segmentItemsProvidedByCli)
        options.segmentItemCapacity = appConfig.mavftpSegmentItems;
    if (!writeWindowProvidedByCli)
        options.writeWindow = appConfig.mavftpWriteWindow;
    options.requestTimeoutMs = appConfig.mavftpRequestTimeoutMs;
    if (!terminateTimeoutProvidedByCli)
        options.terminateTimeoutMs = appConfig.mavftpTerminateTimeoutMs;
    options.requestRetryCount = appConfig.mavftpRequestRetryCount;

    if (options.segmentItemCapacity == 0 ||
        options.segmentItemCapacity > 65535U)
        error = "MAVFTP segment capacity must be within 1..65535 Wire Mission Items.";
    else if (options.requestTimeoutMs < 100 ||
        options.requestTimeoutMs > 60000)
        error = "MAVFTP request timeout must be within 100..60000 milliseconds.";
    else if (options.terminateTimeoutMs < 100 ||
        options.terminateTimeoutMs > 60000)
        error = "MAVFTP terminate timeout must be within 100..60000 milliseconds.";
    else if (options.requestRetryCount > 10U)
        error = "MAVFTP request retry count must be within 0..10.";
    else if (options.writeWindow == 0 || options.writeWindow > 16U)
        error = "MAVFTP write window must be within 1..16.";
    return error.empty();
}

bool ResolveMAVFTPQuickTestOptionsFromCurrent(
    const std::string& artifactWorkspaceRoot,
    MAVFTPMissionHardwareTestOptions& options,
    std::string& error)
{
    error.clear();
    if (!options.quickTest) return true;
    if (!options.enabled)
    {
        error = "MAVFTP quick test requires the explicit MAVFTP hardware-test mode.";
        return false;
    }
    if (artifactWorkspaceRoot.empty())
    {
        error = "MAVFTP quick test requires artifact_workspace_root.";
        return false;
    }

    const fs::path manifestPath = fs::u8path(artifactWorkspaceRoot) /
        L"Current" / L"manifest.json";
    std::error_code fileError;
    const std::uintmax_t manifestSize = fs::file_size(manifestPath, fileError);
    if (fileError || manifestSize == 0 ||
        manifestSize > MaximumBootstrapManifestBytes ||
        manifestSize > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max()))
    {
        error = "MAVFTP quick test Current manifest is missing or outside the bounded size.";
        return false;
    }

    std::ifstream manifestFile(manifestPath, std::ios::binary);
    std::string manifestBytes(static_cast<std::size_t>(manifestSize), '\0');
    if (!manifestFile.read(
            manifestBytes.data(), static_cast<std::streamsize>(manifestSize)))
    {
        error = "MAVFTP quick test could not read Current/manifest.json.";
        return false;
    }

    dronegcs::route_artifact::v2::Manifest manifest;
    dronegcs::route_artifact::Error contractError;
    if (!dronegcs::route_artifact::v2::ParseCanonicalManifest(
            manifestBytes, manifest, contractError))
    {
        error = "MAVFTP quick test Current manifest is not canonical schema v2.";
        return false;
    }
    const auto route = std::find_if(
        manifest.Routes.begin(), manifest.Routes.end(),
        [&options](const dronegcs::route_artifact::RouteRecord& candidate)
        {
            return candidate.RouteSlotIndex == options.routeSlotIndex;
        });
    if (route == manifest.Routes.end())
    {
        error = "MAVFTP quick test RouteSlotIndex is absent from Current Artifact.";
        return false;
    }
    if (options.artifactItemLimit == 0)
        options.artifactItemLimit = static_cast<std::uint32_t>(manifest.FrameCount);
    if (options.artifactItemLimit >
        static_cast<std::uint32_t>(manifest.FrameCount))
    {
        error = "MAVFTP quick test item count exceeds the Current Artifact route length.";
        return false;
    }

    options.artifactSetId = manifest.ArtifactSetId;
    options.artifactSetDigest = manifest.ArtifactSetDigest;
    return true;
}

bool ValidateMAVFTPMissionHardwareTestOptions(
    const MAVFTPMissionHardwareTestOptions& options,
    std::string& error)
{
    error.clear();
    if (!options.enabled)
        return true;
    if (options.droneId.empty())
        error = "MAVFTP hardware test requires one explicit DroneID.";
    else if (options.confirmedDroneId != options.droneId)
        error = "MAVFTP hardware confirmation must exactly match the target DroneID.";
    else if (options.artifactSetId.empty() || !options.artifactSetDigest.IsSet)
        error = "MAVFTP hardware test requires an explicit Artifact Set ID and digest.";
    else if (options.routeSlotIndex < 0)
        error = "MAVFTP hardware test RouteSlotIndex must be non-negative.";
    else if (options.artifactItemLimit == 0)
        error = "MAVFTP hardware test Artifact item count must be positive.";
    else if (options.segmentItemCapacity == 0 ||
        options.segmentItemCapacity > 65535U)
        error = "MAVFTP hardware test segment capacity must be within 1..65535.";
    else if (options.requestTimeoutMs < 100 ||
        options.requestTimeoutMs > 60000)
        error = "MAVFTP hardware test request timeout must be within 100..60000 milliseconds.";
    else if (options.terminateTimeoutMs < 100 ||
        options.terminateTimeoutMs > 60000)
        error = "MAVFTP hardware test terminate timeout must be within 100..60000 milliseconds.";
    else if (options.requestRetryCount > 10U)
        error = "MAVFTP hardware test request retry count must be within 0..10.";
    else if (options.writeWindow == 0 || options.writeWindow > 16U)
        error = "MAVFTP hardware test write window must be within 1..16.";
    return error.empty();
}

std::string MAVFTPMissionHardwareTestReport::ToHumanSummary() const
{
    std::ostringstream output;
    output << "MAVFTP Hardware Validation"
        << " Success=" << YesNo(succeeded)
        << " DroneID=" << droneId
        << " Target=" << targetIp << ':' << targetPort
        << " RouteSlot=" << routeSlotIndex
        << " ArtifactRouteItems=" << artifactRouteItems
        << " WireMissionItems=" << wireMissionItems
        << " SegmentCapacityDomain=WireMissionItems"
        << " SegmentCapacity=" << segmentCapacity
        << " SegmentCount=" << segmentCount
        << " SegmentsCommitted=" << mavftpMetrics.segmentsCommitted
        << " MissionCount=";
    if (flightControllerMissionCount)
        output << *flightControllerMissionCount;
    else
        output << "Unavailable";
    output << " HomeSlotObserved=" << YesNo(homeSlotObserved)
        << " FirstArtifactItemObserved=" << YesNo(firstArtifactItemObserved)
        << " LastItemObserved=" << YesNo(lastItemObserved)
        << " RequestTimeoutMs=" << requestTimeoutMs
        << " TerminateTimeoutMs=" << terminateTimeoutMs
        << " RequestRetryCount=" << requestRetryCount
        << " ConfiguredWriteWindow=" << configuredWriteWindow
        << " CreateDurationMs=" << mavftpMetrics.createDurationMs
        << " WriteBytes=" << mavftpMetrics.writeBytes
        << " WriteBlocks=" << mavftpMetrics.writeBlocks
        << " MaxObservedOutstanding=" << mavftpMetrics.maxObservedOutstanding
        << " WriteBlocksSent=" << mavftpMetrics.writeBlocksSent
        << " WriteBlocksAcked=" << mavftpMetrics.writeBlocksAcked
        << " DuplicateAcks=" << mavftpMetrics.duplicateAcks
        << " StaleOrMismatchedAcks=" << mavftpMetrics.staleOrMismatchedAcks
        << " RetryCount=" << mavftpMetrics.retryCount
        << " RetriesTotal=" << mavftpMetrics.retryCount
        << " CreateRetries=" << mavftpMetrics.createRetries
        << " WriteRetries=" << mavftpMetrics.writeRetries
        << " TerminateRetries=" << mavftpMetrics.terminateRetries
        << " WriteDataDurationMs=" << mavftpMetrics.writeDataDurationMs
        << " TerminateDurationMs=" << mavftpMetrics.terminateDurationMs
        << " CommitDurationMs=" << mavftpMetrics.commitDurationMs
        << " TotalMavFtpDurationMs=" << mavftpMetrics.totalMavftpDurationMs
        << " AckRttMinMs=" << mavftpMetrics.ackRttMinMs
        << " AckRttAverageMs=" << mavftpMetrics.ackRttAverageMs
        << " AckRttP50Ms=" << mavftpMetrics.ackRttP50Ms
        << " AckRttP95Ms=" << mavftpMetrics.ackRttP95Ms
        << " AckRttP99Ms=" << mavftpMetrics.ackRttP99Ms
        << " MaxAckRttMs=" << mavftpMetrics.maxAckRttMs
        << " InterAckAverageMs=" << mavftpMetrics.interAckAverageMs
        << " HardwareValidationDurationMs=" << hardwareValidationDurationMs;
    if (!safeDiagnostic.empty())
        output << " ErrorReason=" << safeDiagnostic;
    return output.str();
}

MAVFTPMissionHardwareTest::MAVFTPMissionHardwareTest(
    ConsoleArtifactConsumerV2& artifactConsumer,
    DroneManager& droneManager,
    MAVFTPMissionHardwareTestOptions options)
    : artifactConsumer_(artifactConsumer)
    , droneManager_(droneManager)
    , options_(std::move(options))
{
    report_.droneId = options_.droneId;
    report_.routeSlotIndex = options_.routeSlotIndex;
    report_.segmentCapacity = options_.segmentItemCapacity;
    report_.requestTimeoutMs = options_.requestTimeoutMs;
    report_.terminateTimeoutMs = options_.terminateTimeoutMs;
    report_.requestRetryCount = options_.requestRetryCount;
    report_.configuredWriteWindow = options_.writeWindow;
}

MAVFTPMissionHardwareTest::~MAVFTPMissionHardwareTest() noexcept
{
    if (session_) session_->SetMissionVerificationMessageSink(nullptr);
    preparationWorker_.Shutdown();
}

bool MAVFTPMissionHardwareTest::Start(
    const std::chrono::steady_clock::time_point now,
    std::string& error)
{
    if (state_ != State::Idle)
    {
        error = "MAVFTP hardware validation already started.";
        return false;
    }
    if (!ValidateMAVFTPMissionHardwareTestOptions(options_, error))
        return false;

    validationStartedAt_ = now;

    ExpectedRouteArtifactV2Identity expected;
    expected.artifactSetId = options_.artifactSetId;
    expected.artifactSetDigest = options_.artifactSetDigest;
    if (artifactConsumer_.RequestValidation(
        std::move(expected), std::chrono::minutes(5)) == 0)
    {
        error = "MAVFTP hardware validation could not start Artifact validation.";
        return false;
    }

    discoveryDeadline_ = now + std::chrono::seconds(30);
    overallDeadline_ = now + std::chrono::minutes(10);
    state_ = State::WaitingSources;
    Log(options_.quickTest ? "MAVFTP QUICK TEST" : "MAVFTP HARDWARE TEST MODE");
    if (options_.quickTest)
    {
        Log("Drone: " + options_.droneId);
        Log("Artifact: " + options_.artifactSetId);
        Log("RouteSlot: " + std::to_string(options_.routeSlotIndex));
        Log("ArtifactItems: " + std::to_string(options_.artifactItemLimit));
    }
    Log("Transport=MAVFTP TargetDroneID=" + options_.droneId +
        " RouteSlotIndex=" + std::to_string(options_.routeSlotIndex) +
        " RequestedArtifactItems=" +
        std::to_string(options_.artifactItemLimit) +
        " SegmentCapacityDomain=WireMissionItems" +
        " SegmentCapacity=" +
        std::to_string(options_.segmentItemCapacity) +
        " RequestTimeoutMs=" + std::to_string(options_.requestTimeoutMs) +
        " TerminateTimeoutMs=" + std::to_string(options_.terminateTimeoutMs) +
        " RequestRetryCount=" + std::to_string(options_.requestRetryCount) +
        " WriteWindow=" + std::to_string(options_.writeWindow));
    return true;
}

void MAVFTPMissionHardwareTest::PollArtifact()
{
    if (artifactSnapshot_) return;
    ConsoleArtifactV2ValidationResult validation;
    if (!artifactConsumer_.PollCompleted(validation)) return;
    if (!validation.success || !validation.snapshot)
    {
        Finish(false, validation.diagnostic.safeDetail.empty()
            ? "Artifact validation failed."
            : validation.diagnostic.safeDetail);
        return;
    }
    artifactSnapshot_ = std::move(validation.snapshot);
    const auto route = std::find_if(
        artifactSnapshot_->routes.begin(), artifactSnapshot_->routes.end(),
        [this](const AcceptedRouteArtifactV2RouteRecord& candidate)
        {
            return candidate.routeSlotIndex == options_.routeSlotIndex;
        });
    if (route == artifactSnapshot_->routes.end())
    {
        Finish(false, "Requested RouteSlotIndex is absent from the accepted Artifact.");
        return;
    }
    if (artifactSnapshot_->frameCount <= 0 ||
        options_.artifactItemLimit >
            static_cast<std::uint32_t>(artifactSnapshot_->frameCount))
    {
        Finish(false, "Requested Artifact item count exceeds the accepted Route length.");
        return;
    }

    MissionPreparationRequest request;
    request.uploadJobId = "MAVFTP-Hardware-Validation";
    request.generation = 1;
    request.artifactSnapshot = artifactSnapshot_;
    request.target.droneId = options_.droneId;
    request.target.routeSlotIndex = route->routeSlotIndex;
    request.target.relativePath = route->relativePath;
    request.target.fileByteSize = route->fileByteSize;
    request.target.missionItemCount =
        static_cast<std::uint32_t>(artifactSnapshot_->frameCount);
    request.target.missionPayloadByteSize =
        static_cast<std::uint64_t>(request.target.missionItemCount) *
        dronegcs::route_mission::MissionItemRecordBytesV1;
    request.target.routeArtifactDigest = route->routeArtifactDigest;
    if (!preparationWorker_.Submit(std::move(request)))
    {
        Finish(false, "Mission preparation worker rejected the hardware-test Route.");
        return;
    }
    state_ = State::PreparingMission;
    Log("Artifact accepted; Mission preparation started.");
}

void MAVFTPMissionHardwareTest::PollPreparation()
{
    if (compiledSnapshot_) return;
    MissionPreparationResult prepared;
    if (!preparationWorker_.Poll(prepared)) return;
    if (!prepared.Succeeded())
    {
        Finish(false, prepared.safeDiagnostic.empty()
            ? "Mission preparation failed."
            : prepared.safeDiagnostic);
        return;
    }
    if (prepared.mission.size() < options_.artifactItemLimit)
    {
        Finish(false, "Prepared Mission contains fewer items than explicitly requested.");
        return;
    }
    prepared.mission.resize(options_.artifactItemLimit);

    dronegcs::mission_ftp::CompileConfig compileConfig;
    compileConfig.segmentCapacity = options_.segmentItemCapacity;
    compileConfig.verificationPolicy =
        dronegcs::mission_ftp::VerificationPolicy::ProductionEndpoints;
    const auto compiled = dronegcs::mission_ftp::MissionFtpCompiler::Compile(
        prepared.mission, compileConfig);
    if (!compiled.IsValid())
    {
        Finish(false, compiled.validation.safeDiagnostic);
        return;
    }
    compiledSnapshot_ = compiled.snapshot;
    report_.artifactRouteItems = options_.artifactItemLimit;
    report_.wireMissionItems = compiledSnapshot_->GetTotalWireMissionItems();
    report_.segmentCount =
        compiledSnapshot_->GetSegmentDescriptors().size();
    state_ = State::WaitingSources;
    Log("Compile complete ArtifactItems=" +
        std::to_string(report_.artifactRouteItems) +
        " WireMissionItems=" + std::to_string(report_.wireMissionItems) +
        " SegmentCapacityDomain=WireMissionItems" +
        " SegmentCapacity=" + std::to_string(report_.segmentCapacity) +
        " SegmentCount=" + std::to_string(report_.segmentCount));
    for (const auto& segment : compiledSnapshot_->GetSegmentDescriptors())
    {
        Log("SegmentPlan Index=" + std::to_string(segment.segmentIndex) +
            " Options=" + std::string(segment.options ==
                dronegcs::route_mission::FirstContainerOptionsV1
                    ? "CLEAR" : "NO_CLEAR") +
            " StartIndex=" + std::to_string(segment.startIndex) +
            " ItemCount=" + std::to_string(segment.itemCount) +
            " EncodedBytes=" + std::to_string(segment.encodedBytes));
    }
    if (options_.quickTest)
        Log("WireItems: " + std::to_string(report_.wireMissionItems));
}

void MAVFTPMissionHardwareTest::RefreshTargetSession(
    const std::chrono::steady_clock::time_point now)
{
    if (session_ && session_->GetFlightData().online) return;
    session_ = droneManager_.FindDrone(options_.droneId);
    if (!session_ || !session_->GetFlightData().online)
    {
        session_ = nullptr;
        if (now >= discoveryDeadline_)
            Finish(false, "Target Drone was not online before the discovery deadline.");
    }
}

void MAVFTPMissionHardwareTest::TryBeginUpload(
    const std::chrono::steady_clock::time_point now)
{
    if (state_ != State::WaitingSources || !compiledSnapshot_ || !session_)
        return;
    const DroneSessionConfig& sessionConfig = session_->GetConfig();
    auto* client = session_->GetMavFtpClient();
    if (!client)
    {
        Finish(false, "Target DroneSession has no endpoint-bound MAVFTP client.");
        return;
    }
    if (sessionConfig.mavFtpRequestTimeoutMs != options_.requestTimeoutMs ||
        sessionConfig.mavFtpTerminateTimeoutMs != options_.terminateTimeoutMs ||
        sessionConfig.mavFtpRequestRetryCount != options_.requestRetryCount ||
        sessionConfig.mavFtpWriteWindow != options_.writeWindow)
    {
        Finish(false, "Target DroneSession MAVFTP request configuration differs from the hardware-test snapshot.");
        return;
    }
    report_.targetIp = sessionConfig.remoteIp;
    report_.targetPort = static_cast<std::uint16_t>(sessionConfig.remotePort);
    Log("MAVFTP HARDWARE TEST MODE TargetDroneID=" + options_.droneId +
        " TargetIP=" + report_.targetIp +
        " TargetPort=" + std::to_string(report_.targetPort) +
        " MissionItems=" + std::to_string(report_.artifactRouteItems));

    dronegcs::mission_ftp::MAVFTPMissionTransportConfig config;
    config.compileConfig.segmentCapacity = options_.segmentItemCapacity;
    config.compileConfig.verificationPolicy =
        dronegcs::mission_ftp::VerificationPolicy::ProductionEndpoints;
    writer_ = std::make_unique<
        dronegcs::mission_ftp::MAVFTPMissionTransport>(*client, config);
    MissionTransportIdentity identity;
    identity.uploadJobId = "MAVFTP-Hardware-Validation";
    identity.droneId = options_.droneId;
    identity.routeSlotIndex = options_.routeSlotIndex;
    identity.generation = 1;
    std::string diagnostic;
    if (!writer_->PrepareCompiled(
            std::move(identity), compiledSnapshot_, diagnostic))
    {
        Finish(false, diagnostic.empty()
            ? writer_->GetSafeDiagnostic()
            : diagnostic);
        return;
    }
    if (!writer_->Upload(now))
    {
        LogWriterLifecycle();
        if (!segmentFailureLogged_)
        {
            LogSegmentEvent("SegmentFailed", 0);
            segmentFailureLogged_ = true;
        }
        Finish(false, writer_->GetSafeDiagnostic().empty()
            ? "MAVFTP upload could not start."
            : writer_->GetSafeDiagnostic());
        return;
    }
    state_ = State::Uploading;
    LogSegmentEvent("SegmentStart", 0);
    loggedSegmentStart_ = 0;
}

void MAVFTPMissionHardwareTest::Tick(
    const std::chrono::steady_clock::time_point now)
{
    if (state_ == State::Idle || state_ == State::Complete) return;
    if (now >= overallDeadline_)
    {
        artifactConsumer_.CancelValidation();
        preparationWorker_.CancelActive();
        if (writer_ && !writer_->IsTerminal()) writer_->Cancel(now);
        CaptureWriterMetrics();
        Finish(false, "MAVFTP hardware validation exceeded its overall deadline.");
        return;
    }

    RefreshTargetSession(now);
    if (state_ == State::Complete) return;
    if (state_ == State::WaitingSources) PollArtifact();
    if (state_ == State::PreparingMission) PollPreparation();
    TryBeginUpload(now);

    if (state_ == State::Uploading && writer_)
    {
        if (!session_ || !session_->GetFlightData().online)
        {
            writer_->Cancel(now);
            CaptureWriterMetrics();
            Finish(false, "Target Drone went offline during MAVFTP upload.");
            return;
        }
        writer_->Tick(now);
        LogWriterLifecycle();
        if (writer_->IsTerminal())
        {
            CaptureWriterMetrics();
            if (!writer_->Verify())
            {
                Finish(false, writer_->GetSafeDiagnostic());
                return;
            }
            if (!BeginMissionCount(now))
                Finish(false, "MISSION_REQUEST_LIST could not be sent after commit.");
        }
    }
    else if (state_ == State::WaitingMissionCount)
    {
        TickMissionCount(now);
    }
    else if (state_ == State::WaitingSample)
    {
        TickSample(now);
    }
}

bool MAVFTPMissionHardwareTest::BeginMissionCount(
    const std::chrono::steady_clock::time_point now)
{
    if (!session_ || !session_->SendMissionRequestList()) return false;
    state_ = State::WaitingMissionCount;
    responseRetries_ = 0;
    responseDeadline_ = now + ResponseTimeout;
    Log("MAVFTP commit confirmed; requesting Flight Controller Mission Count.");
    return true;
}

void MAVFTPMissionHardwareTest::TickMissionCount(
    const std::chrono::steady_clock::time_point now)
{
    if (!session_) return;
    if (const auto count = session_->ConsumeMissionCount())
    {
        report_.flightControllerMissionCount = count;
        if (*count != report_.wireMissionItems)
        {
            Finish(false, "Flight Controller Mission Count differs from Artifact plus Home Slot.");
            return;
        }
        sampleSequences_ = {
            0,
            1,
            static_cast<std::uint16_t>(report_.wireMissionItems - 1) };
        std::sort(sampleSequences_.begin(), sampleSequences_.end());
        sampleSequences_.erase(
            std::unique(sampleSequences_.begin(), sampleSequences_.end()),
            sampleSequences_.end());
        sampleIndex_ = 0;
        session_->SetMissionVerificationMessageSink(this);
        if (!BeginCurrentSample(now))
            Finish(false, "First Mission sample request could not be sent.");
        return;
    }
    if (now < responseDeadline_) return;
    if (responseRetries_ >= MaximumResponseRetries ||
        !session_->SendMissionRequestList())
    {
        Finish(false, "Flight Controller Mission Count response timed out.");
        return;
    }
    ++responseRetries_;
    responseDeadline_ = now + ResponseTimeout;
}

bool MAVFTPMissionHardwareTest::BeginCurrentSample(
    const std::chrono::steady_clock::time_point now)
{
    if (!session_ || sampleIndex_ >= sampleSequences_.size()) return false;
    std::size_t wireBytes = 0;
    if (!session_->SendMissionRequestInt(
        sampleSequences_[sampleIndex_], wireBytes))
        return false;
    receivedSampleSequence_.reset();
    state_ = State::WaitingSample;
    responseRetries_ = 0;
    responseDeadline_ = now + ResponseTimeout;
    return true;
}

void MAVFTPMissionHardwareTest::TickSample(
    const std::chrono::steady_clock::time_point now)
{
    if (receivedSampleSequence_)
    {
        const std::uint16_t sequence = *receivedSampleSequence_;
        if (sequence == 0) report_.homeSlotObserved = true;
        if (sequence == 1) report_.firstArtifactItemObserved = true;
        if (sequence == report_.wireMissionItems - 1)
            report_.lastItemObserved = true;
        ++sampleIndex_;
        if (sampleIndex_ == sampleSequences_.size())
        {
            Finish(true, {});
            return;
        }
        if (!BeginCurrentSample(now))
            Finish(false, "Next Mission sample request could not be sent.");
        return;
    }
    if (now < responseDeadline_) return;
    if (responseRetries_ >= MaximumResponseRetries || !session_)
    {
        Finish(false, "Flight Controller Mission sample response timed out.");
        return;
    }
    std::size_t wireBytes = 0;
    if (!session_->SendMissionRequestInt(
        sampleSequences_[sampleIndex_], wireBytes))
    {
        Finish(false, "Mission sample retry could not be sent.");
        return;
    }
    ++responseRetries_;
    responseDeadline_ = now + ResponseTimeout;
}

bool MAVFTPMissionHardwareTest::HandleMissionMessage(
    const mavlink_message_t& message,
    const MissionVerificationPeerIdentity& peer,
    const std::chrono::steady_clock::time_point)
{
    if (state_ != State::WaitingSample || !session_ ||
        !(peer == session_->GetMissionVerificationPeerIdentity()) ||
        message.msgid != MAVLINK_MSG_ID_MISSION_ITEM_INT)
        return false;
    mavlink_mission_item_int_t item{};
    mavlink_msg_mission_item_int_decode(&message, &item);
    if (item.target_system != 255 ||
        item.target_component != MAV_COMP_ID_MISSIONPLANNER ||
        item.mission_type != MAV_MISSION_TYPE_MISSION ||
        sampleIndex_ >= sampleSequences_.size() ||
        item.seq != sampleSequences_[sampleIndex_])
        return false;
    receivedSampleSequence_ = item.seq;
    return true;
}

void MAVFTPMissionHardwareTest::CaptureWriterMetrics()
{
    if (writer_) report_.mavftpMetrics = writer_->GetMavFtpMetrics();
}

void MAVFTPMissionHardwareTest::LogSegmentEvent(
    const char* const event,
    const std::size_t segmentIndex) const
{
    if (!compiledSnapshot_ ||
        segmentIndex >= compiledSnapshot_->GetSegmentDescriptors().size())
        return;
    const auto& segment =
        compiledSnapshot_->GetSegmentDescriptors()[segmentIndex];
    const auto metrics = writer_
        ? writer_->GetMavFtpMetrics()
        : dronegcs::mission_ftp::MAVFTPMissionTransportMetrics{};
    const std::uint64_t segmentsPlanned = metrics.segmentsPlanned > 0
        ? metrics.segmentsPlanned : report_.segmentCount;
    Log(std::string(event) +
        " Index=" + std::to_string(segment.segmentIndex) +
        " StartIndex=" + std::to_string(segment.startIndex) +
        " ItemCount=" + std::to_string(segment.itemCount) +
        " SegmentsCommitted=" +
        std::to_string(metrics.segmentsCommitted) +
        " SegmentsPlanned=" + std::to_string(segmentsPlanned));
}

void MAVFTPMissionHardwareTest::LogWriterLifecycle()
{
    if (!writer_ || !compiledSnapshot_) return;
    const auto metrics = writer_->GetMavFtpMetrics();
    const std::size_t activeIndex =
        static_cast<std::size_t>(metrics.activeSegmentIndex);

    if (std::string(writer_->GetTerminalPhaseName()) == "TerminatePending" &&
        loggedTerminatePending_ != activeIndex)
    {
        LogSegmentEvent("SegmentTerminatePending", activeIndex);
        loggedTerminatePending_ = activeIndex;
    }

    while (loggedSegmentsCommitted_ < metrics.segmentsCommitted)
    {
        const std::size_t committedIndex =
            static_cast<std::size_t>(loggedSegmentsCommitted_);
        if (loggedTerminatePending_ != committedIndex)
        {
            LogSegmentEvent("SegmentTerminatePending", committedIndex);
            loggedTerminatePending_ = committedIndex;
        }
        LogSegmentEvent("SegmentCommitted",
            committedIndex);
        ++loggedSegmentsCommitted_;
    }

    if (!writer_->IsTerminal() &&
        metrics.segmentsCommitted > 0 &&
        loggedSegmentStart_ != activeIndex)
    {
        LogSegmentEvent("SegmentStart", activeIndex);
        loggedSegmentStart_ = activeIndex;
    }

    if (writer_->GetStatus() == MissionTransportStatus::Failed &&
        !segmentFailureLogged_)
    {
        if (std::string(writer_->GetTerminalPhaseName()) == "TerminateFailed" &&
            loggedTerminatePending_ != activeIndex)
        {
            LogSegmentEvent("SegmentTerminatePending", activeIndex);
            loggedTerminatePending_ = activeIndex;
        }
        LogSegmentEvent("SegmentFailed", activeIndex);
        segmentFailureLogged_ = true;
    }
}

void MAVFTPMissionHardwareTest::Cancel(
    const std::chrono::steady_clock::time_point now)
{
    artifactConsumer_.CancelValidation();
    preparationWorker_.CancelActive();
    if (writer_ && !writer_->IsTerminal()) writer_->Cancel(now);
    CaptureWriterMetrics();
    Finish(false, "MAVFTP hardware validation cancelled.");
}

void MAVFTPMissionHardwareTest::Finish(
    const bool success,
    std::string diagnostic)
{
    if (state_ == State::Complete) return;
    if (session_) session_->SetMissionVerificationMessageSink(nullptr);
    report_.completed = true;
    report_.succeeded = success;
    report_.safeDiagnostic = std::move(diagnostic);
    if (validationStartedAt_ != std::chrono::steady_clock::time_point{})
    {
        report_.hardwareValidationDurationMs =
            static_cast<std::uint64_t>(std::chrono::duration_cast<
                std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - validationStartedAt_).count());
    }
    state_ = State::Complete;
    Log(report_.ToHumanSummary());
}

void MAVFTPMissionHardwareTest::Log(const std::string& line) const
{
    ConsoleUI::AddLog("[MAVFTP-HW] " + line);
}
