#include "MissionUploadTransaction.h"

#include <utility>

namespace
{
    bool SameFloatBits(const float left, const float right)
    {
        return dronegcs::route_mission::Binary32ToBits(left) ==
            dronegcs::route_mission::Binary32ToBits(right);
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

MissionUploadTransaction::MissionUploadTransaction(
    MissionUploadTransactionIdentity identity,
    std::vector<dronegcs::route_mission::MissionItemIntV1> mission,
    IMissionProtocolTransport& transport,
    MissionUploadTransactionConfig config)
    : identity_(std::move(identity))
    , mission_(std::move(mission))
    , transport_(transport)
    , config_(config)
    , served_(mission_.size(), false)
{
}

bool MissionUploadTransaction::IsTerminal() const
{
    return phase_ == MissionUploadPhase::Succeeded ||
        phase_ == MissionUploadPhase::Failed ||
        phase_ == MissionUploadPhase::Cancelled;
}

void MissionUploadTransaction::Fail(const char* diagnostic)
{
    if (IsTerminal()) return;
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
        Fail("Mission transport send failed.");
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
        Fail("Vehicle requested a Mission sequence outside the prepared route.");
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
        metrics_.logicalBytesCompleted +=
            dronegcs::route_mission::MissionItemRecordBytesV1;
        progressChanged_ = true;
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
    if (phase_ != MissionUploadPhase::Idle || mission_.empty() ||
        mission_.size() > 65535 || config_.responseTimeout.count() <= 0)
    {
        Fail("Prepared Mission is invalid for standard Mission upload.");
        return false;
    }
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
        received.frame == expected.Frame &&
        received.command == expected.Command &&
        SameFloatBits(received.param1, expected.Param1) &&
        SameFloatBits(received.param2, expected.Param2) &&
        SameFloatBits(received.param3, expected.Param3) &&
        SameFloatBits(received.param4, expected.Param4) &&
        received.x == expected.X && received.y == expected.Y &&
        SameFloatBits(received.z, expected.Z) &&
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
            Fail("Vehicle requested a Mission sequence outside the prepared route.");
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
            Fail(AckDiagnostic(ack.type));
            return true;
        }
        transportAccepted_ = true;
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
            Fail("Mission verification count differs from the prepared route.");
            return true;
        }
        ResetRetryBudget();
        SendVerificationItem(0, MissionUploadPhase::VerifyingFirst, now);
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
        const std::uint16_t expectedSequence = phase_ ==
            MissionUploadPhase::VerifyingFirst
            ? 0 : static_cast<std::uint16_t>(mission_.size() - 1);
        if (item.seq != expectedSequence ||
            !VerifyItem(item, mission_[expectedSequence]))
        {
            Fail(phase_ == MissionUploadPhase::VerifyingFirst
                ? "First Mission item verification failed."
                : "Last Mission item verification failed.");
            return true;
        }
        ResetRetryBudget();
        if (phase_ == MissionUploadPhase::VerifyingFirst && mission_.size() > 1)
        {
            SendVerificationItem(
                static_cast<std::uint16_t>(mission_.size() - 1),
                MissionUploadPhase::VerifyingLast, now);
        }
        else
        {
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
        Fail("Mission protocol response timed out after bounded retries.");
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
        SendVerificationItem(0, MissionUploadPhase::VerifyingFirst, now);
        break;
    case MissionUploadPhase::VerifyingLast:
        SendVerificationItem(static_cast<std::uint16_t>(mission_.size() - 1),
            MissionUploadPhase::VerifyingLast, now);
        break;
    default:
        break;
    }
}

void MissionUploadTransaction::Cancel()
{
    if (!IsTerminal())
    {
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
