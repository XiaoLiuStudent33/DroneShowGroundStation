#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MissionProfileProbe.h"

#include "MavFtpProtocol.h"
#include "RouteMissionCodec.h"

#include <array>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace
{
    namespace mission = dronegcs::route_mission;
    namespace ftp = dronegcs::mavftp;

    std::uint16_t ReadU16(const std::uint8_t* bytes)
    {
        return static_cast<std::uint16_t>(bytes[0]) |
            static_cast<std::uint16_t>(bytes[1] << 8);
    }

    std::string HexPrefix(const std::vector<std::uint8_t>& bytes)
    {
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        const std::size_t count = (std::min)(
            bytes.size(), mission::LegacyMissionFtpContainerBytesV1);
        for (std::size_t index = 0; index < count; ++index)
        {
            if (index != 0) stream << '-';
            stream << std::setw(2) << static_cast<unsigned>(bytes[index]);
        }
        return stream.str();
    }

    std::string JsonEscape(const std::string& value)
    {
        std::string output;
        output.reserve(value.size());
        for (const char character : value)
        {
            if (character == '"' || character == '\\') output.push_back('\\');
            if (static_cast<unsigned char>(character) >= 0x20)
                output.push_back(character);
        }
        return output;
    }

    template <typename T>
    std::string OptionalNumber(const std::optional<T>& value)
    {
        return value ? std::to_string(*value) : "null";
    }
}

const char* MissionProfileComparisonName(const MissionProfileComparison value)
{
    switch (value)
    {
    case MissionProfileComparison::ExactMatch: return "ExactMatch";
    case MissionProfileComparison::CompatibleWithDifferences:
        return "CompatibleWithDifferences";
    case MissionProfileComparison::Incompatible: return "Incompatible";
    case MissionProfileComparison::Unknown: return "Unknown";
    }
    return "Unknown";
}

std::string MissionProfileProbeReport::ToMachineJson() const
{
    std::ostringstream stream;
    stream << '{'
        << "\"DroneID\":\"" << JsonEscape(droneId) << "\"," 
        << "\"PeerIdentitySafeSummary\":\""
        << JsonEscape(peerIdentitySafeSummary) << "\"," 
        << "\"FirmwareSummary\":\"" << JsonEscape(firmwareSummary) << "\"," 
        << "\"FTPTransportSupported\":" << (ftpTransportSupported ? "true" : "false") << ','
        << "\"MissionPath\":\"" << JsonEscape(missionPath) << "\"," 
        << "\"MissionPathBytes\":" << missionPathBytes << ','
        << "\"MissionPathEncodedSize\":" << missionPathEncodedSize << ','
        << "\"MissionPathNullTerminated\":"
        << (missionPathNullTerminated ? "true" : "false") << ','
        << "\"MissionPathOpen\":" << (missionPathOpen ? "true" : "false") << ','
        << "\"RemoteFileSize\":" << remoteFileSize << ','
        << "\"HeaderSizeObserved\":" << headerSizeObserved << ','
        << "\"HeaderBytesSafeSummary\":\"" << headerBytesSafeSummary << "\"," 
        << "\"MagicObserved\":" << OptionalNumber(magicObserved) << ','
        << "\"DataTypeObserved\":" << OptionalNumber(dataTypeObserved) << ','
        << "\"OptionsObserved\":" << OptionalNumber(optionsObserved) << ','
        << "\"StartObserved\":" << OptionalNumber(startObserved) << ','
        << "\"NumItemsObserved\":" << OptionalNumber(numItemsObserved) << ','
        << "\"RecordSizeObserved\":" << OptionalNumber(recordSizeObserved) << ','
        << "\"RemoteCRCSupported\":" << (remoteCrcSupported ? "true" : "false") << ','
        << "\"RemoteCRC\":" << OptionalNumber(remoteCrc) << ','
        << "\"LocalCRC\":" << localCrc << ','
        << "\"CurrentCodecComparison\":\""
        << MissionProfileComparisonName(currentCodecComparison) << "\"," 
        << "\"WriteProbeAttempted\":" << (writeProbeAttempted ? "true" : "false") << ','
        << "\"WriteAccepted\":" << (writeAccepted ? "true" : "false") << ','
        << "\"MissionCountBeforeWrite\":" << OptionalNumber(missionCountBeforeWrite) << ','
        << "\"MissionCountAfterWrite\":" << OptionalNumber(missionCountAfterWrite) << ','
        << "\"RestoreAttempted\":" << (restoreAttempted ? "true" : "false") << ','
        << "\"RestoreVerified\":" << (restoreVerified ? "true" : "false") << ','
        << "\"MissionCountAfterRestore\":" << OptionalNumber(missionCountAfterRestore) << ','
        << "\"SafeDiagnostic\":\"" << JsonEscape(safeDiagnostic) << "\"}"
        ;
    return stream.str();
}

std::string MissionProfileProbeReport::OpenFileRequestSafeSummary() const
{
    std::ostringstream stream;
    stream << "OpenFileRO request: Path=" << missionPath
        << " PathBytes=" << missionPathBytes
        << " EncodedSize=" << missionPathEncodedSize
        << " NullTerminated=" << (missionPathNullTerminated ? "yes" : "no");
    return stream.str();
}

std::string MissionProfileProbeReport::ToHumanSummary() const
{
    std::ostringstream stream;
    stream << "MissionProfileProbe Drone=" << droneId
        << " Open=" << (missionPathOpen ? "yes" : "no")
        << " Bytes=" << remoteFileSize
        << " LocalCRC=" << localCrc
        << " Profile=" << MissionProfileComparisonName(currentCodecComparison)
        << " Write=" << (writeProbeAttempted
            ? (writeAccepted ? "accepted" : "failed") : "not-attempted")
        << " Restore=" << (restoreAttempted
            ? (restoreVerified ? "verified" : "FAILED") : "not-attempted")
        << " Diagnostic=" << safeDiagnostic;
    return stream.str();
}

MissionProfileProbe::MissionProfileProbe(
    DroneSession& session,
    MissionProfileProbeOptions options)
    : session_(session), options_(std::move(options))
{
    report_.droneId = session_.GetDroneId();
    report_.missionPath = MissionPath;
    report_.missionPathBytes = std::char_traits<char>::length(MissionPath);
    std::vector<std::uint8_t> pathData;
    ftp::Error pathError;
    if (ftp::MakePathData(MissionPath, pathData, pathError))
    {
        report_.missionPathEncodedSize = pathData.size();
        report_.missionPathNullTerminated = !pathData.empty() &&
            pathData.back() == 0;
    }
}

MissionProfileProbe::~MissionProfileProbe() noexcept
{
    if (!report_.completed && client_) client_->Cancel();
}

bool MissionProfileProbe::Start(std::string& error)
{
    if (state_ != State::Idle)
    {
        error = "Mission profile probe has already started.";
        return false;
    }
    if (options_.droneId.empty() || options_.droneId != session_.GetDroneId())
    {
        error = "Mission profile probe requires one explicit matching DroneID.";
        return false;
    }
    const FlightData flight = session_.GetFlightData();
    const DroneSessionConfig& sessionConfig = session_.GetConfig();
    report_.peerIdentitySafeSummary =
        "sysid=" + std::to_string(sessionConfig.discoveredSystemId) +
        " compid=" + std::to_string(sessionConfig.discoveredComponentId) +
        " endpoint=bound";
    if (!flight.online || session_.GetState() != DroneSessionState::Online)
    {
        error = "Mission profile probe requires an online DroneSession.";
        return false;
    }
    if (flight.armed)
    {
        error = "Mission profile probe is blocked while the vehicle is armed.";
        return false;
    }
    client_ = session_.GetMavFtpClient();
    if (!client_)
    {
        error = "Mission profile probe MAVFTP service is not enabled.";
        return false;
    }
    if (client_->IsBusy())
    {
        error = "Another DroneGCS MAVFTP transaction is active for this Drone.";
        return false;
    }

    if (flight.autopilotVersionValid)
    {
        report_.firmwareSummary =
            "flight_sw_version=" + std::to_string(flight.flightSoftwareVersion) +
            " board_version=" + std::to_string(flight.boardVersion) +
            " vendor_id=" + std::to_string(flight.vendorId) +
            " product_id=" + std::to_string(flight.productId);
        report_.ftpTransportSupported =
            (flight.autopilotCapabilities & (1ULL << 5)) != 0;
    }
    else
    {
        report_.firmwareSummary = "Unknown (AUTOPILOT_VERSION not cached)";
    }

    if (!client_->StartResetSessions(error)) return false;
    state_ = State::Resetting;
    return true;
}

void MissionProfileProbe::Tick(const std::chrono::steady_clock::time_point now)
{
    if (report_.completed || !client_) return;
    if (state_ == State::WaitingBackupMissionCount ||
        state_ == State::WaitingTinyMissionCount ||
        state_ == State::WaitingRestoredMissionCount)
    {
        TickMissionCount(now);
        return;
    }
    if (const auto result = client_->ConsumeResult())
        HandleFtpResult(*result);
}

bool MissionProfileProbe::StartRead(const State state, std::string& error)
{
    if (!client_->StartReadFile(MissionPath, error)) return false;
    state_ = state;
    return true;
}

bool MissionProfileProbe::StartCrc(const State state, std::string& error)
{
    if (!client_->StartCalculateCrc32(MissionPath, error)) return false;
    state_ = state;
    return true;
}

bool MissionProfileProbe::StartWrite(
    const State state,
    const std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    if (!client_->StartWriteFile(MissionPath, bytes, error)) return false;
    state_ = state;
    return true;
}

void MissionProfileProbe::HandleFtpResult(ftp::OperationResult result)
{
    std::string error;
    switch (state_)
    {
    case State::Resetting:
        if (!result.success)
        {
            FinishReadOnly(false, "ResetSessions failed: " + result.safeDiagnostic);
            return;
        }
        // A correlated ResetSessions ACK is direct runtime evidence that the
        // endpoint supports the transport, even when AUTOPILOT_VERSION was
        // not cached before this development probe started.
        report_.ftpTransportSupported = true;
        if (!StartRead(State::ReadingBackup, error))
            FinishReadOnly(false, error);
        return;
    case State::ReadingBackup:
        if (!result.success)
        {
            FinishReadOnly(false, "Mission path read failed: " + result.safeDiagnostic);
            return;
        }
        report_.missionPathOpen = true;
        report_.remoteFileSize = result.remoteFileSize;
        backupBytes_ = std::move(result.bytes);
        backupCrc_ = ftp::CalculateCrc32(
            backupBytes_.data(), backupBytes_.size());
        report_.localCrc = backupCrc_;
        AnalyzeBackup();
        if (!StartCrc(State::CalculatingBackupCrc, error))
            FinishReadOnly(false, error);
        return;
    case State::CalculatingBackupCrc:
        if (result.success && result.remoteCrc32)
        {
            report_.remoteCrcSupported = true;
            report_.remoteCrc = result.remoteCrc32;
            if (*result.remoteCrc32 != backupCrc_)
            {
                FinishReadOnly(false, "Remote/local mission CRC mismatch.");
                return;
            }
        }
        else if (!result.unsupported)
        {
            FinishReadOnly(false, "Remote CRC request failed: " + result.safeDiagnostic);
            return;
        }
        if (options_.allowWriteProbe)
            BeginMissionCount(State::WaitingBackupMissionCount);
        else FinishReadOnly(true, result.unsupported
            ? "Read-only probe complete; remote CRC unsupported."
            : "Read-only probe complete.");
        return;
    case State::WritingTiny:
        report_.writeProbeAttempted = true;
        if (!result.success)
        {
            BeginRestore("Tiny mission write/close failed: " + result.safeDiagnostic);
            return;
        }
        report_.writeAccepted = true;
        if (!StartRead(State::ReadingTiny, error))
            BeginRestore(error);
        return;
    case State::ReadingTiny:
        if (!result.success || result.bytes != tinyBytes_)
        {
            BeginRestore(result.success
                ? "Tiny mission readback bytes differ."
                : "Tiny mission readback failed: " + result.safeDiagnostic);
            return;
        }
        tinyReadbackVerified_ = true;
        if (!StartCrc(State::CalculatingTinyCrc, error)) BeginRestore(error);
        return;
    case State::CalculatingTinyCrc:
        if (result.success && result.remoteCrc32 &&
            *result.remoteCrc32 != tinyCrc_)
        {
            BeginRestore("Tiny mission remote/local CRC mismatch.");
            return;
        }
        if (!result.success && !result.unsupported)
        {
            BeginRestore("Tiny mission CRC request failed.");
            return;
        }
        BeginMissionCount(State::WaitingTinyMissionCount);
        return;
    case State::RestoringBackup:
        if (!result.success)
        {
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: backup restore write/close failed.");
            return;
        }
        if (!StartRead(State::ReadingRestored, error))
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: " + error);
        return;
    case State::ReadingRestored:
        if (!result.success)
        {
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore readback failed.");
            return;
        }
        restoredBytes_ = std::move(result.bytes);
        if (!StartCrc(State::CalculatingRestoredCrc, error))
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: " + error);
        return;
    case State::CalculatingRestoredCrc:
        if (!result.success && !result.unsupported)
        {
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore CRC request failed.");
            return;
        }
        if (result.success && result.remoteCrc32 &&
            *result.remoteCrc32 != backupCrc_)
        {
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore remote CRC mismatch.");
            return;
        }
        BeginMissionCount(State::WaitingRestoredMissionCount);
        return;
    default:
        return;
    }
}

void MissionProfileProbe::AnalyzeBackup()
{
    report_.headerSizeObserved = (std::min)(backupBytes_.size(),
        mission::LegacyMissionFtpContainerBytesV1);
    report_.headerBytesSafeSummary = HexPrefix(backupBytes_);
    if (backupBytes_.size() < mission::LegacyMissionFtpContainerBytesV1)
    {
        report_.currentCodecComparison = backupBytes_.empty()
            ? MissionProfileComparison::Unknown
            : MissionProfileComparison::Incompatible;
        return;
    }
    report_.magicObserved = ReadU16(backupBytes_.data());
    report_.dataTypeObserved = ReadU16(backupBytes_.data() + 2);
    report_.optionsObserved = ReadU16(backupBytes_.data() + 4);
    report_.startObserved = ReadU16(backupBytes_.data() + 6);
    report_.numItemsObserved = ReadU16(backupBytes_.data() + 8);
    const std::size_t bodyBytes = backupBytes_.size() -
        mission::LegacyMissionFtpContainerBytesV1;
    if (bodyBytes % mission::MissionItemRecordBytesV1 != 0)
    {
        report_.currentCodecComparison = MissionProfileComparison::Incompatible;
        return;
    }
    report_.recordSizeObserved = mission::MissionItemRecordBytesV1;
    inferredBackupCount_ = bodyBytes / mission::MissionItemRecordBytesV1;
    const bool exact = *report_.magicObserved == mission::LegacyMissionFtpMagicV1 &&
        *report_.dataTypeObserved == mission::LegacyMissionTypeV1 &&
        *report_.optionsObserved == mission::FirstContainerOptionsV1 &&
        *report_.startObserved == 0 &&
        *report_.numItemsObserved == *inferredBackupCount_;
    report_.currentCodecComparison = exact
        ? MissionProfileComparison::ExactMatch
        : MissionProfileComparison::CompatibleWithDifferences;
}

bool MissionProfileProbe::BuildTinyMission(
    std::vector<std::uint8_t>& output,
    std::string& error) const
{
    std::array<std::uint8_t, mission::LegacyMissionFtpContainerBytesV1> header{};
    mission::Error missionError;
    if (!mission::EncodeLegacyMissionFtpContainerV1(
        0, 1, mission::FirstContainerOptionsV1, header, missionError))
    {
        error = missionError.Detail;
        return false;
    }
    dronegcs::wpl110::Row row;
    const FlightData flight = session_.GetFlightData();
    if (flight.gpsValid)
    {
        row.LatitudeE7 = static_cast<std::int32_t>(flight.latitude * 10000000.0);
        row.LongitudeE7 = static_cast<std::int32_t>(flight.longitude * 10000000.0);
    }
    row.AltitudeE7 = 0;
    std::array<std::uint8_t, mission::MissionItemRecordBytesV1> record{};
    if (!mission::EncodeMissionItemRecordV1(row, record, missionError))
    {
        error = missionError.Detail;
        return false;
    }
    output.assign(header.begin(), header.end());
    output.insert(output.end(), record.begin(), record.end());
    error.clear();
    return true;
}

void MissionProfileProbe::BeginWriteProbe()
{
    report_.writeProbeAttempted = true;
    if (backupBytes_.empty() || backupBytes_.size() != report_.remoteFileSize ||
        !inferredBackupCount_ || !report_.missionCountBeforeWrite ||
        client_->IsBusy() ||
        session_.GetFlightData().armed || !session_.GetFlightData().online)
    {
        FinishReadOnly(false, "Write probe safety gate rejected incomplete backup/session state.");
        return;
    }
    std::string error;
    if (!BuildTinyMission(tinyBytes_, error))
    {
        FinishReadOnly(false, error);
        return;
    }
    tinyCrc_ = ftp::CalculateCrc32(tinyBytes_.data(), tinyBytes_.size());
    if (!StartWrite(State::WritingTiny, tinyBytes_, error))
        FinishReadOnly(false, error);
}

void MissionProfileProbe::BeginRestore(const std::string& reason)
{
    if (report_.restoreAttempted)
    {
        FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: repeated restore blocked.");
        return;
    }
    report_.restoreAttempted = true;
    deferredDiagnostic_ = reason;
    std::string error;
    if (!StartWrite(State::RestoringBackup, backupBytes_, error))
        FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore could not start: " + error);
}

void MissionProfileProbe::BeginMissionCount(const State state)
{
    if (!session_.SendMissionRequestList())
    {
        if (state == State::WaitingBackupMissionCount)
            FinishReadOnly(false,
                "Pre-write MISSION_REQUEST_LIST send failed; no write performed.");
        else if (state == State::WaitingTinyMissionCount)
            BeginRestore("MISSION_REQUEST_LIST send failed after tiny write.");
        else
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore MISSION_REQUEST_LIST send failed.");
        return;
    }
    state_ = state;
    missionCountRetries_ = 0;
    missionCountDeadline_ = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(750);
}

void MissionProfileProbe::TickMissionCount(
    const std::chrono::steady_clock::time_point now)
{
    if (const auto count = session_.ConsumeMissionCount())
    {
        if (state_ == State::WaitingBackupMissionCount)
        {
            report_.missionCountBeforeWrite = count;
            BeginWriteProbe();
        }
        else if (state_ == State::WaitingTinyMissionCount)
        {
            report_.missionCountAfterWrite = count;
            const bool countMatches = *count == 1;
            BeginRestore(countMatches && tinyReadbackVerified_
                ? "Tiny mission verified; restoring backup."
                : "Tiny mission MISSION_COUNT/readback mismatch; restoring backup.");
        }
        else
        {
            report_.missionCountAfterRestore = count;
            const bool bytesMatch = restoredBytes_ == backupBytes_;
            const bool crcMatches = ftp::CalculateCrc32(
                restoredBytes_.data(), restoredBytes_.size()) == backupCrc_;
            const bool countMatches = report_.missionCountBeforeWrite &&
                *count == *report_.missionCountBeforeWrite;
            report_.restoreVerified = bytesMatch && crcMatches && countMatches;
            FinishAfterRestore(report_.restoreVerified
                ? deferredDiagnostic_ + " Backup restored and verified."
                : "HIGH-VISIBILITY DEVELOPMENT ERROR: restored mission verification mismatch.");
        }
        return;
    }
    if (now < missionCountDeadline_) return;
    if (missionCountRetries_ >= 3)
    {
        if (state_ == State::WaitingBackupMissionCount)
            FinishReadOnly(false,
                "Pre-write MISSION_COUNT timeout; no write performed.");
        else if (state_ == State::WaitingTinyMissionCount)
            BeginRestore("MISSION_COUNT timeout after tiny write.");
        else
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore MISSION_COUNT timeout.");
        return;
    }
    ++missionCountRetries_;
    if (!session_.SendMissionRequestList())
    {
        if (state_ == State::WaitingBackupMissionCount)
            FinishReadOnly(false,
                "Pre-write MISSION_COUNT retry send failed; no write performed.");
        else if (state_ == State::WaitingTinyMissionCount)
            BeginRestore("MISSION_COUNT retry send failed.");
        else
            FinishAfterRestore("HIGH-VISIBILITY DEVELOPMENT ERROR: restore MISSION_COUNT retry failed.");
        return;
    }
    missionCountDeadline_ = now + std::chrono::milliseconds(750);
}

void MissionProfileProbe::FinishReadOnly(
    const bool success,
    std::string diagnostic)
{
    report_.completed = true;
    report_.succeeded = success;
    report_.safeDiagnostic = std::move(diagnostic);
    state_ = State::Complete;
}

void MissionProfileProbe::FinishAfterRestore(std::string diagnostic)
{
    report_.completed = true;
    report_.succeeded = report_.restoreVerified;
    report_.safeDiagnostic = std::move(diagnostic);
    state_ = State::Complete;
}

void MissionProfileProbe::Cancel()
{
    if (client_) client_->Cancel();
    FinishReadOnly(false, "Mission profile probe cancelled.");
}

std::unique_ptr<MissionProfileProbe> CreateMissionProfileProbe(
    DroneManager& manager,
    const MissionProfileProbeOptions& options,
    std::string& error)
{
    if (!options.enabled || options.droneId.empty())
    {
        error = "Mission profile probe requires --mission-profile-probe <DroneID>.";
        return nullptr;
    }
    DroneSession* session = manager.FindDrone(options.droneId);
    if (!session)
    {
        error = "The explicitly requested DroneID is not present.";
        return nullptr;
    }
    auto probe = std::make_unique<MissionProfileProbe>(*session, options);
    if (!probe->Start(error)) return nullptr;
    return probe;
}
