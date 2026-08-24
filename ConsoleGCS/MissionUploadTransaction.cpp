#include "MissionUploadTransaction.h"

#include "ArduPilotMissionWireAdapter.h"

#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace
{
    bool SameFloatBits(const float left, const float right)
    {
        return dronegcs::route_mission::Binary32ToBits(left) ==
            dronegcs::route_mission::Binary32ToBits(right);
    }

    bool CanonicalizeArduPilotMissionAltitudeForReadback(
        const float meters,
        float& outCanonicalMeters)
    {
        // ArduPilot stores Location::alt as signed integer centimeters:
        // packet.z * 100.0f is converted to int32 (truncate toward zero),
        // then read back as storedCentimeters * 0.01f.
        const float scaledCentimeters = meters * 100.0f;
        constexpr float MinimumInt32AsFloat = -2147483648.0f;
        constexpr float MaximumInt32ExclusiveAsFloat = 2147483648.0f;
        if (!(scaledCentimeters >= MinimumInt32AsFloat) ||
            !(scaledCentimeters < MaximumInt32ExclusiveAsFloat))
        {
            return false;
        }

        const std::int32_t storedCentimeters =
            static_cast<std::int32_t>(scaledCentimeters);
        outCanonicalMeters =
            static_cast<float>(storedCentimeters) * 0.01f;
        return true;
    }

    bool IsArduPilotAltitudeEquivalent(
        const float expectedMeters,
        const float actualMeters)
    {
        float canonicalExpectedMeters = 0.0f;
        return CanonicalizeArduPilotMissionAltitudeForReadback(
                expectedMeters, canonicalExpectedMeters) &&
            SameFloatBits(canonicalExpectedMeters, actualMeters);
    }

    bool IsArduPilotFrameEquivalent(
        const std::uint8_t expectedFrame,
        const std::uint8_t actualFrame)
    {
        if (expectedFrame == actualFrame) return true;

        constexpr std::uint8_t GlobalRelativeAlt =
            static_cast<std::uint8_t>(MAV_FRAME_GLOBAL_RELATIVE_ALT);
        constexpr std::uint8_t GlobalRelativeAltInt =
            static_cast<std::uint8_t>(MAV_FRAME_GLOBAL_RELATIVE_ALT_INT);
        constexpr std::uint8_t ArduCopterV203CanonicalReadbackFrame = 4;
        const auto isUnorderedPair = [expectedFrame, actualFrame](
            const std::uint8_t left, const std::uint8_t right)
        {
            return (expectedFrame == left && actualFrame == right) ||
                (expectedFrame == right && actualFrame == left);
        };

        return isUnorderedPair(
                GlobalRelativeAlt, ArduCopterV203CanonicalReadbackFrame) ||
            isUnorderedPair(GlobalRelativeAlt, GlobalRelativeAltInt);
    }

    std::uint64_t ElapsedMilliseconds(
        const std::chrono::steady_clock::time_point start,
        const std::chrono::steady_clock::time_point end)
    {
        if (start == std::chrono::steady_clock::time_point{} || end <= start)
            return 0;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end - start).count());
    }

    std::string FloatText(const float value)
    {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<float>::max_digits10)
            << value;
        return output.str();
    }

    std::string DoubleText(const double value)
    {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << std::setprecision(std::numeric_limits<double>::max_digits10)
            << value;
        return output.str();
    }

    std::string FloatBitsText(const float value)
    {
        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << "0x" << std::uppercase << std::hex << std::setfill('0')
            << std::setw(8)
            << dronegcs::route_mission::Binary32ToBits(value);
        return output.str();
    }

    void AppendExpectedItem(
        std::ostringstream& output,
        const dronegcs::route_mission::MissionItemIntV1& item)
    {
        output << "Expected{"
            << "seq=" << item.Sequence
            << " frame=" << static_cast<unsigned>(item.Frame)
            << " command=" << item.Command
            << " current=" << static_cast<unsigned>(item.Current)
            << " autocontinue=" << static_cast<unsigned>(item.AutoContinue)
            << " param1=" << FloatText(item.Param1)
            << " param1Bits=" << FloatBitsText(item.Param1)
            << " param2=" << FloatText(item.Param2)
            << " param2Bits=" << FloatBitsText(item.Param2)
            << " param3=" << FloatText(item.Param3)
            << " param3Bits=" << FloatBitsText(item.Param3)
            << " param4=" << FloatText(item.Param4)
            << " param4Bits=" << FloatBitsText(item.Param4)
            << " x=" << item.X
            << " y=" << item.Y
            << " z=" << FloatText(item.Z)
            << " zBits=" << FloatBitsText(item.Z)
            << " mission_type=" << static_cast<unsigned>(item.MissionType)
            << '}';
    }

    void AppendActualItem(
        std::ostringstream& output,
        const mavlink_mission_item_int_t& item)
    {
        output << "Actual{"
            << "seq=" << item.seq
            << " frame=" << static_cast<unsigned>(item.frame)
            << " command=" << item.command
            << " current=" << static_cast<unsigned>(item.current)
            << " autocontinue=" << static_cast<unsigned>(item.autocontinue)
            << " param1=" << FloatText(item.param1)
            << " param1Bits=" << FloatBitsText(item.param1)
            << " param2=" << FloatText(item.param2)
            << " param2Bits=" << FloatBitsText(item.param2)
            << " param3=" << FloatText(item.param3)
            << " param3Bits=" << FloatBitsText(item.param3)
            << " param4=" << FloatText(item.param4)
            << " param4Bits=" << FloatBitsText(item.param4)
            << " x=" << item.x
            << " y=" << item.y
            << " z=" << FloatText(item.z)
            << " zBits=" << FloatBitsText(item.z)
            << " mission_type=" << static_cast<unsigned>(item.mission_type)
            << '}';
    }

    std::string BuildVerificationMismatchDiagnostic(
        const bool first,
        const std::size_t artifactRouteIndex,
        const std::uint16_t wireMissionSequence,
        const mavlink_mission_item_int_t& actual,
        const dronegcs::route_mission::MissionItemIntV1& expected)
    {
        std::ostringstream fieldMask;
        fieldMask.imbue(std::locale::classic());
        bool hasField = false;
        const auto appendField = [&fieldMask, &hasField](
            const bool differs, const char* const name)
        {
            if (!differs) return;
            if (hasField) fieldMask << ',';
            fieldMask << name;
            hasField = true;
        };
        appendField(actual.seq != expected.Sequence, "Seq");
        appendField(!IsArduPilotFrameEquivalent(
            expected.Frame, actual.frame), "Frame");
        appendField(actual.command != expected.Command, "Command");
        appendField(actual.autocontinue != expected.AutoContinue, "AutoContinue");
        appendField(!SameFloatBits(actual.param1, expected.Param1), "Param1");
        appendField(!SameFloatBits(actual.param2, expected.Param2), "Param2");
        appendField(!SameFloatBits(actual.param3, expected.Param3), "Param3");
        appendField(!SameFloatBits(actual.param4, expected.Param4), "Param4");
        appendField(actual.x != expected.X, "X");
        appendField(actual.y != expected.Y, "Y");
        appendField(!IsArduPilotAltitudeEquivalent(
            expected.Z, actual.z), "Z");

        std::ostringstream output;
        output.imbue(std::locale::classic());
        output << (first ? "FirstItemMismatch" : "LastItemMismatch")
            << " ReplyType=MISSION_ITEM_INT"
            << " ArtifactRouteIndex=" << artifactRouteIndex
            << " WireMissionSeq=" << wireMissionSequence
            << " FieldMask=" << fieldMask.str()
            << " ExpectedZ=" << FloatText(expected.Z)
            << " ActualZ=" << FloatText(actual.z)
            << " DeltaZ=" << DoubleText(
                static_cast<double>(actual.z) -
                static_cast<double>(expected.Z))
            << ' ';
        AppendExpectedItem(output, expected);
        output << ' ';
        AppendActualItem(output, actual);
        std::string result = output.str();
        if (result.size() > MissionVerificationMismatchDiagnosticMaxBytes)
            result.resize(MissionVerificationMismatchDiagnosticMaxBytes);
        return result;
    }

}

const char* MissionUploadPhaseName(const MissionUploadPhase phase)
{
	switch (phase)
	{
	case MissionUploadPhase::Idle: return "Idle";
	case MissionUploadPhase::VerifyingCount: return "VerifyingCount";
	case MissionUploadPhase::VerifyingHome: return "VerifyingHome";
	case MissionUploadPhase::VerifyingFirst: return "VerifyingFirst";
	case MissionUploadPhase::VerifyingLast: return "VerifyingLast";
	case MissionUploadPhase::Succeeded: return "Succeeded";
	case MissionUploadPhase::Failed: return "Failed";
	case MissionUploadPhase::Cancelled: return "Cancelled";
	default: return "Unknown";
	}
}

MissionUploadTransaction::MissionUploadTransaction(
    MissionUploadTransactionIdentity identity,
    std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
    IMissionVerificationTransport& transport,
    MissionUploadTransactionConfig config)
    : identity_(std::move(identity))
    , transport_(transport)
    , config_(config)
{
    metrics_.artifactRouteItems = mission.size();
    wireMissionValid_ = dronegcs::ardupilot_mission::BuildWireMission(
        mission, mission_);
    metrics_.wireMissionItems = mission_.size();
}

bool MissionUploadTransaction::IsTerminal() const
{
    return phase_ == MissionUploadPhase::Succeeded ||
        phase_ == MissionUploadPhase::Failed ||
        phase_ == MissionUploadPhase::Cancelled;
}

void MissionUploadTransaction::FinalizeTiming(
    const std::chrono::steady_clock::time_point now)
{
    if (verificationStartedAt_ == std::chrono::steady_clock::time_point{}) return;
    metrics_.verificationDurationMs = ElapsedMilliseconds(
        verificationStartedAt_, now);
}

void MissionUploadTransaction::Fail(
    const char* diagnostic,
    const std::chrono::steady_clock::time_point now)
{
    if (IsTerminal()) return;
	terminalSourcePhase_ = phase_;
    FinalizeTiming(now);
    phase_ = MissionUploadPhase::Failed;
    safeDiagnostic_ = diagnostic ? diagnostic : "Mission verification failed.";
}

void MissionUploadTransaction::ResetDeadline(
    const std::chrono::steady_clock::time_point now)
{
    deadline_ = now + config_.responseTimeout;
}

void MissionUploadTransaction::ResetRetryBudget()
{
    retriesInWait_ = 0;
}

bool MissionUploadTransaction::RecordSend(
    const bool sent,
    const std::size_t wireBytes,
    const std::chrono::steady_clock::time_point now)
{
    if (!sent)
    {
        Fail("Mission transport send failed.", now);
        return false;
    }
    metrics_.wireBytesSent += wireBytes;
    ++metrics_.missionMessagesSent;
    ResetDeadline(now);
    return true;
}

bool MissionUploadTransaction::SendVerificationList(
    const std::chrono::steady_clock::time_point now)
{
    std::size_t wireBytes = 0;
    phase_ = MissionUploadPhase::VerifyingCount;
    const bool sent = transport_.SendMissionRequestList(wireBytes);
    return RecordSend(sent, wireBytes, now);
}

bool MissionUploadTransaction::SendVerificationItem(
    const std::uint16_t sequence,
    const MissionUploadPhase phase,
    const std::chrono::steady_clock::time_point now)
{
    std::size_t wireBytes = 0;
    phase_ = phase;
    lastRequestedSequence_ = sequence;
    const bool sent = transport_.SendMissionRequestInt(sequence, wireBytes);
    return RecordSend(sent, wireBytes, now);
}

bool MissionUploadTransaction::BeginVerification(
    const std::chrono::steady_clock::time_point now,
    const bool observeReservedHome)
{
    if (phase_ != MissionUploadPhase::Idle || !wireMissionValid_ ||
        mission_.empty() ||
        mission_.size() > dronegcs::ardupilot_mission::MaximumWireMissionItems ||
        config_.responseTimeout.count() <= 0)
    {
        Fail("Prepared Mission is invalid for production verification.", now);
        return false;
    }
    verificationStartedAt_ = now;
    observeReservedHome_ = observeReservedHome;
    ResetRetryBudget();
    return SendVerificationList(now);
}

bool MissionUploadTransaction::MatchesTarget(
    const std::uint8_t targetSystem,
    const std::uint8_t targetComponent,
    const std::uint8_t missionType) const
{
    return targetSystem == 255 &&
        targetComponent == MAV_COMP_ID_MISSIONPLANNER &&
        missionType == MAV_MISSION_TYPE_MISSION;
}

bool MissionUploadTransaction::VerifyItem(
    const mavlink_mission_item_int_t& received,
    const dronegcs::route_mission::MissionItemIntV1& expected) const
{
    return received.seq == expected.Sequence &&
        IsArduPilotFrameEquivalent(expected.Frame, received.frame) &&
        received.command == expected.Command &&
        SameFloatBits(received.param1, expected.Param1) &&
        SameFloatBits(received.param2, expected.Param2) &&
        SameFloatBits(received.param3, expected.Param3) &&
        SameFloatBits(received.param4, expected.Param4) &&
        received.x == expected.X && received.y == expected.Y &&
        IsArduPilotAltitudeEquivalent(expected.Z, received.z) &&
        received.autocontinue == expected.AutoContinue;
}

bool MissionUploadTransaction::HandleMissionMessage(
    const mavlink_message_t& message,
    const MissionVerificationPeerIdentity& peer,
    const std::chrono::steady_clock::time_point now)
{
    if (IsTerminal() || phase_ == MissionUploadPhase::Idle ||
        !(peer == identity_.peer) || message.sysid != identity_.peer.systemId ||
        message.compid != identity_.peer.componentId)
        return false;

    if (message.msgid == MAVLINK_MSG_ID_MISSION_COUNT &&
        phase_ == MissionUploadPhase::VerifyingCount)
    {
        mavlink_mission_count_t count{};
        mavlink_msg_mission_count_decode(&message, &count);
        if (!MatchesTarget(count.target_system, count.target_component,
            count.mission_type))
            return false;
        if (count.count != mission_.size())
        {
            Fail("Mission verification count differs from the prepared route.", now);
            return true;
        }
        ResetRetryBudget();
        if (observeReservedHome_)
        {
            SendVerificationItem(
                dronegcs::ardupilot_mission::ReservedHomeWireSequence,
                MissionUploadPhase::VerifyingHome, now);
        }
        else
        {
            SendVerificationItem(
                dronegcs::ardupilot_mission::FirstRouteWireSequence,
                MissionUploadPhase::VerifyingFirst, now);
        }
        return true;
    }

    if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT &&
        (phase_ == MissionUploadPhase::VerifyingHome ||
         phase_ == MissionUploadPhase::VerifyingFirst ||
         phase_ == MissionUploadPhase::VerifyingLast))
    {
        mavlink_mission_item_int_t item{};
        mavlink_msg_mission_item_int_decode(&message, &item);
        if (!MatchesTarget(item.target_system, item.target_component,
            item.mission_type))
            return false;

        // Home is dynamic on ArduPilot. Production MAVFTP verification requires
        // it to be observable, but deliberately does not compare its values.
        if (phase_ == MissionUploadPhase::VerifyingHome)
        {
            if (item.seq !=
                dronegcs::ardupilot_mission::ReservedHomeWireSequence)
                return false;
            ResetRetryBudget();
            SendVerificationItem(
                dronegcs::ardupilot_mission::FirstRouteWireSequence,
                MissionUploadPhase::VerifyingFirst, now);
            return true;
        }
        if (item.seq == dronegcs::ardupilot_mission::ReservedHomeWireSequence)
            return true;

        const std::uint16_t expectedSequence = phase_ ==
            MissionUploadPhase::VerifyingFirst
            ? dronegcs::ardupilot_mission::FirstRouteWireSequence
            : static_cast<std::uint16_t>(mission_.size() - 1);
        if (item.seq != expectedSequence ||
            !VerifyItem(item, mission_[expectedSequence]))
        {
            const bool first = phase_ == MissionUploadPhase::VerifyingFirst;
            const std::size_t artifactRouteIndex =
                dronegcs::ardupilot_mission::ArtifactRouteIndexForWireSequence(
                    expectedSequence);
            verificationMismatchDiagnostic_ = BuildVerificationMismatchDiagnostic(
                first, artifactRouteIndex, expectedSequence,
                item, mission_[expectedSequence]);
            Fail(first
                ? "First Mission item verification failed."
                : "Last Mission item verification failed.", now);
            return true;
        }
        ResetRetryBudget();
        const std::uint16_t lastRouteSequence =
            static_cast<std::uint16_t>(mission_.size() - 1);
        if (phase_ == MissionUploadPhase::VerifyingFirst &&
            lastRouteSequence !=
                dronegcs::ardupilot_mission::FirstRouteWireSequence)
        {
            SendVerificationItem(
                lastRouteSequence,
                MissionUploadPhase::VerifyingLast, now);
        }
        else
        {
			terminalSourcePhase_ = phase_;
            FinalizeTiming(now);
            phase_ = MissionUploadPhase::Succeeded;
            safeDiagnostic_.clear();
        }
        return true;
    }

    return false;
}

void MissionUploadTransaction::Tick(
    const std::chrono::steady_clock::time_point now)
{
    if (IsTerminal() || phase_ == MissionUploadPhase::Idle || now < deadline_)
        return;
    if (retriesInWait_ >= config_.maxRetries)
    {
        Fail("Mission verification response timed out after bounded retries.", now);
        return;
    }
    ++retriesInWait_;
    ++metrics_.retryCount;
    switch (phase_)
    {
    case MissionUploadPhase::VerifyingCount:
        SendVerificationList(now);
        break;
    case MissionUploadPhase::VerifyingHome:
        SendVerificationItem(
            dronegcs::ardupilot_mission::ReservedHomeWireSequence,
            MissionUploadPhase::VerifyingHome, now);
        break;
    case MissionUploadPhase::VerifyingFirst:
        SendVerificationItem(
            dronegcs::ardupilot_mission::FirstRouteWireSequence,
            MissionUploadPhase::VerifyingFirst, now);
        break;
    case MissionUploadPhase::VerifyingLast:
        SendVerificationItem(static_cast<std::uint16_t>(mission_.size() - 1),
            MissionUploadPhase::VerifyingLast, now);
        break;
    default:
        break;
    }
}

void MissionUploadTransaction::Cancel(
    const std::chrono::steady_clock::time_point now)
{
	if (!IsTerminal())
	{
		terminalSourcePhase_ = phase_;
		FinalizeTiming(now);
		phase_ = MissionUploadPhase::Cancelled;
        safeDiagnostic_ = "Mission verification was cancelled.";
    }
}

bool MissionUploadTransaction::ConsumeProgressChanged()
{
    const bool result = progressChanged_;
    progressChanged_ = false;
    return result;
}
