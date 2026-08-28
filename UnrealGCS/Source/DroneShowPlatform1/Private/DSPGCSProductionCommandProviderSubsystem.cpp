/**
 * @brief 实现 UE 领域命令到 DSP-GCS DTO 的编码和结果事件分发。
 *
 * 分类：Production。
 * 该 Provider 复用 ProductionTelemetry 已启动的双向 Transport，不直接发送 MAVLink。
 */
#include "DSPGCSProductionCommandProviderSubsystem.h"

#include "Async/Async.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "DSPGCSProtocol.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	dspgcs::CommandType ToProtocolType(const EGCSProductionCommandType Type)
	{
		switch (Type)
		{
		case EGCSProductionCommandType::Arm: return dspgcs::CommandType::Arm;
		case EGCSProductionCommandType::Disarm: return dspgcs::CommandType::Disarm;
		case EGCSProductionCommandType::Takeoff: return dspgcs::CommandType::Takeoff;
		case EGCSProductionCommandType::Land: return dspgcs::CommandType::Land;
		case EGCSProductionCommandType::RTL: return dspgcs::CommandType::RTL;
		case EGCSProductionCommandType::SetMode: return dspgcs::CommandType::SetMode;
		case EGCSProductionCommandType::HoverTest: return dspgcs::CommandType::HoverTest;
		case EGCSProductionCommandType::SetLedColor: return dspgcs::CommandType::SetLedColor;
		case EGCSProductionCommandType::TimeStart: return dspgcs::CommandType::TimeStart;
		case EGCSProductionCommandType::TimeStop: return dspgcs::CommandType::TimeStop;
		}
		return dspgcs::CommandType::Arm;
	}

	FString AckStateName(const dspgcs::AckState State)
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

	FString CommandFromProtocolString(const std::string& Value)
	{
		return UTF8_TO_TCHAR(Value.c_str());
	}
}

void UDSPGCSProductionCommandProviderSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UDSPGCSProductionTelemetryProviderSubsystem>();
	GConfig->GetBool(
		TEXT("DSPGCS.ProductionCommand"),
		TEXT("Enabled"),
		bEnabled,
		GGameIni);
	GConfig->GetBool(
		TEXT("DSPGCS.ProductionCommand"),
		TEXT("SafeTestMode"),
		bSafeTestMode,
		GGameIni);

	if (!bEnabled)
	{
		return;
	}
	if (!bSafeTestMode)
	{
		bEnabled = false;
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production commands require SafeTestMode=True."));
		return;
	}

	UDSPGCSProductionTelemetryProviderSubsystem* ProductionChannel =
		GetGameInstance()->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>();
	if (!ProductionChannel)
	{
		bEnabled = false;
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production command channel is unavailable."));
		return;
	}

	Channel = ProductionChannel;
	const TWeakObjectPtr<UDSPGCSProductionCommandProviderSubsystem> WeakThis(this);
	ProductionChannel->SetCommandResultHandler(
		[WeakThis](
			const dspgcs::MessageEnvelope& Envelope,
			const std::vector<std::uint8_t>& Payload)
		{
			if (UDSPGCSProductionCommandProviderSubsystem* Self = WeakThis.Get())
			{
				Self->HandleResultPayload(Envelope, Payload);
			}
		});
	UE_LOG(LogTemp, Display, TEXT("DSP-GCS production command provider enabled (SAFE_TEST_MODE)."));
}

void UDSPGCSProductionCommandProviderSubsystem::Deinitialize()
{
	if (UDSPGCSProductionTelemetryProviderSubsystem* ProductionChannel = Channel.Get())
	{
		ProductionChannel->SetCommandResultHandler({});
	}
	PendingCommands.Reset();
	Channel.Reset();
	Super::Deinitialize();
}

FString UDSPGCSProductionCommandProviderSubsystem::SubmitCommand(
	const EGCSProductionCommandType CommandType,
	const TArray<FString>& TargetDroneIDs,
	const FGCSProductionCommandParameters& Parameters)
{
	if (CommandType == EGCSProductionCommandType::HoverTest &&
		ControlMode != EGCSGroundStationControlMode::Autonomous)
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("DSP-GCS HOVER_TEST rejected: ground-station mode is MANUAL."));
		return {};
	}

	if (CommandType == EGCSProductionCommandType::HoverTest &&
		Parameters.HoverAltitudeMeters <= 0.0f)
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS HOVER_TEST rejected: invalid altitude."));
		return {};
	}

	UDSPGCSProductionTelemetryProviderSubsystem* ProductionChannel = Channel.Get();
	if (!bEnabled || !bSafeTestMode || !ProductionChannel || TargetDroneIDs.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS command rejected locally: provider/targets invalid."));
		return {};
	}
	if (!ProductionChannel->IsTelemetryLinkConnected())
	{
		UE_LOG(
			LogTemp,
			Warning,
			TEXT("DSP-GCS command rejected locally: ServerDisconnected."));
		return {};
	}

	TSet<FString> UniqueTargets;
	dspgcs::CommandRequestDTO Request;
	for (const FString& DroneID : TargetDroneIDs)
	{
		if (!DroneID.IsEmpty() && !UniqueTargets.Contains(DroneID))
		{
			UniqueTargets.Add(DroneID);
			Request.targetDroneIds.emplace_back(TCHAR_TO_UTF8(*DroneID));
		}
	}
	if (Request.targetDroneIds.empty())
	{
		return {};
	}

	const FString CommandID = NextCommandID();
	Request.commandId = TCHAR_TO_UTF8(*CommandID);
	Request.commandType = ToProtocolType(CommandType);
	Request.parameters.takeoffAltitudeMeters = Parameters.TakeoffAltitudeMeters;
	Request.parameters.hoverAltitudeMeters = Parameters.HoverAltitudeMeters;
	Request.parameters.hoverDurationSeconds = Parameters.HoverDurationSeconds;
	Request.parameters.customMode = Parameters.CustomMode;
	Request.parameters.modeName = TCHAR_TO_UTF8(*Parameters.ModeName);
	Request.parameters.start = Parameters.bStart;
	Request.parameters.red = Parameters.Red;
	Request.parameters.green = Parameters.Green;
	Request.parameters.blue = Parameters.Blue;
	Request.parameters.timeControlMask = Parameters.TimeControlMask;
	Request.parameters.scheduledHour = Parameters.ScheduledHour;
	Request.parameters.scheduledMinute = Parameters.ScheduledMinute;
	Request.parameters.scheduledSecond = Parameters.ScheduledSecond;
	Request.source.sourceType = "UI";
	Request.source.sourceId = "UnrealGCS";
	Request.source.stationId = "GCS-PRIMARY";
	Request.timestampUtcMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
	Request.safeTestMode = true;

	dspgcs::MessageEnvelope Envelope;
	Envelope.messageType = dspgcs::MessageType::CommandRequest;
	Envelope.sourceId = "UnrealGCS-ProductionCommand";
	Envelope.sessionId = "UE-PRODUCTION";
	Envelope.sequence = ++Sequence;
	Envelope.timestampUtcMs = Request.timestampUtcMs;
	Envelope.correlationId = Request.commandId;

	if (!ProductionChannel->SendFrame(dspgcs::EncodeCommand(Envelope, Request)))
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS command send failed CommandID=%s"), *CommandID);
		return {};
	}

	FPendingCommand Pending;
	Pending.Type = CommandType;
	Pending.Source = TEXT("UI:UnrealGCS");
	Pending.PendingDroneIDs = MoveTemp(UniqueTargets);
	PendingCommands.Add(CommandID, MoveTemp(Pending));
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS COMMAND CommandID=%s Source=UI:UnrealGCS Targets=%d Type=%d"),
		*CommandID,
		static_cast<int32>(Request.targetDroneIds.size()),
		static_cast<int32>(CommandType));
	if (CommandType == EGCSProductionCommandType::SetLedColor)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("DSP-GCS SetLedColor Targets=%d RGB=(%u,%u,%u)"),
			static_cast<int32>(Request.targetDroneIds.size()),
			Parameters.Red,
			Parameters.Green,
			Parameters.Blue);
	}
	else if (CommandType == EGCSProductionCommandType::TimeStart ||
		CommandType == EGCSProductionCommandType::TimeStop)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("DSP-GCS TimeControl Targets=%d Control=%u Mask=%u LocalTime=%02u:%02u:%02u"),
			static_cast<int32>(Request.targetDroneIds.size()),
			CommandType == EGCSProductionCommandType::TimeStart ? 1u : 0u,
			Parameters.TimeControlMask,
			Parameters.ScheduledHour,
			Parameters.ScheduledMinute,
			Parameters.ScheduledSecond);
	}
	return CommandID;
}

void UDSPGCSProductionCommandProviderSubsystem::SetControlMode(
	const EGCSGroundStationControlMode NewMode)
{
	ControlMode = NewMode;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("GroundStation Control Mode=%s"),
		ControlMode == EGCSGroundStationControlMode::Autonomous
			? TEXT("AUTONOMOUS")
			: TEXT("MANUAL"));
}

void UDSPGCSProductionCommandProviderSubsystem::HandleResultPayload(
	const dspgcs::MessageEnvelope& Envelope,
	const std::vector<std::uint8_t>& Payload)
{
	dspgcs::CommandResultBatchDTO Batch;
	if (!dspgcs::DecodeCommandResult(Payload, Batch))
	{
		return;
	}
	const TWeakObjectPtr<UDSPGCSProductionCommandProviderSubsystem> WeakThis(this);
	AsyncTask(
		ENamedThreads::GameThread,
		[WeakThis,
		 EnvelopeCopy = dspgcs::MessageEnvelope(Envelope),
		 BatchCopy = MoveTemp(Batch)]() mutable
	{
		if (UDSPGCSProductionCommandProviderSubsystem* Self = WeakThis.Get())
		{
			Self->ApplyResultBatch(
				MoveTemp(EnvelopeCopy),
				MoveTemp(BatchCopy));
		}
	});
}

void UDSPGCSProductionCommandProviderSubsystem::ApplyResultBatch(
	dspgcs::MessageEnvelope Envelope,
	dspgcs::CommandResultBatchDTO Batch)
{
	const FString CommandID = CommandFromProtocolString(Batch.commandId);
	FPendingCommand* Pending = PendingCommands.Find(CommandID);
	if (!Pending)
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS result for unknown CommandID=%s"), *CommandID);
		return;
	}

	for (const dspgcs::DroneCommandResultDTO& ProtocolResult : Batch.results)
	{
		FGCSProductionCommandResult Result;
		Result.CommandID = CommandID;
		Result.DroneID = CommandFromProtocolString(ProtocolResult.droneId);
		Result.CommandType = Pending->Type;
		Result.Source = Pending->Source;
		Result.AckState = AckStateName(ProtocolResult.ackState);
		Result.ErrorCode = CommandFromProtocolString(ProtocolResult.error.code);
		Result.Detail = CommandFromProtocolString(ProtocolResult.error.message);
		Result.bSimulated = ProtocolResult.ackState == dspgcs::AckState::Simulated;
		Result.bFinal = ProtocolResult.final;
		LastResult = Result;
		Pending->PendingDroneIDs.Remove(Result.DroneID);
		OnCommandResult.Broadcast(Result);
		UE_LOG(
			LogTemp,
			Display,
			TEXT("DSP-GCS RESULT CommandID=%s DroneID=%s Result=%s Source=%s"),
			*Result.CommandID,
			*Result.DroneID,
			*Result.AckState,
			*Result.Source);
	}

	if (Batch.final || Pending->PendingDroneIDs.IsEmpty())
	{
		PendingCommands.Remove(CommandID);
	}
}

FString UDSPGCSProductionCommandProviderSubsystem::NextCommandID()
{
	return FString::Printf(
		TEXT("UE-PROD-%lld-%d"),
		FDateTime::UtcNow().ToUnixTimestamp(),
		++CommandCounter);
}
