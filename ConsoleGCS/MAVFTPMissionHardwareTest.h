#pragma once

#include "AppConfig.h"
#include "ConsoleArtifactConsumerV2.h"
#include "DroneManager.h"
#include "MAVFTPMissionTransport.h"
#include "MissionPreparationWorker.h"
#include "MissionUploadTransaction.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct MAVFTPMissionHardwareTestOptions
{
    bool enabled = false;
    bool quickTest = false;
    MissionTransportMode transportMode = MissionTransportMode::MissionProtocol;
    std::string droneId;
    std::string confirmedDroneId;
    std::string artifactSetId;
    dronegcs::route_artifact::Digest256 artifactSetDigest;
    std::int32_t routeSlotIndex = -1;
    std::uint32_t artifactItemLimit = 0;
    std::uint32_t segmentItemCapacity = 65535U;
    int requestTimeoutMs = 750;
    int terminateTimeoutMs = 2500;
    std::uint32_t requestRetryCount = 3;
    std::uint32_t writeWindow = 1;
};

/** Applies the single CLI > INI > code-default transport configuration merge. */
bool ResolveMAVFTPMissionTransportOptions(
    const AppConfig& appConfig,
    bool segmentItemsProvidedByCli,
    bool terminateTimeoutProvidedByCli,
    bool writeWindowProvidedByCli,
    MAVFTPMissionHardwareTestOptions& options,
    std::string& error);

bool ValidateMAVFTPMissionHardwareTestOptions(
    const MAVFTPMissionHardwareTestOptions& options,
    std::string& error);

/**
 * Resolves only the quick-test bootstrap fields from canonical Current
 * metadata. ConsoleArtifactConsumerV2 still performs the authoritative full
 * Current/READY/routes validation before compilation or network writes.
 */
bool ResolveMAVFTPQuickTestOptionsFromCurrent(
    const std::string& artifactWorkspaceRoot,
    MAVFTPMissionHardwareTestOptions& options,
    std::string& error);

struct MAVFTPMissionHardwareTestReport
{
    bool completed = false;
    bool succeeded = false;
    std::string droneId;
    std::string targetIp;
    std::uint16_t targetPort = 0;
    std::int32_t routeSlotIndex = -1;
    std::uint64_t artifactRouteItems = 0;
    std::uint64_t wireMissionItems = 0;
    std::uint32_t segmentCapacity = 0;
    std::uint64_t segmentCount = 0;
    int requestTimeoutMs = 750;
    int terminateTimeoutMs = 2500;
    std::uint32_t requestRetryCount = 3;
    std::uint32_t configuredWriteWindow = 1;
    std::optional<std::uint16_t> flightControllerMissionCount;
    bool homeSlotObserved = false;
    bool firstArtifactItemObserved = false;
    bool lastItemObserved = false;
    dronegcs::mission_ftp::MAVFTPMissionTransportMetrics mavftpMetrics;
    std::uint64_t hardwareValidationDurationMs = 0;
    std::string safeDiagnostic;

    std::string ToHumanSummary() const;
};

/**
 * Explicit, single-target hardware validation path. It never participates in
 * RouteUploadManager selection and is constructed only by the opt-in CLI mode.
 */
class MAVFTPMissionHardwareTest final : public IMissionProtocolMessageSink
{
public:
    MAVFTPMissionHardwareTest(
        ConsoleArtifactConsumerV2& artifactConsumer,
        DroneManager& droneManager,
        MAVFTPMissionHardwareTestOptions options);
    ~MAVFTPMissionHardwareTest() noexcept;

    bool Start(std::chrono::steady_clock::time_point now, std::string& error);
    void Tick(std::chrono::steady_clock::time_point now);
    void Cancel(std::chrono::steady_clock::time_point now);

    bool IsComplete() const { return report_.completed; }
    const MAVFTPMissionHardwareTestReport& GetReport() const { return report_; }

    bool HandleMissionMessage(
        const mavlink_message_t& message,
        const MissionProtocolPeerIdentity& peer,
        std::chrono::steady_clock::time_point now) override;

private:
    enum class State : std::uint8_t
    {
        Idle,
        WaitingSources,
        PreparingMission,
        Uploading,
        WaitingMissionCount,
        WaitingSample,
        Complete
    };

    void PollArtifact();
    void PollPreparation();
    void RefreshTargetSession(std::chrono::steady_clock::time_point now);
    void TryBeginUpload(std::chrono::steady_clock::time_point now);
    bool BeginMissionCount(std::chrono::steady_clock::time_point now);
    bool BeginCurrentSample(std::chrono::steady_clock::time_point now);
    void TickMissionCount(std::chrono::steady_clock::time_point now);
    void TickSample(std::chrono::steady_clock::time_point now);
    void CaptureWriterMetrics();
    void LogSegmentEvent(const char* event, std::size_t segmentIndex) const;
    void LogWriterLifecycle();
    void Finish(bool success, std::string diagnostic);
    void Log(const std::string& line) const;

    ConsoleArtifactConsumerV2& artifactConsumer_;
    DroneManager& droneManager_;
    MAVFTPMissionHardwareTestOptions options_;
    MAVFTPMissionHardwareTestReport report_;
    State state_ = State::Idle;
    std::shared_ptr<const AcceptedRouteArtifactV2> artifactSnapshot_;
    MissionPreparationWorker preparationWorker_;
    std::shared_ptr<const dronegcs::mission_ftp::CompiledMissionSnapshot>
        compiledSnapshot_;
    DroneSession* session_ = nullptr;
    std::unique_ptr<dronegcs::mission_ftp::MAVFTPMissionTransport> writer_;
    std::vector<std::uint16_t> sampleSequences_;
    std::size_t sampleIndex_ = 0;
    std::optional<std::uint16_t> receivedSampleSequence_;
    std::chrono::steady_clock::time_point discoveryDeadline_{};
    std::chrono::steady_clock::time_point validationStartedAt_{};
    std::chrono::steady_clock::time_point overallDeadline_{};
    std::chrono::steady_clock::time_point responseDeadline_{};
    std::uint32_t responseRetries_ = 0;
    std::uint64_t loggedSegmentsCommitted_ = 0;
    std::size_t loggedSegmentStart_ = static_cast<std::size_t>(-1);
    std::size_t loggedTerminatePending_ = static_cast<std::size_t>(-1);
    bool segmentFailureLogged_ = false;
};
