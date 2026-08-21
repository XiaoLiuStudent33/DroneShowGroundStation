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

    const char* AckDiagnostic(const std::uint8_t result)
    {
        switch (result)
        {
        case MAV_MISSION_ERROR: return "Vehicle reported a Mission protocol error.";
        case MAV_MISSION_UNSUPPORTED_FRAME: return "Vehicle rejected the Mission frame.";
        case MAV_MISSION_UNSUPPORTED: return "Vehicle does not support the Mission item.";
        case MAV_MISSION_NO_SPACE: return "Vehicle has insufficient Mission storage.";
        case MAV_MISSION_INVALID: return "Vehicle rejected invalid Mission data.";
        case MAV_MISSION_INVALID_PARAM1:
        case MAV_MISSION_INVALID_PARAM2:
        case MAV_MISSION_INVALID_PARAM3:
        case MAV_MISSION_INVALID_PARAM4:
        case MAV_MISSION_INVALID_PARAM5_X:
        case MAV_MISSION_INVALID_PARAM6_Y:
        case MAV_MISSION_INVALID_PARAM7:
            return "Vehicle rejected a Mission item parameter.";
        case MAV_MISSION_DENIED: return "Vehicle denied the Mission upload.";
        default: return "Vehicle rejected the Mission upload.";
        }
    }
}

const char* MissionUploadPhaseName(const MissionUploadPhase phase)
{
	switch (phase)
	{
	case MissionUploadPhase::Idle: return "Idle";
	case MissionUploadPhase::AwaitingRequest: return "AwaitingRequest";
	case MissionUploadPhase::AwaitingAck: return "AwaitingAck";
	case MissionUploadPhase::VerifyingCount: return "VerifyingCount";
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
    IMissionProtocolTransport& transport,
    MissionUploadTransactionConfig config)
    : identity_(std::move(identity))
    , transport_(transport)
    , config_(config)
{
    metrics_.artifactRouteItems = mission.size();
    wireMissionValid_ = dronegcs::ardupilot_mission::BuildWireMission(
        mission, mission_);
    metrics_.wireMissionItems = mission_.size();
    served_.assign(mission_.size(), false);
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
    if (uploadStartedAt_ == std::chrono::steady_clock::time_point{}) return;
    if (verificationStartedAt_ == std::chrono::steady_clock::time_point{})
    {
        metrics_.uploadDurationMs = ElapsedMilliseconds(uploadStartedAt_, now);
        return;
    }
    metrics_.uploadDurationMs = ElapsedMilliseconds(
        uploadStartedAt_, verificationStartedAt_);
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
    safeDiagnostic_ = diagnostic ? diagnostic : "Mission upload failed.";
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

bool MissionUploadTransaction::SendCount(
    const std::chrono::steady_clock::time_point now)
{
    std::size_t wireBytes = 0;
    const bool sent = transport_.SendMissionCount(
        static_cast<std::uint16_t>(mission_.size()), wireBytes);
    return RecordSend(sent, wireBytes, now);
}

bool MissionUploadTransaction::SendItem(
    const std::uint16_t sequence,
    const std::chrono::steady_clock::time_point now,
    const bool countDuplicateRequest)
{
    if (sequence >= mission_.size())
    {
        Fail("Vehicle requested a Mission sequence outside the prepared route.", now);
        return false;
    }
    std::size_t wireBytes = 0;
    const bool sent = transport_.SendMissionItemInt(mission_[sequence], wireBytes);
    if (!RecordSend(sent, wireBytes, now))
        return false;
    lastRequestedSequence_ = sequence;
    if (!served_[sequence])
    {
        served_[sequence] = true;
        ++uniqueServed_;
		metrics_.uniqueItemsServed = uniqueServed_;
        if (sequence != dronegcs::ardupilot_mission::ReservedHomeWireSequence)
        {
            metrics_.logicalBytesCompleted +=
                dronegcs::route_mission::MissionItemRecordBytesV1;
            progressChanged_ = true;
        }
    }
    else if (countDuplicateRequest)
    {
        ++metrics_.duplicateRequests;
    }
    phase_ = uniqueServed_ == mission_.size()
        ? MissionUploadPhase::AwaitingAck
        : MissionUploadPhase::AwaitingRequest;
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

bool MissionUploadTransaction::Start(
    const std::chrono::steady_clock::time_point now)
{
    if (phase_ != MissionUploadPhase::Idle || !wireMissionValid_ ||
        mission_.empty() ||
        mission_.size() > dronegcs::ardupilot_mission::MaximumWireMissionItems ||
        config_.responseTimeout.count() <= 0)
    {
        Fail("Prepared Mission is invalid for standard Mission upload.", now);
        return false;
    }
    uploadStartedAt_ = now;
    phase_ = MissionUploadPhase::AwaitingRequest;
    ResetRetryBudget();
    return SendCount(now);
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
    const MissionProtocolPeerIdentity& peer,
    const std::chrono::steady_clock::time_point now)
{
    if (IsTerminal() || phase_ == MissionUploadPhase::Idle ||
        !(peer == identity_.peer) || message.sysid != identity_.peer.systemId ||
        message.compid != identity_.peer.componentId)
        return false;

    if ((message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT ||
         message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST) &&
        (phase_ == MissionUploadPhase::AwaitingRequest ||
         phase_ == MissionUploadPhase::AwaitingAck))
    {
        std::uint16_t sequence = 0;
        std::uint8_t targetSystem = 0;
        std::uint8_t targetComponent = 0;
        std::uint8_t missionType = MAV_MISSION_TYPE_MISSION;
        if (message.msgid == MAVLINK_MSG_ID_MISSION_REQUEST_INT)
        {
            mavlink_mission_request_int_t request{};
            mavlink_msg_mission_request_int_decode(&message, &request);
            sequence = request.seq;
            targetSystem = request.target_system;
            targetComponent = request.target_component;
            missionType = request.mission_type;
        }
        else
        {
            mavlink_mission_request_t request{};
            mavlink_msg_mission_request_decode(&message, &request);
            sequence = request.seq;
            targetSystem = request.target_system;
            targetComponent = request.target_component;
            missionType = request.mission_type;
        }
        if (!MatchesTarget(targetSystem, targetComponent, missionType))
            return false;
        ++metrics_.missionRequestsReceived;
        if (sequence >= mission_.size())
        {
            Fail("Vehicle requested a Mission sequence outside the prepared route.", now);
            return true;
        }
        ResetRetryBudget();
        SendItem(sequence, now);
        return true;
    }

    if (message.msgid == MAVLINK_MSG_ID_MISSION_ACK &&
        phase_ == MissionUploadPhase::AwaitingAck)
    {
        mavlink_mission_ack_t ack{};
        mavlink_msg_mission_ack_decode(&message, &ack);
        if (!MatchesTarget(ack.target_system, ack.target_component,
            ack.mission_type))
            return false;
        if (ack.type != MAV_MISSION_ACCEPTED)
        {
            Fail(AckDiagnostic(ack.type), now);
            return true;
        }
        transportAccepted_ = true;
        verificationStartedAt_ = now;
        metrics_.uploadDurationMs = ElapsedMilliseconds(
            uploadStartedAt_, verificationStartedAt_);
        ResetRetryBudget();
        SendVerificationList(now);
        return true;
    }

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
        SendVerificationItem(
            dronegcs::ardupilot_mission::FirstRouteWireSequence,
            MissionUploadPhase::VerifyingFirst, now);
        return true;
    }

    if (message.msgid == MAVLINK_MSG_ID_MISSION_ITEM_INT &&
        (phase_ == MissionUploadPhase::VerifyingFirst ||
         phase_ == MissionUploadPhase::VerifyingLast))
    {
        mavlink_mission_item_int_t item{};
        mavlink_msg_mission_item_int_decode(&message, &item);
        if (!MatchesTarget(item.target_system, item.target_component,
            item.mission_type))
            return false;

        // ArduPilot may expose its authoritative, dynamic Home as seq 0.
        // It is not an Artifact route item and must not satisfy or fail either
        // route verification request.
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
        Fail("Mission protocol response timed out after bounded retries.", now);
        return;
    }
    ++retriesInWait_;
    ++metrics_.retryCount;
    switch (phase_)
    {
    case MissionUploadPhase::AwaitingRequest:
        if (uniqueServed_ == 0) SendCount(now);
        else SendItem(lastRequestedSequence_, now, false);
        break;
    case MissionUploadPhase::AwaitingAck:
        SendItem(lastRequestedSequence_, now, false);
        break;
    case MissionUploadPhase::VerifyingCount:
        SendVerificationList(now);
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
        safeDiagnostic_ = "Mission upload was cancelled.";
    }
}

bool MissionUploadTransaction::ConsumeProgressChanged()
{
    const bool result = progressChanged_;
    progressChanged_ = false;
    return result;
}
