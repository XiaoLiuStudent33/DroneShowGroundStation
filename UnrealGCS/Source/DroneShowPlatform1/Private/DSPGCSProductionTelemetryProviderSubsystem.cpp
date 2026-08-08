#include "DSPGCSProductionTelemetryProviderSubsystem.h"

#include "Async/Async.h"
#include "DSPGCSProtocol.h"
#include "DSPGCSTransportFactory.h"
#include "GCSServerAdapterSubsystem.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeLock.h"

#include <iterator>

namespace
{
	FString FromProtocolString(const std::string& Value)
	{
		return UTF8_TO_TCHAR(Value.c_str());
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
		Target.Battery = Source.battery.valid
			? Source.battery.remainingPercent
			: 0.0f;
		Target.Mode = FromProtocolString(Source.mode.modeName);
		Target.RSSI = Source.rssi.valid
			? static_cast<int32>(Source.rssi.valueDbm)
			: 0;
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
	return CreateTransport();
}

void UDSPGCSProductionTelemetryProviderSubsystem::Stop()
{
	SetCommandResultHandler({});
	if (Transport)
	{
		Transport->Receive({});
		Transport->Stop();
		Transport.reset();
	}
	ResetPendingTelemetrySnapshot();
	AdapterRef.Reset();
}

bool UDSPGCSProductionTelemetryProviderSubsystem::SendFrame(
	const std::vector<std::uint8_t>& Frame)
{
	return Transport
		&& Transport->GetState() == dspgcs::TransportState::Running
		&& Transport->Send({ Frame });
}

void UDSPGCSProductionTelemetryProviderSubsystem::SetCommandResultHandler(
	FCommandResultHandler Handler)
{
	FScopeLock Lock(&CommandResultHandlerMutex);
	CommandResultHandler = MoveTemp(Handler);
}

bool UDSPGCSProductionTelemetryProviderSubsystem::CreateTransport()
{
	FString TransportTypeString = TEXT("Loopback");
	FString BindAddressString = TEXT("127.0.0.1");
	FString RemoteAddressString = TEXT("127.0.0.1");
	int32 BindPort = 12002;
	int32 RemotePort = 12001;
	GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("TransportType"), TransportTypeString, GGameIni);
	GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("BindAddress"), BindAddressString, GGameIni);
	GConfig->GetString(TEXT("DSPGCS.ProductionTelemetry"), TEXT("RemoteAddress"), RemoteAddressString, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("BindPort"), BindPort, GGameIni);
	GConfig->GetInt(TEXT("DSPGCS.ProductionTelemetry"), TEXT("RemotePort"), RemotePort, GGameIni);

	dspgcs::DSPGCSTransportConfig Config;
	if (!dspgcs::TryParseTransportType(TCHAR_TO_UTF8(*TransportTypeString), Config.type)
		|| BindPort <= 0 || BindPort > 65535
		|| RemotePort <= 0 || RemotePort > 65535)
	{
		UE_LOG(LogTemp, Error, TEXT("DSP-GCS production telemetry configuration is invalid."));
		return false;
	}
	Config.bindAddress = TCHAR_TO_UTF8(*BindAddressString);
	Config.bindPort = static_cast<uint16>(BindPort);
	Config.remoteAddress = TCHAR_TO_UTF8(*RemoteAddressString);
	Config.remotePort = static_cast<uint16>(RemotePort);

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
		TEXT("DSP-GCS production channel listening %s:%d"),
		*BindAddressString,
		BindPort);
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

	const TWeakObjectPtr<UDSPGCSProductionTelemetryProviderSubsystem> WeakThis(this);
	AsyncTask(ENamedThreads::GameThread, [WeakThis, Envelope, Batch = MoveTemp(CompleteBatch)]()
	{
		UDSPGCSProductionTelemetryProviderSubsystem* Self = WeakThis.Get();
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
			Self->ReceivedDroneCount = Snapshot.Drones.Num();
			UE_LOG(
				LogTemp,
				VeryVerbose,
				TEXT("DSP-GCS production telemetry applied drones=%d revision=%lld"),
				Snapshot.Drones.Num(),
				Snapshot.FleetRevision);
		}
	});
}

bool UDSPGCSProductionTelemetryProviderSubsystem::TryAssembleTelemetrySnapshot(
	const dspgcs::MessageEnvelope& Envelope,
	dspgcs::TelemetryBatchDTO&& Chunk,
	dspgcs::TelemetryBatchDTO& CompleteBatch)
{
	constexpr std::uint32_t MaxSnapshotChunks = 1024;
	if (Chunk.chunkCount == 0 ||
		Chunk.chunkCount > MaxSnapshotChunks ||
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

	if (PendingSnapshotReceivedCount != PendingSnapshotChunks.size())
	{
		return false;
	}

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
