#pragma once

#include "DSPGCSProductionCommandTypes.h"
#include "DSPGCSProtocol.h"

namespace GCSCommandResultConversion
{
	inline FString AckStateName(const dspgcs::AckState State)
	{
		switch (State)
		{
		case dspgcs::AckState::Received: return TEXT("RECEIVED");
		case dspgcs::AckState::Validated: return TEXT("VALIDATED");
		case dspgcs::AckState::Queued: return TEXT("QUEUED");
		case dspgcs::AckState::Sent: return TEXT("SENT");
		case dspgcs::AckState::InProgress: return TEXT("IN_PROGRESS");
		case dspgcs::AckState::Accepted: return TEXT("ACCEPTED");
		case dspgcs::AckState::Rejected: return TEXT("REJECTED");
		case dspgcs::AckState::Failed: return TEXT("FAILED");
		case dspgcs::AckState::TimedOut: return TEXT("TIMEOUT");
		case dspgcs::AckState::Cancelled: return TEXT("CANCELLED");
		case dspgcs::AckState::Simulated: return TEXT("SIMULATED");
		}
		return TEXT("UNKNOWN");
	}

	inline EGCSCommandResultState ToState(const dspgcs::AckState State)
	{
		switch (State)
		{
		case dspgcs::AckState::Received: return EGCSCommandResultState::Received;
		case dspgcs::AckState::Validated: return EGCSCommandResultState::Validated;
		case dspgcs::AckState::Queued: return EGCSCommandResultState::Queued;
		case dspgcs::AckState::Sent: return EGCSCommandResultState::Sent;
		case dspgcs::AckState::InProgress: return EGCSCommandResultState::InProgress;
		case dspgcs::AckState::Accepted: return EGCSCommandResultState::Accepted;
		case dspgcs::AckState::Rejected: return EGCSCommandResultState::Rejected;
		case dspgcs::AckState::Failed: return EGCSCommandResultState::Failed;
		case dspgcs::AckState::TimedOut: return EGCSCommandResultState::TimedOut;
		case dspgcs::AckState::Cancelled: return EGCSCommandResultState::Cancelled;
		case dspgcs::AckState::Simulated: return EGCSCommandResultState::Simulated;
		default: return EGCSCommandResultState::Unknown;
		}
	}

	inline FGCSProductionCommandResult ToResult(
		const FString& CommandID, const dspgcs::DroneCommandResultDTO& Source)
	{
		FGCSProductionCommandResult Result;
		Result.CommandID = CommandID;
		Result.DroneID = UTF8_TO_TCHAR(Source.droneId.c_str());
		Result.State = ToState(Source.ackState);
		Result.AckState = AckStateName(Source.ackState);
		Result.ErrorCode = UTF8_TO_TCHAR(Source.error.code.c_str());
		Result.Detail = UTF8_TO_TCHAR(Source.error.message.c_str());
		Result.bSimulated = Source.ackState == dspgcs::AckState::Simulated;
		Result.bFinal = Source.final;
		return Result;
	}
}
