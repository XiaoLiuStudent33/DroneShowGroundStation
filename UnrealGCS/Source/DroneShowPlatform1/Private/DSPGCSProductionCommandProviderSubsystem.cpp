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
#include "DSPGCSCommandBatching.h"
#include "GCSCommandResultConversion.h"
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
		case EGCSProductionCommandType::ExecuteDanceStep: return dspgcs::CommandType::ExecuteDanceStep;
		case EGCSProductionCommandType::EndDanceStep: return dspgcs::CommandType::EndDanceStep;
		case EGCSProductionCommandType::MissionPause: return dspgcs::CommandType::MissionPause;
		case EGCSProductionCommandType::MissionForward: return dspgcs::CommandType::MissionForward;
		case EGCSProductionCommandType::MissionBackward: return dspgcs::CommandType::MissionBackward;
		case EGCSProductionCommandType::ChangeSpeed: return dspgcs::CommandType::ChangeSpeed;
		case EGCSProductionCommandType::TriggerSmoke: return dspgcs::CommandType::TriggerSmoke;
		case EGCSProductionCommandType::StopTelemetry: return dspgcs::CommandType::StopTelemetry;
		case EGCSProductionCommandType::ResumeTelemetry: return dspgcs::CommandType::ResumeTelemetry;
		case EGCSProductionCommandType::DisableLedOutput: return dspgcs::CommandType::DisableLedOutput;
		case EGCSProductionCommandType::RestoreLedOutput: return dspgcs::CommandType::RestoreLedOutput;
		case EGCSProductionCommandType::Wake: return dspgcs::CommandType::Wake;
		case EGCSProductionCommandType::Standby: return dspgcs::CommandType::Standby;
		case EGCSProductionCommandType::Reboot: return dspgcs::CommandType::Reboot;
		case EGCSProductionCommandType::Shutdown: return dspgcs::CommandType::Shutdown;
		}
		return dspgcs::CommandType::Arm;
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
			const dspgcs::MessageEnvelope&,
			const std::vector<std::uint8_t>& Payload)
		{
			if (WeakThis.Get())
			{
				dspgcs::CommandResultBatchDTO Batch;
				if (!dspgcs::DecodeCommandResult(Payload, Batch))
				{
					return;
				}
				AsyncTask(ENamedThreads::GameThread,
					[WeakThis, Batch = MoveTemp(Batch)]() mutable
				{
					if (UDSPGCSProductionCommandProviderSubsystem* Provider = WeakThis.Get())
					{
						const FString CommandID = CommandFromProtocolString(Batch.commandId);
						TArray<FGCSProductionCommandResult> Results;
						Results.Reserve(static_cast<int32>(Batch.results.size()));
						for (const dspgcs::DroneCommandResultDTO& Item : Batch.results)
						{
							Results.Add(GCSCommandResultConversion::ToResult(CommandID, Item));
						}
						Provider->ApplyResultBatch(CommandID, MoveTemp(Results), Batch.final);
					}
				});
			}
		});
	UE_LOG(LogTemp, Display, TEXT("DSP-GCS production command provider enabled (SAFE_TEST_MODE)."));
}

void UDSPGCSProductionCommandProviderSubsystem::Deinitialize()
{
    FTSTicker::GetCoreTicker().RemoveTicker(BatchTicker);
    BatchTicker.Reset();
    QueuedBatches.clear();
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
	Request.parameters.advancedSpeedMetersPerSecond = Parameters.AdvancedSpeedMetersPerSecond;
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

    const auto Parts = dspgcs::SplitCommandTargets(Envelope, Request);
    if (Parts.empty()) return {};
    for (const auto& Part : Parts)
    {
        auto ChildEnvelope = Envelope;
        ChildEnvelope.correlationId = Part.commandId;
        ChildEnvelope.sequence = ++Sequence;
        auto Bytes = dspgcs::EncodeCommand(ChildEnvelope, Part);
        const FString ChildID = UTF8_TO_TCHAR(Part.commandId.c_str());
        FPendingCommand Pending;
        Pending.Type = CommandType;
        Pending.Source = TEXT("UI:UnrealGCS");
        Pending.OperationID = CommandID;
        for (const auto& ID : Part.targetDroneIds)
            Pending.PendingDroneIDs.Add(UTF8_TO_TCHAR(ID.c_str()));
        PendingCommands.Add(ChildID, MoveTemp(Pending));
        if (Parts.size() == 1)
        {
            if (!ProductionChannel->SendFrame(Bytes))
            {
                PendingCommands.Remove(ChildID);
                UE_LOG(LogTemp, Error, TEXT("DSP-GCS command send failed CommandID=%s"), *CommandID);
                return {};
            }
        }
        else
        {
            FQueuedBatch Batch;
            Batch.CommandID = ChildID;
            Batch.Bytes.Append(Bytes.data(), static_cast<int32>(Bytes.size()));
            QueuedBatches.push_back(MoveTemp(Batch));
        }
    }
    if (!QueuedBatches.empty() && !BatchTicker.IsValid())
        BatchTicker = FTSTicker::GetCoreTicker().AddTicker(
            FTickerDelegate::CreateUObject(this, &ThisClass::PumpCommandBatches), 0.02f);
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
		CommandType == EGCSProductionCommandType::TimeStop ||
		CommandType == EGCSProductionCommandType::EndDanceStep)
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
	else if (CommandType == EGCSProductionCommandType::ExecuteDanceStep)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("DSP-GCS ExecuteDanceStep Targets=%d Actions=AUTO+MAV_CMD_USER_1(1)"),
			static_cast<int32>(Request.targetDroneIds.size()));
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

void UDSPGCSProductionCommandProviderSubsystem::ApplyResultBatch(
	const FString& CommandID,
	TArray<FGCSProductionCommandResult> Results,
	const bool bBatchFinal)
{
	FPendingCommand* Pending = PendingCommands.Find(CommandID);
	if (!Pending)
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS result for unknown CommandID=%s"), *CommandID);
		return;
	}

    // Finish bookkeeping before broadcasting: subscribers can submit another
    // command (rehashing PendingCommands) or tear down/rebind the provider.
    TArray<FGCSProductionCommandResult> Deliver;
    for (FGCSProductionCommandResult& Result : Results)
    {
        if (!Pending->PendingDroneIDs.Contains(Result.DroneID)) continue;
        Result.CommandID = Pending->OperationID;
        Result.CommandType = Pending->Type;
        Result.Source = Pending->Source;
        if (Result.bFinal) Pending->PendingDroneIDs.Remove(Result.DroneID);
        Deliver.Add(MoveTemp(Result));
    }
    if (bBatchFinal)
    {
        for (const FString& DroneID : Pending->PendingDroneIDs)
        {
            FGCSProductionCommandResult Missing;
            Missing.CommandID = Pending->OperationID;
            Missing.DroneID = DroneID;
            Missing.CommandType = Pending->Type;
            Missing.Source = Pending->Source;
            Missing.State = EGCSCommandResultState::Failed;
            Missing.AckState = TEXT("FAILED");
            Missing.ErrorCode = TEXT("INCOMPLETE_FINAL_RESULT");
            Missing.Detail = TEXT("Final batch omitted a terminal vehicle result");
            Missing.bFinal = true;
            Deliver.Add(MoveTemp(Missing));
        }
    }
    if (bBatchFinal || Pending->PendingDroneIDs.IsEmpty()) PendingCommands.Remove(CommandID);
    for (const auto& Result : Deliver)
    {
        LastResult = Result;
        OnCommandResult.Broadcast(Result);
    }
}

bool UDSPGCSProductionCommandProviderSubsystem::PumpCommandBatches(float)
{
    if (QueuedBatches.empty())
    {
        BatchTicker.Reset();
        return false;
    }
    // Pop before callbacks. Each child is attempted once; never replay earlier
    // successful children when another child's local send fails.
    FQueuedBatch Batch = MoveTemp(QueuedBatches.front());
    QueuedBatches.pop_front();
    const auto* Pending = PendingCommands.Find(Batch.CommandID);
    if (!Pending) return true;
    auto* ProductionChannel = Channel.Get();
    const std::vector<uint8> Bytes(Batch.Bytes.GetData(), Batch.Bytes.GetData() + Batch.Bytes.Num());
    if (ProductionChannel && ProductionChannel->IsTelemetryLinkConnected() &&
        ProductionChannel->SendFrame(Bytes)) return true;
    TArray<FGCSProductionCommandResult> Failed;
    for (const FString& DroneID : Pending->PendingDroneIDs)
    {
        FGCSProductionCommandResult Result;
        Result.DroneID = DroneID;
        Result.State = EGCSCommandResultState::Failed;
        Result.AckState = TEXT("FAILED");
        Result.ErrorCode = TEXT("LOCAL_SEND_FAILED");
        Result.Detail = TEXT("Command batch could not be sent");
        Result.bFinal = true;
        Failed.Add(MoveTemp(Result));
    }
    ApplyResultBatch(Batch.CommandID, MoveTemp(Failed), true);
    return true;
}

FString UDSPGCSProductionCommandProviderSubsystem::NextCommandID()
{
	return FString::Printf(
		TEXT("UE-PROD-%lld-%d"),
		FDateTime::UtcNow().ToUnixTimestamp(),
		++CommandCounter);
}


bool UDSPGCSProductionCommandProviderSubsystem::IsReadyForOperatorCommands() const
{
    const auto* Telemetry = Channel.Get();
    return IsEnabled() && Telemetry && Telemetry->IsEnabled()
        && Telemetry->IsTelemetryLinkConnected();
}
