/**
 * @brief 实现 UE Production DSP Frame 接收、解码、分片重组和领域模型转换。
 *
 * 分类：Production。
 * 调用链：DSPGCSLoopbackTransport -> ProductionTelemetryProvider -> GCSServerAdapter -> FleetManager。
 * 当前 localhost Transport 是实际 Production 通道，不是 Test Only。
 */
#include "DSPGCSProductionTelemetryProviderSubsystem.h"

#include "DSPGCSFrozenAssignmentProtocol.h"
#include "DSPGCSUploadJobProtocol.h"

#include "DSPGCSProtocol.h"
#include "GCSVehicleStateProjection.h"
#include "DSPGCSTransportFactory.h"
#include "GCSServerAdapterSubsystem.h"
#include "RouteGpsCaptureSubsystem.h"
#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"

#include <iterator>
#include <limits>

namespace
{
	FString FromProductionProtocolString(const std::string& Value)
	{
		return UTF8_TO_TCHAR(Value.c_str());
	}

	FDateTime FromProductionUnixMilliseconds(const int64 TimestampUtcMs)
	{
		return TimestampUtcMs > 0
			? FDateTime::FromUnixTimestamp(TimestampUtcMs / 1000)
			: FDateTime::UtcNow();
	}

	FString FormatMDHKGPSUtc(const dspgcs::MDHKStatusDTO& Status)
	{
		const bool bValid = Status.gpsYear != 0
			&& Status.gpsMonth >= 1 && Status.gpsMonth <= 12
			&& Status.gpsDay >= 1 && Status.gpsDay <= 31
			&& Status.gpsHour <= 23
			&& Status.gpsMinute <= 59
			&& Status.gpsSecond <= 60;
		return bValid
			? FString::Printf(
				TEXT("%04u-%02u-%02u %02u:%02u:%02u"),
				static_cast<uint32>(Status.gpsYear),
				static_cast<uint32>(Status.gpsMonth),
				static_cast<uint32>(Status.gpsDay),
				static_cast<uint32>(Status.gpsHour),
				static_cast<uint32>(Status.gpsMinute),
				static_cast<uint32>(Status.gpsSecond))
			: TEXT("-1");
	}

	FString FormatMDHKFlightSoftwareVersion(const uint32 Version)
	{
		return FString::Printf(
			TEXT("%u.%u.%u (0x%08X)"),
			(Version >> 24) & 0xffu,
			(Version >> 16) & 0xffu,
			(Version >> 8) & 0xffu,
			Version);
	}

	FString FormatMDHKCustomVersion(const std::vector<std::uint8_t>& Version)
	{
		if (Version.size() != 8)
		{
			return TEXT("-1");
		}

		FString Result;
		Result.Reserve(16);
		for (const std::uint8_t Byte : Version)
		{
			Result += FString::Printf(TEXT("%02X"), static_cast<uint32>(Byte));
		}
		return Result;
	}


}

void UDSPGCSProductionTelemetryProviderSubsystem::Initialize(
	FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGCSServerAdapterSubsystem>();

	GConfig->GetBool(
		TEXT("DSPGCS.ProductionTelemetry"),
		TEXT("Enabled"),
		bEnabled,
		GGameIni);
	if (!bEnabled)
	{
		return;
	}

	UGCSServerAdapterSubsystem* Adapter =
		GetGameInstance()->GetSubsystem<UGCSServerAdapterSubsystem>();
	if (!Adapter || !Adapter->RegisterProvider(*this))
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production telemetry provider registration failed."));
	}
}

void UDSPGCSProductionTelemetryProviderSubsystem::Deinitialize()
{
	if (UGCSServerAdapterSubsystem* Adapter = AdapterRef.Get())
	{
		Adapter->UnregisterProvider(*this);
	}
	Stop();
	Super::Deinitialize();
}

FName UDSPGCSProductionTelemetryProviderSubsystem::GetProviderID() const
{
	return TEXT("DSPGCSProductionTelemetry");
}

bool UDSPGCSProductionTelemetryProviderSubsystem::Start(
	UGCSServerAdapterSubsystem& Adapter)
{
	AdapterRef = &Adapter;
	if (!CreateTransport())
	{
		return false;
	}
	StartTelemetryHealthMonitor();
	return true;
}

void UDSPGCSProductionTelemetryProviderSubsystem::Stop()
{
	StopTelemetryHealthMonitor();
	LastValidTelemetryReceiveTimeSeconds.store(0.0);
	SetTelemetryLinkState(EDSPGCSTelemetryLinkState::Disconnected);
	SetCommandResultHandler({});
	SetFrozenAssignmentAckHandler({});
	if (Transport)
	{
		Transport->Receive({});
		Transport->Stop();
		Transport.reset();
	}
	ResetPendingTelemetrySnapshot();
	{
		FScopeLock Lock(&MDHKTelemetryRowsMutex);
		MDHKTelemetryRows.Reset();
	}
	AdapterRef.Reset();
}

bool UDSPGCSProductionTelemetryProviderSubsystem::SendFrame(
	const std::vector<std::uint8_t>& Frame)
{
	return IsTelemetryLinkConnected()
		&& Transport
		&& Transport->GetState() == dspgcs::TransportState::Running
		&& Transport->Send({ Frame });
}

EDSPGCSTelemetryLinkState
UDSPGCSProductionTelemetryProviderSubsystem::GetTelemetryLinkState() const
{
	return GetTelemetryLinkStateAt(FPlatformTime::Seconds());
}

bool UDSPGCSProductionTelemetryProviderSubsystem::IsTelemetryLinkConnected() const
{
	return GetTelemetryLinkState() == EDSPGCSTelemetryLinkState::Connected;
}

TArray<FMDHKTelemetryTableRow>
UDSPGCSProductionTelemetryProviderSubsystem::GetMDHKTelemetryRows() const
{
	FScopeLock Lock(&MDHKTelemetryRowsMutex);
	return MDHKTelemetryRows;
}

void UDSPGCSProductionTelemetryProviderSubsystem::SetCommandResultHandler(
	FCommandResultHandler Handler)
{
	FScopeLock Lock(&CommandResultHandlerMutex);
	CommandResultHandler = MoveTemp(Handler);
}

void UDSPGCSProductionTelemetryProviderSubsystem::SetFrozenAssignmentAckHandler(
	FFrozenAssignmentAckHandler Handler)
{
	FScopeLock Lock(&FrozenAssignmentAckHandlerMutex);
	FrozenAssignmentAckHandler = MoveTemp(Handler);
}

void UDSPGCSProductionTelemetryProviderSubsystem::SetUploadJobFrameHandler(
	FUploadJobFrameHandler Handler)
{
	FScopeLock Lock(&UploadJobFrameHandlerMutex);
	UploadJobFrameHandler = MoveTemp(Handler);
}

bool UDSPGCSProductionTelemetryProviderSubsystem::CreateTransport()
{
    dspgcs::DSPGCSTransportConfig Config =
        dspgcs::MakeUnrealDSPGCSDefaults();
	FString TransportTypeString = TEXT("Loopback");
	FString ConsoleAddressString =
		UTF8_TO_TCHAR(dspgcs::DSPGCSDefaults::LoopbackAddress);
	FString UnrealAddressString = ConsoleAddressString;
	int32 ConsolePort = dspgcs::DSPGCSDefaults::ConsoleBindPort;
	int32 UnrealPort = dspgcs::DSPGCSDefaults::UnrealBindPort;
	int32 ReceiveBufferBytes = Config.receiveBufferBytes;
	int32 ReceiverPollMs = Config.receiverPollMs;
	int32 ConfiguredTelemetryTimeoutMs = TelemetryTimeoutMs;
	GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("TransportType"), TransportTypeString, GGameIni);
	if (!GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("ConsoleAddress"), ConsoleAddressString, GGameIni))
	{
		GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("RemoteAddress"), ConsoleAddressString, GGameIni);
	}
	if (!GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("UnrealAddress"), UnrealAddressString, GGameIni))
	{
		GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("BindAddress"), UnrealAddressString, GGameIni);
	}
	if (!GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("ConsolePort"), ConsolePort, GGameIni))
	{
		GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("RemotePort"), ConsolePort, GGameIni);
	}
	if (!GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("UnrealPort"), UnrealPort, GGameIni))
	{
		GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("BindPort"), UnrealPort, GGameIni);
	}
	GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("ReceiveBufferBytes"), ReceiveBufferBytes, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("ReceiverPollMs"), ReceiverPollMs, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("TelemetryTimeoutMs"), ConfiguredTelemetryTimeoutMs, GGameIni);

	if (!dspgcs::TryParseTransportType(TCHAR_TO_UTF8(*TransportTypeString), Config.type)
		|| ConsoleAddressString.IsEmpty() || UnrealAddressString.IsEmpty()
		|| ConsolePort <= 0 || ConsolePort > 65535
		|| UnrealPort <= 0 || UnrealPort > 65535
		|| ReceiveBufferBytes < dspgcs::DSPGCSDefaults::MinimumSocketBufferBytes
		|| ReceiveBufferBytes > dspgcs::DSPGCSDefaults::MaximumSocketBufferBytes
		|| ReceiverPollMs < dspgcs::DSPGCSDefaults::MinimumReceiverPollMs
		|| ReceiverPollMs > dspgcs::DSPGCSDefaults::MaximumReceiverPollMs
		|| ConfiguredTelemetryTimeoutMs < MinimumTelemetryTimeoutMs
		|| ConfiguredTelemetryTimeoutMs > MaximumTelemetryTimeoutMs)
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production telemetry configuration is invalid."));
		return false;
	}
	Config.bindAddress = TCHAR_TO_UTF8(*UnrealAddressString);
	Config.bindPort = static_cast<uint16>(UnrealPort);
	Config.remoteAddress = TCHAR_TO_UTF8(*ConsoleAddressString);
	Config.remotePort = static_cast<uint16>(ConsolePort);
	Config.receiveBufferBytes = ReceiveBufferBytes;
	Config.receiverPollMs = ReceiverPollMs;
	TelemetryTimeoutMs = ConfiguredTelemetryTimeoutMs;

	std::string FactoryError;
	Transport = dspgcs::DSPGCSTransportFactory::CreateTransport(Config, FactoryError);
	if (!Transport)
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production factory error: %s"), UTF8_TO_TCHAR(FactoryError.c_str()));
		return false;
	}

	const TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> WeakThis(this);
	Transport->Receive([WeakThis](dspgcs::TransportFrame Frame)
	{
		if (UDSPGCSProductionTelemetryProviderSubsystem* Self = WeakThis.Get())
		{
			Self->HandleTransportFrame(MoveTemp(Frame));
		}
	});
	if (!Transport->Start())
	{
		Transport.reset();
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production telemetry transport failed to start."));
		return false;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[DSPGCS Config] Transport=%s LocalEndpoint(UE)=%s:%d RemoteEndpoint(Console)=%s:%d ReceiveBuffer=%d ReceiverPoll=%dms TelemetryTimeout=%dms"),
		*TransportTypeString,
		*UnrealAddressString,
		UnrealPort,
		*ConsoleAddressString,
		ConsolePort,
		ReceiveBufferBytes,
		ReceiverPollMs,
		TelemetryTimeoutMs);
	return true;
}

void UDSPGCSProductionTelemetryProviderSubsystem::HandleTransportFrame(
	dspgcs::TransportFrame Frame)
{
	dspgcs::MessageEnvelope Envelope;
	std::vector<std::uint8_t> Payload;
	if (!dspgcs::DecodeEnvelope(Frame.bytes, Envelope, Payload))
	{
		return;
	}

	if (Envelope.messageType == dspgcs::MessageType::CommandResultBatch)
	{
		FCommandResultHandler Handler;
		{
			FScopeLock Lock(&CommandResultHandlerMutex);
			Handler = CommandResultHandler;
		}
		if (Handler)
		{
			Handler(Envelope, Payload);
		}
		return;
	}

	if (Envelope.messageType == dspgcs::FrozenAssignmentMessageTypes::Ack)
	{
		dspgcs::FrozenAssignmentAckDTO Ack;
		std::string DecodeError;
		if (!dspgcs::DecodeFrozenAssignmentAck(Payload, Ack, DecodeError))
		{
			return;
		}

		FFrozenAssignmentAckHandler Handler;
		{
			FScopeLock Lock(&FrozenAssignmentAckHandlerMutex);
			Handler = FrozenAssignmentAckHandler;
		}
		if (Handler)
		{
			Handler(Envelope, Ack);
		}
		return;
	}

	if (Envelope.messageType == dspgcs::UploadJobMessageTypes::CommandAck ||
		Envelope.messageType == dspgcs::UploadJobMessageTypes::Snapshot ||
		Envelope.messageType == dspgcs::UploadJobMessageTypes::RouteStatePage ||
		Envelope.messageType == dspgcs::UploadJobMessageTypes::RouteStateDelta ||
		Envelope.messageType == dspgcs::MavFtpRuntimeConfigMessageTypes::Ack ||
		Envelope.messageType == dspgcs::MavFtpRuntimeConfigMessageTypes::Snapshot)
	{
		FUploadJobFrameHandler Handler;
		{
			FScopeLock Lock(&UploadJobFrameHandlerMutex);
			Handler = UploadJobFrameHandler;
		}
		if (Handler)
		{
			Handler(Envelope, Payload);
		}
		return;
	}

	if (Envelope.messageType != dspgcs::MessageType::TelemetryBatch)
	{
		return;
	}

	dspgcs::TelemetryBatchDTO Batch;
	if (!dspgcs::DecodeTelemetry(Payload, Batch))
	{
		return;
	}

	dspgcs::TelemetryBatchDTO CompleteBatch;
	if (!TryAssembleTelemetrySnapshot(
		Envelope,
		MoveTemp(Batch),
		CompleteBatch))
	{
		return;
	}

	UGCSServerAdapterSubsystem* Adapter = AdapterRef.Get();
	if (!Adapter)
	{
		return;
	}

	FGCSFleetSnapshot Snapshot;
	Snapshot.Header.SchemaVersion = GCSDataSchema::CurrentVersion;
	Snapshot.Header.SourceID = FromProductionProtocolString(Envelope.sourceId);
	Snapshot.Header.SessionID = FromProductionProtocolString(Envelope.sessionId);
	Snapshot.Header.Sequence = static_cast<int64>(Envelope.sequence);
	Snapshot.Header.SourceTimestamp = FromProductionUnixMilliseconds(Envelope.timestampUtcMs);
	Snapshot.FleetRevision = static_cast<int64>(CompleteBatch.fleetRevision);
	Snapshot.Drones.Reserve(static_cast<int32>(CompleteBatch.items.size()));
	TArray<FMDHKTelemetryTableRow> NewMDHKRows;
	NewMDHKRows.Reserve(static_cast<int32>(CompleteBatch.items.size()));
	for (size_t Index = 0; Index < CompleteBatch.items.size(); ++Index)
	{
		const dspgcs::TelemetryDTO& Source = CompleteBatch.items[Index];
		Snapshot.Drones.Add(GCSVehicleStateProjection::FromTransport(Source, Index == 0));
		if (Source.mdhkStatus.received)
		{
			FMDHKTelemetryTableRow& Row = NewMDHKRows.AddDefaulted_GetRef();
			Row.DroneID = FromProductionProtocolString(Source.droneId);
			Row.bReceived = true;
			Row.SampleTimestampUtcMs = Source.mdhkStatus.sampleTimestampUtcMs;
			Row.BatteryRemainingPercent = Source.mdhkStatus.batteryRemainingPercent;
			Row.bBatteryRemainingValid = Source.mdhkStatus.batteryRemainingValid;
			Row.BatteryTemperatureCelsius = Source.mdhkStatus.batteryTemperatureCelsius;
			Row.bBatteryTemperatureValid = Source.mdhkStatus.batteryTemperatureValid;
			Row.Cell1Volts = Source.mdhkStatus.cell1Volts;
			Row.bCell1Valid = Source.mdhkStatus.cell1Valid;
			Row.Cell2Volts = Source.mdhkStatus.cell2Volts;
			Row.bCell2Valid = Source.mdhkStatus.cell2Valid;
			Row.Cell3Volts = Source.mdhkStatus.cell3Volts;
			Row.bCell3Valid = Source.mdhkStatus.cell3Valid;
			Row.Cell4Volts = Source.mdhkStatus.cell4Volts;
			Row.bCell4Valid = Source.mdhkStatus.cell4Valid;
			Row.LatitudeDeg = Source.mdhkStatus.latitudeDeg;
			Row.LongitudeDeg = Source.mdhkStatus.longitudeDeg;
			Row.bGPSValid = Source.mdhkStatus.gpsValid;
			Row.AltitudeAmslMeters = Source.mdhkStatus.gpsAltitudeAmslMeters;
			Row.bAltitudeValid = Source.mdhkStatus.altitudeValid;
			Row.ValidFlags = static_cast<int64>(Source.mdhkStatus.validFlags);
			Row.PayloadSchemaVersion = Source.mdhkStatus.schemaVersion;
			Row.TimeBootMs = static_cast<int64>(Source.mdhkStatus.timeBootMs);
			Row.GPSUtc = FormatMDHKGPSUtc(Source.mdhkStatus);
			Row.ValidFlagsText = FString::Printf(
				TEXT("0x%08X"), Source.mdhkStatus.validFlags);
			Row.RelativeAltitudeMeters = Source.mdhkStatus.relativeAltitudeMeters;
			Row.FreeMemoryBytes = static_cast<int64>(Source.mdhkStatus.freeMemoryBytes);
			Row.FlightSoftwareVersionText = FormatMDHKFlightSoftwareVersion(
				Source.mdhkStatus.flightSoftwareVersion);
			Row.WaypointDistanceMeters = Source.mdhkStatus.waypointDistanceMeters;
			Row.CompassVariance = Source.mdhkStatus.compassVariance;
			Row.MagneticField1Mgauss = Source.mdhkStatus.magneticField1Mgauss;
			Row.MagneticField2Mgauss = Source.mdhkStatus.magneticField2Mgauss;
			Row.BatteryCycleCount = Source.mdhkStatus.batteryCycleCount
				== std::numeric_limits<std::uint16_t>::max()
				? -1 : static_cast<int32>(Source.mdhkStatus.batteryCycleCount);
			Row.CellVoltageDeltaMv = Source.mdhkStatus.cellVoltageDeltaMv
				== std::numeric_limits<std::uint16_t>::max()
				? -1 : static_cast<int32>(Source.mdhkStatus.cellVoltageDeltaMv);
			Row.EKFFlagsText = FString::Printf(
				TEXT("0x%04X"), Source.mdhkStatus.ekfFlags);
			Row.MissionSequence = Source.mdhkStatus.missionSequence;
			Row.MissionTotal = Source.mdhkStatus.missionTotal;
			Row.BatteryCurrentAmps = Source.mdhkStatus.batteryCurrentCentiamps == -1
				? -1.0
				: static_cast<double>(Source.mdhkStatus.batteryCurrentCentiamps) / 100.0;
			Row.GPSFixType = Source.mdhkStatus.gpsFixType;
			Row.SatellitesVisible = Source.mdhkStatus.satellitesVisible
				== std::numeric_limits<std::uint8_t>::max()
				? -1 : static_cast<int32>(Source.mdhkStatus.satellitesVisible);
			Row.RGBText = FString::Printf(
				TEXT("R=%u G=%u B=%u"),
				static_cast<uint32>(Source.mdhkStatus.red),
				static_cast<uint32>(Source.mdhkStatus.green),
				static_cast<uint32>(Source.mdhkStatus.blue));
			Row.FlightCustomVersionText = FormatMDHKCustomVersion(
				Source.mdhkStatus.flightCustomVersion);
			Row.HardwareID = FromProductionProtocolString(Source.mdhkStatus.hardwareId);
		}
	}
	{
		FScopeLock Lock(&MDHKTelemetryRowsMutex);
		MDHKTelemetryRows = MoveTemp(NewMDHKRows);
	}

	if (Adapter->PushFleetSnapshot(Snapshot))
	{
		TArray<FRouteGpsTelemetryObservation> Observations;
		const FDateTime ReceivedUtc = FDateTime::UtcNow();
		const int64 ReceivedUtcMs = ReceivedUtc.ToUnixTimestamp() * 1000
			+ ReceivedUtc.GetMillisecond();
		const double ReceivedMonotonicSeconds = FPlatformTime::Seconds();
		const int32 Count = FMath::Min(
			Snapshot.Drones.Num(), static_cast<int32>(CompleteBatch.items.size()));
		Observations.Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FDroneData& Drone = Snapshot.Drones[Index];
			const dspgcs::TelemetryDTO& Source = CompleteBatch.items[Index];
			FRouteGpsTelemetryObservation& Observation = Observations.AddDefaulted_GetRef();
			Observation.DroneID = Drone.DroneID;
			Observation.EndpointIdentity = Drone.EndpointIdentity;
			Observation.bVehicleOnline = Drone.bOnline;
			Observation.bGpsValid = Drone.bGPSValid;
			Observation.FixType = Drone.GPSFixType;
			Observation.Latitude = Drone.GPSLat;
			Observation.Longitude = Drone.GPSLon;
			Observation.AltitudeAMSL = Drone.AltitudeAMSL;
			Observation.bAltitudeValid = Drone.bAltitudeValid;
			Observation.GpsSampleTimestampUtcMs = Source.sampleTimestampUtcMs;
			Observation.TelemetryReceivedUtcMs = ReceivedUtcMs;
			Observation.TelemetryReceivedMonotonicSeconds = ReceivedMonotonicSeconds;
		}
		const TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Observations = MoveTemp(Observations)]()
			{
				UDSPGCSProductionTelemetryProviderSubsystem* Self = WeakThis.Get();
				UGameInstance* GameInstance = Self ? Self->GetGameInstance() : nullptr;
				URouteGpsCaptureSubsystem* Capture = GameInstance
					? GameInstance->GetSubsystem<URouteGpsCaptureSubsystem>() : nullptr;
				if (!Capture) return;
				for (const FRouteGpsTelemetryObservation& Observation : Observations)
				{
					Capture->ObserveTelemetrySample(Observation);
				}
			});
		ReceivedDroneCount.store(Snapshot.Drones.Num());
		MarkValidTelemetryReceivedAt(FPlatformTime::Seconds());
		UE_LOG(
			LogTemp,
			VeryVerbose,
			TEXT("DSP-GCS production telemetry accepted drones=%d revision=%lld"),
			Snapshot.Drones.Num(),
			Snapshot.FleetRevision);
	}
}

bool UDSPGCSProductionTelemetryProviderSubsystem::TryAssembleTelemetrySnapshot(
	const dspgcs::MessageEnvelope& Envelope,
	dspgcs::TelemetryBatchDTO&& Chunk,
	dspgcs::TelemetryBatchDTO& CompleteBatch)
{
	if (Chunk.chunkCount == 0 ||
		Chunk.chunkCount > dspgcs::MaxSnapshotChunks ||
		Chunk.chunkIndex >= Chunk.chunkCount)
	{
		return false;
	}

	if (Chunk.chunkCount == 1)
	{
		if (Chunk.chunkIndex != 0)
		{
			return false;
		}
		ResetPendingTelemetrySnapshot();
		CompleteBatch = MoveTemp(Chunk);
		return true;
	}

	if (Chunk.batchMode != dspgcs::BatchMode::Snapshot ||
		Chunk.snapshotId.empty())
	{
		return false;
	}

	// SessionID + SnapshotID + FleetRevision 共同标识一个原子 Snapshot。
	// 任一维度变化都放弃未完成旧批次，禁止把不同 Snapshot 的分片拼在一起。
	const bool bStartsNewSnapshot =
		PendingSnapshotSessionID != Envelope.sessionId ||
		PendingSnapshotID != Chunk.snapshotId ||
		PendingSnapshotFleetRevision != Chunk.fleetRevision ||
		PendingSnapshotChunks.size() != Chunk.chunkCount;
	if (bStartsNewSnapshot)
	{
		ResetPendingTelemetrySnapshot();
		PendingSnapshotSessionID = Envelope.sessionId;
		PendingSnapshotID = Chunk.snapshotId;
		PendingSnapshotFleetRevision = Chunk.fleetRevision;
		PendingSnapshotChunks.resize(Chunk.chunkCount);
		PendingSnapshotChunksReceived.resize(Chunk.chunkCount, false);
	}

	const std::size_t ChunkIndex = static_cast<std::size_t>(Chunk.chunkIndex);
	if (!PendingSnapshotChunksReceived[ChunkIndex])
	{
		PendingSnapshotChunks[ChunkIndex] = MoveTemp(Chunk.items);
		PendingSnapshotChunksReceived[ChunkIndex] = true;
		++PendingSnapshotReceivedCount;
	}

	// 分片未收齐时只保存在 Provider 内部，绝不向 Adapter/FleetManager 提交半个 Fleet。
	if (PendingSnapshotReceivedCount != PendingSnapshotChunks.size())
	{
		return false;
	}

	// 只有全部分片齐备后才合成一个完整 Batch；调用方随后只进行一次 GameThread 原子提交。
	CompleteBatch = MoveTemp(Chunk);
	CompleteBatch.chunkIndex = 0;
	CompleteBatch.chunkCount = 1;
	CompleteBatch.items.clear();
	std::size_t TotalItems = 0;
	for (const auto& Items : PendingSnapshotChunks)
	{
		TotalItems += Items.size();
	}
	CompleteBatch.items.reserve(TotalItems);
	for (auto& Items : PendingSnapshotChunks)
	{
		CompleteBatch.items.insert(
			CompleteBatch.items.end(),
			std::make_move_iterator(Items.begin()),
			std::make_move_iterator(Items.end()));
	}
	ResetPendingTelemetrySnapshot();
	return true;
}

void UDSPGCSProductionTelemetryProviderSubsystem::ResetPendingTelemetrySnapshot()
{
	PendingSnapshotSessionID.clear();
	PendingSnapshotID.clear();
	PendingSnapshotFleetRevision = 0;
	PendingSnapshotChunks.clear();
	PendingSnapshotChunksReceived.clear();
	PendingSnapshotReceivedCount = 0;
}

void UDSPGCSProductionTelemetryProviderSubsystem::StartTelemetryHealthMonitor()
{
	StopTelemetryHealthMonitor();
	LastValidTelemetryReceiveTimeSeconds.store(0.0);
	SetTelemetryLinkState(EDSPGCSTelemetryLinkState::Disconnected);
	TelemetryHealthTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this,
			&UDSPGCSProductionTelemetryProviderSubsystem::TickTelemetryHealth),
		TelemetryHealthCheckIntervalSeconds);
}

void UDSPGCSProductionTelemetryProviderSubsystem::StopTelemetryHealthMonitor()
{
	if (TelemetryHealthTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TelemetryHealthTickerHandle);
		TelemetryHealthTickerHandle.Reset();
	}
}

bool UDSPGCSProductionTelemetryProviderSubsystem::TickTelemetryHealth(
	const float DeltaSeconds)
{
	(void)DeltaSeconds;
	EvaluateTelemetryLinkHealthAt(FPlatformTime::Seconds());
	return true;
}

void UDSPGCSProductionTelemetryProviderSubsystem::MarkValidTelemetryReceivedAt(
	const double ReceiveTimeSeconds)
{
	LastValidTelemetryReceiveTimeSeconds.store(ReceiveTimeSeconds);
	if (IsInGameThread())
	{
		SetTelemetryLinkState(EDSPGCSTelemetryLinkState::Connected);
	}
}

void UDSPGCSProductionTelemetryProviderSubsystem::EvaluateTelemetryLinkHealthAt(
	const double CurrentTimeSeconds)
{
	SetTelemetryLinkState(GetTelemetryLinkStateAt(CurrentTimeSeconds));
}

EDSPGCSTelemetryLinkState
UDSPGCSProductionTelemetryProviderSubsystem::GetTelemetryLinkStateAt(
	const double CurrentTimeSeconds) const
{
	const double LastReceiveTimeSeconds =
		LastValidTelemetryReceiveTimeSeconds.load();
	if (LastReceiveTimeSeconds <= 0.0)
	{
		return EDSPGCSTelemetryLinkState::Disconnected;
	}

	const double ElapsedMilliseconds =
		(CurrentTimeSeconds - LastReceiveTimeSeconds) * 1000.0;
	return ElapsedMilliseconds <= static_cast<double>(TelemetryTimeoutMs)
		? EDSPGCSTelemetryLinkState::Connected
		: EDSPGCSTelemetryLinkState::Disconnected;
}

void UDSPGCSProductionTelemetryProviderSubsystem::SetTelemetryLinkState(
	const EDSPGCSTelemetryLinkState NewState)
{
	check(IsInGameThread());
	if (TelemetryLinkState == NewState)
	{
		return;
	}

	TelemetryLinkState = NewState;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("DSP-GCS telemetry link state=%s"),
		TelemetryLinkState == EDSPGCSTelemetryLinkState::Connected
			? TEXT("Connected")
			: TEXT("Disconnected"));
	OnTelemetryLinkStateChanged.Broadcast();
}
