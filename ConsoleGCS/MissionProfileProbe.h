#pragma once

#include "DroneManager.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

enum class MissionProfileComparison : std::uint8_t
{
    ExactMatch,
    CompatibleWithDifferences,
    Incompatible,
    Unknown
};

const char* MissionProfileComparisonName(MissionProfileComparison value);

struct MissionProfileProbeOptions
{
    bool enabled = false;
    bool allowWriteProbe = false;
    std::string droneId;
};

struct MissionProfileProbeReport
{
    std::string droneId;
    std::string peerIdentitySafeSummary;
    std::string firmwareSummary = "Unknown";
    bool ftpTransportSupported = false;
    std::string missionPath;
    std::size_t missionPathBytes = 0;
    std::size_t missionPathEncodedSize = 0;
    bool missionPathNullTerminated = false;
    bool missionPathOpen = false;
    std::uint32_t remoteFileSize = 0;
    std::size_t headerSizeObserved = 0;
    std::string headerBytesSafeSummary;
    std::optional<std::uint16_t> magicObserved;
    std::optional<std::uint16_t> dataTypeObserved;
    std::optional<std::uint16_t> optionsObserved;
    std::optional<std::uint16_t> startObserved;
    std::optional<std::uint16_t> numItemsObserved;
    std::optional<std::size_t> recordSizeObserved;
    bool remoteCrcSupported = false;
    std::optional<std::uint32_t> remoteCrc;
    std::uint32_t localCrc = 0;
    MissionProfileComparison currentCodecComparison =
        MissionProfileComparison::Unknown;
    bool writeProbeAttempted = false;
    bool writeAccepted = false;
    std::optional<std::uint16_t> missionCountBeforeWrite;
    std::optional<std::uint16_t> missionCountAfterWrite;
    bool restoreAttempted = false;
    bool restoreVerified = false;
    std::optional<std::uint16_t> missionCountAfterRestore;
    bool completed = false;
    bool succeeded = false;
    std::string safeDiagnostic;

    std::string ToMachineJson() const;
    std::string ToHumanSummary() const;
    std::string OpenFileRequestSafeSummary() const;
};

class MissionProfileProbe final
{
public:
    static constexpr const char* MissionPath = "@MISSION/mission.dat";

    MissionProfileProbe(DroneSession& session, MissionProfileProbeOptions options);
    ~MissionProfileProbe() noexcept;

    bool Start(std::string& error);
    void Tick(std::chrono::steady_clock::time_point now);
    void Cancel();
    bool IsComplete() const { return report_.completed; }
    const MissionProfileProbeReport& GetReport() const { return report_; }

private:
    enum class State : std::uint8_t
    {
        Idle,
        Resetting,
        ReadingBackup,
        CalculatingBackupCrc,
        WaitingBackupMissionCount,
        WritingTiny,
        ReadingTiny,
        CalculatingTinyCrc,
        WaitingTinyMissionCount,
        RestoringBackup,
        ReadingRestored,
        CalculatingRestoredCrc,
        WaitingRestoredMissionCount,
        Complete
    };

    void HandleFtpResult(dronegcs::mavftp::OperationResult result);
    bool StartRead(State state, std::string& error);
    bool StartCrc(State state, std::string& error);
    bool StartWrite(State state, const std::vector<std::uint8_t>& bytes,
        std::string& error);
    void AnalyzeBackup();
    bool BuildTinyMission(std::vector<std::uint8_t>& output,
        std::string& error) const;
    void BeginWriteProbe();
    void BeginRestore(const std::string& reason);
    void BeginMissionCount(State state);
    void TickMissionCount(std::chrono::steady_clock::time_point now);
    void FinishReadOnly(bool success, std::string diagnostic);
    void FinishAfterRestore(std::string diagnostic);

    DroneSession& session_;
    dronegcs::mavftp::Client* client_ = nullptr;
    MissionProfileProbeOptions options_;
    MissionProfileProbeReport report_;
    State state_ = State::Idle;
    std::vector<std::uint8_t> backupBytes_;
    std::vector<std::uint8_t> tinyBytes_;
    std::vector<std::uint8_t> restoredBytes_;
    std::uint32_t backupCrc_ = 0;
    std::uint32_t tinyCrc_ = 0;
    std::optional<std::size_t> inferredBackupCount_;
    bool tinyReadbackVerified_ = false;
    std::string deferredDiagnostic_;
    std::chrono::steady_clock::time_point missionCountDeadline_{};
    std::uint32_t missionCountRetries_ = 0;
};

std::unique_ptr<MissionProfileProbe> CreateMissionProfileProbe(
    DroneManager& manager,
    const MissionProfileProbeOptions& options,
    std::string& error);
