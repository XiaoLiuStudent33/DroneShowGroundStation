/**
 * @brief 实现旧 Stage 4.2 Loopback acceptance 命令/遥测验证流程。
 *
 * 分类：Acceptance / Loopback Test；当前 Production 配置关闭。
 * 它是测试 Provider，而 FDSPGCSLoopbackTransport 同时仍是 Production Transport。
 */
#include "DSPGCSLoopbackProviderSubsystem.h"

#include "Async/Async.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "GCSServerAdapterSubsystem.h"
#include "Misc/ConfigCacheIni.h"

namespace
{
	FString FromProtocolString(const std::string& Value)
	{
		return UTF8_TO_TCHAR(Value.c_str());
	}

	std::string ToProtocolString(const FString& Value)
	{
		return TCHAR_TO_UTF8(*Value);
	}

	FDateTime FromUnixMilliseconds(const int64 TimestampUtcMs)
	{
		return TimestampUtcMs > 0
			? FDateTime::FromUnixTimestamp(TimestampUtcMs / 1000)
			: FDateTime::UtcNow();
	}

	FDroneData ToFleetDrone(const dspgcs::TelemetryDTO& Source, const bool bSelected)
	{
		FDroneData Target;
		Target.DroneID = FromProtocolString(Source.droneId);
		Target.SysID = static_cast<int32>(Source.sysId);
		Target.GPSLat = Source.gps.latitudeDeg;
		Target.GPSLon = Source.gps.longitudeDeg;
		Target.bGPSValid = Source.gps.valid;
		Target.GPSFixType = static_cast<int32>(Source.gps.fixType);
		Target.Altitude = Source.altitude.relativeMeters;
		Target.AltitudeAMSL = Source.altitude.amslMeters;
		Target.bAltitudeValid = Source.altitude.valid;
		Target.Battery = Source.battery.remainingPercent;
		Target.Mode = FromProtocolString(Source.mode.modeName);
		Target.RSSI = static_cast<int32>(Source.rssi.valueDbm);
		Target.bOnline = Source.connection.online;
		Target.LEDColor = FLinearColor(
			static_cast<float>(Source.led.red) / 255.0f,
			static_cast<float>(Source.led.green) / 255.0f,
			static_cast<float>(Source.led.blue) / 255.0f,
			1.0f);
		Target.bSelected = bSelected;
		return Target;
	}
}

void UDSPGCSLoopbackProviderSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGCSServerAdapterSubsystem>();

	GConfig->GetBool(TEXT("DSPGCS.Transport"), TEXT("Enabled"), bEnabled, GGameIni);
	GConfig->GetBool(TEXT("DSPGCS.Transport"), TEXT("SafeTestMode"), bSafeTestMode, GGameIni);
	if (!bEnabled)
	{
		return;
	}

	UGCSServerAdapterSubsystem* Adapter =
		GetGameInstance()->GetSubsystem<UGCSServerAdapterSubsystem>();
	if (!Adapter || !Adapter->RegisterProvider(*this))
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS loopback provider registration failed."));
	}
}

void UDSPGCSLoopbackProviderSubsystem::Deinitialize()
{
	if (UGCSServerAdapterSubsystem* Adapter = AdapterRef.Get())
	{
		Adapter->UnregisterProvider(*this);
	}
	Stop();
	Super::Deinitialize();
}

FName UDSPGCSLoopbackProviderSubsystem::GetProviderID() const
{
	return TEXT("DSPGCSLoopback");
}

bool UDSPGCSLoopbackProviderSubsystem::Start(UGCSServerAdapterSubsystem& Adapter)
{
	if (!bSafeTestMode)
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS loopback refused: SafeTestMode must be true."));
		return false;
	}

	AdapterRef = &Adapter;
	return CreateTransport();
}

void UDSPGCSLoopbackProviderSubsystem::Stop()
{
	if (Transport)
	{
		Transport->Receive({});
		Transport->Stop();
		Transport.reset();
	}
	AdapterRef.Reset();
}

bool UDSPGCSLoopbackProviderSubsystem::CreateTransport()
{
	FString TransportTypeString = TEXT("Loopback");
	FString ConsoleIPString = TEXT("127.0.0.1");
	FString UnrealBindIPString = TEXT("127.0.0.1");
	int32 ConsolePort = 12001;
	int32 UnrealPort = 12002;
	GConfig->GetString(TEXT("DSPGCS.Transport"), TEXT("TransportType"), TransportTypeString, GGameIni);
	GConfig->GetString(TEXT("DSPGCS.Transport"), TEXT("RemoteAddress"), ConsoleIPString, GGameIni);
	GConfig->GetString(TEXT("DSPGCS.Transport"), TEXT("BindAddress"), UnrealBindIPString, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.Transport"), TEXT("RemotePort"), ConsolePort, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.Transport"), TEXT("BindPort"), UnrealPort, GGameIni);

	dspgcs::DSPGCSTransportConfig TransportConfig;
	if (!dspgcs::TryParseTransportType(TCHAR_TO_UTF8(*TransportTypeString), TransportConfig.type))
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS CONFIG ERROR: Unsupported TransportType=%s"), *TransportTypeString);
		return false;
	}
	if (UnrealPort <= 0 || UnrealPort > 65535 || ConsolePort <= 0 || ConsolePort > 65535
		|| UnrealBindIPString.IsEmpty() || ConsoleIPString.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS CONFIG ERROR: invalid endpoint configuration."));
		return false;
	}
	TransportConfig.bindAddress = TCHAR_TO_UTF8(*UnrealBindIPString);
	TransportConfig.bindPort = static_cast<uint16>(UnrealPort);
	TransportConfig.remoteAddress = TCHAR_TO_UTF8(*ConsoleIPString);
	TransportConfig.remotePort = static_cast<uint16>(ConsolePort);

	std::string FactoryError;
	Transport = dspgcs::DSPGCSTransportFactory::CreateTransport(TransportConfig, FactoryError);
	if (!Transport)
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS FACTORY ERROR: %s"), UTF8_TO_TCHAR(FactoryError.c_str()));
		return false;
	}

	const TWeakObjectPtr<UDSPGCSLoopbackProviderSubsystem> WeakThis(this);
	Transport->Receive([WeakThis](dspgcs::TransportFrame Frame)
	{
		if (UDSPGCSLoopbackProviderSubsystem* Self = WeakThis.Get())
		{
			Self->HandleTransportFrame(MoveTemp(Frame));
		}
	});
	if (!Transport->Start())
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("DSP-GCS TRANSPORT START ERROR: Type=%s Bind=%s:%d Remote=%s:%d State=%d"),
			UTF8_TO_TCHAR(dspgcs::ToString(TransportConfig.type)),
			*UnrealBindIPString,
			UnrealPort,
			*ConsoleIPString,
			ConsolePort,
			static_cast<int32>(Transport->GetState()));
		Transport.reset();
		return false;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS Factory selected TransportType=%s Bind=%s:%d Remote=%s:%d State=%d SAFE_TEST_MODE=true"),
		UTF8_TO_TCHAR(dspgcs::ToString(TransportConfig.type)),
		*UnrealBindIPString,
		UnrealPort,
		*ConsoleIPString,
		ConsolePort,
		static_cast<int32>(Transport->GetState()));
	return true;
}

void UDSPGCSLoopbackProviderSubsystem::HandleTransportFrame(dspgcs::TransportFrame Frame)
{
	dspgcs::MessageEnvelope Envelope;
	std::vector<std::uint8_t> Payload;
	if (!dspgcs::DecodeEnvelope(Frame.bytes, Envelope, Payload))
	{
		UE_LOG(LogTemp, Warning, TEXT("DSP-GCS rejected an invalid frame envelope."));
		return;
	}

	const TWeakObjectPtr<UDSPGCSLoopbackProviderSubsystem> WeakThis(this);
	if (Envelope.messageType == dspgcs::MessageType::TelemetryBatch)
	{
		dspgcs::TelemetryBatchDTO Batch;
		if (!dspgcs::DecodeTelemetry(Payload, Batch))
		{
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Envelope, Batch]()
		{
			UDSPGCSLoopbackProviderSubsystem* Self = WeakThis.Get();
			UGCSServerAdapterSubsystem* Adapter = Self ? Self->AdapterRef.Get() : nullptr;
			if (!Self || !Adapter)
			{
				return;
			}

			FGCSFleetSnapshot Snapshot;
			Snapshot.Header.SchemaVersion = GCSDataSchema::CurrentVersion;
			Snapshot.Header.SourceID = FromProtocolString(Envelope.sourceId);
			Snapshot.Header.SessionID = FromProtocolString(Envelope.sessionId);
			Snapshot.Header.Sequence = static_cast<int64>(Envelope.sequence);
			Snapshot.Header.SourceTimestamp = FromUnixMilliseconds(Envelope.timestampUtcMs);
			Snapshot.FleetRevision = static_cast<int64>(Batch.fleetRevision);
			Snapshot.Drones.Reserve(static_cast<int32>(Batch.items.size()));
			for (size_t Index = 0; Index < Batch.items.size(); ++Index)
			{
				Snapshot.Drones.Add(ToFleetDrone(Batch.items[Index], Index == 0));
			}

			if (Adapter->PushFleetSnapshot(Snapshot))
			{
				Self->ReceivedTelemetryCount = Snapshot.Drones.Num();
				UE_LOG(
					LogTemp,
					Display,
					TEXT("DSP-GCS TELEMETRY ConsoleDTO->Loopback->FleetManager count=%d revision=%lld"),
					Snapshot.Drones.Num(),
					Snapshot.FleetRevision);
				Self->SendAcceptanceCommands();
			}
		});
		return;
	}

	if (Envelope.messageType == dspgcs::MessageType::CommandResultBatch)
	{
		dspgcs::CommandResultBatchDTO Batch;
		if (!dspgcs::DecodeCommandResult(Payload, Batch))
		{
			return;
		}

		AsyncTask(ENamedThreads::GameThread, [WeakThis, Batch]()
		{
			UDSPGCSLoopbackProviderSubsystem* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			const FString BatchCommandID = FromProtocolString(Batch.commandId);
			for (const dspgcs::DroneCommandResultDTO& Result : Batch.results)
			{
				const FString ResultCommandID = FromProtocolString(Result.commandId);
				const FString DroneID = FromProtocolString(Result.droneId);
				if (ResultCommandID != BatchCommandID || !Result.final)
				{
					continue;
				}

				if (ResultCommandID == Self->ArmCommandID)
				{
					Self->ArmResultDroneIDs.Add(DroneID);
				}
				else if (ResultCommandID == Self->HoverCommandID)
				{
					Self->HoverResultDroneIDs.Add(DroneID);
				}
				++Self->ReceivedResultCount;
				UE_LOG(
					LogTemp,
					Display,
					TEXT("DSP-GCS RESULT CommandID=%s DroneID=%s Ack=%d Final=%d"),
					*ResultCommandID,
					*DroneID,
					static_cast<int32>(Result.ackState),
					Result.final ? 1 : 0);
			}
			Self->EvaluateAcceptance();
		});
	}
}

void UDSPGCSLoopbackProviderSubsystem::SendAcceptanceCommands()
{
	if (bCommandsSent || !bSafeTestMode)
	{
		return;
	}

	bCommandsSent = true;
	const int64 Unique = FDateTime::UtcNow().GetTicks();
	ArmCommandID = FString::Printf(TEXT("UE-LOOPBACK-ARM-%lld"), Unique);
	HoverCommandID = FString::Printf(TEXT("UE-LOOPBACK-HOVER-%lld"), Unique);

	SendCommandFrame(ArmCommandID, static_cast<uint16>(dspgcs::CommandType::Arm), { TEXT("Drone001") }, true);
	SendCommandFrame(
		HoverCommandID,
		static_cast<uint16>(dspgcs::CommandType::HoverTest),
		{ TEXT("Drone001"), TEXT("Drone002"), TEXT("Drone003") },
		true);
}

bool UDSPGCSLoopbackProviderSubsystem::SendCommandFrame(
	const FString& CommandID,
	const uint16 CommandType,
	const TArray<FString>& TargetDroneIDs,
	const bool bStart)
{
	if (!Transport
		|| Transport->GetState() != dspgcs::TransportState::Running
		|| !bSafeTestMode)
	{
		return false;
	}

	dspgcs::CommandRequestDTO Request;
	Request.commandId = ToProtocolString(CommandID);
	Request.commandType = static_cast<dspgcs::CommandType>(CommandType);
	for (const FString& DroneID : TargetDroneIDs)
	{
		Request.targetDroneIds.push_back(ToProtocolString(DroneID));
	}
	Request.parameters.start = bStart;
	Request.source.sourceType = "AUTOMATION";
	Request.source.sourceId = "UnrealGCS";
	Request.source.stationId = "LOCAL-LOOPBACK";
	Request.timestampUtcMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
	Request.safeTestMode = true;

	dspgcs::MessageEnvelope Envelope;
	Envelope.messageType = dspgcs::MessageType::CommandRequest;
	Envelope.sourceId = "UnrealGCS";
	Envelope.sessionId = "UE-LOOPBACK-SESSION";
	Envelope.sequence = static_cast<uint64>(++OutgoingSequence);
	Envelope.timestampUtcMs = Request.timestampUtcMs;
	Envelope.correlationId = Request.commandId;
	const std::vector<std::uint8_t> Frame = dspgcs::EncodeCommand(Envelope, Request);

	const bool bSent = Transport->Send({ Frame });
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS COMMAND sent CommandID=%s Type=%d Targets=%d SAFE_TEST_MODE=true"),
		*CommandID,
		static_cast<int32>(CommandType),
		TargetDroneIDs.Num());
	return bSent;
}

void UDSPGCSLoopbackProviderSubsystem::EvaluateAcceptance()
{
	if (bAcceptancePassed
		|| ReceivedTelemetryCount != 3
		|| ArmResultDroneIDs.Num() != 1
		|| !ArmResultDroneIDs.Contains(TEXT("Drone001"))
		|| HoverResultDroneIDs.Num() != 3
		|| !HoverResultDroneIDs.Contains(TEXT("Drone001"))
		|| !HoverResultDroneIDs.Contains(TEXT("Drone002"))
		|| !HoverResultDroneIDs.Contains(TEXT("Drone003")))
	{
		return;
	}

	bAcceptancePassed = true;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS LOOPBACK UE PASS telemetry=3 singleArm=1 multiHover=3 CommandID=present perDroneResults=present UI=FleetManager"));
}
