#include "MockDroneDataSubsystem.h"

#include "GCSServerAdapterSubsystem.h"
#include "Misc/ConfigCacheIni.h"

void UMockDroneDataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UGCSServerAdapterSubsystem>();

	bool bUseMockData = true;
	GConfig->GetBool(
		TEXT("DroneGCS.Data"),
		TEXT("UseMockData"),
		bUseMockData,
		GGameIni);
	if (!bUseMockData)
	{
		return;
	}

	UGCSServerAdapterSubsystem* Adapter =
		GetGameInstance()->GetSubsystem<UGCSServerAdapterSubsystem>();
	if (!Adapter || !Adapter->RegisterProvider(*this))
	{
		UE_LOG(LogTemp, Error, TEXT("Unable to register the mock GCS data provider."));
	}
}

void UMockDroneDataSubsystem::Deinitialize()
{
	if (UGCSServerAdapterSubsystem* Adapter = AdapterRef.Get())
	{
		Adapter->UnregisterProvider(*this);
	}
	Stop();
	Super::Deinitialize();
}

FName UMockDroneDataSubsystem::GetProviderID() const
{
	return TEXT("MockDroneData");
}

bool UMockDroneDataSubsystem::Start(UGCSServerAdapterSubsystem& Adapter)
{
	AdapterRef = &Adapter;

	FGCSFleetSnapshot Snapshot;
	Snapshot.Header.SchemaVersion = GCSDataSchema::CurrentVersion;
	Snapshot.Header.SourceID = GetProviderID().ToString();
	Snapshot.Header.SessionID = TEXT("LOCAL-MOCK-SESSION");
	Snapshot.Header.Sequence = 1;
	Snapshot.Header.SourceTimestamp = FDateTime::UtcNow();
	Snapshot.FleetRevision = 1;

	struct FMockSeed
	{
		const TCHAR* DroneID;
		int32 SysID;
		double Latitude;
		double Longitude;
		float Altitude;
		float Battery;
		const TCHAR* Mode;
		int32 RSSI;
		FLinearColor LEDColor;
	};

	const FMockSeed Seeds[] =
	{
		{ TEXT("DRONE 0001"), 1, 31.932471, 118.638531, 0.0f, 98.0f, TEXT("READY"), -42, FLinearColor(0.0f, 0.8f, 1.0f) },
		{ TEXT("DRONE 0002"), 2, 31.932561, 118.638621, 0.0f, 96.0f, TEXT("LOITER"), -45, FLinearColor::Red },
		{ TEXT("DRONE 0003"), 3, 31.932651, 118.638711, 0.0f, 94.0f, TEXT("STANDBY"), -47, FLinearColor::Green },
		{ TEXT("DRONE 0004"), 4, 31.932741, 118.638801, 0.0f, 91.0f, TEXT("READY"), -50, FLinearColor::Blue },
		{ TEXT("DRONE 0005"), 5, 31.932831, 118.638891, 0.0f, 88.0f, TEXT("READY"), -53, FLinearColor::Yellow }
	};

	for (const FMockSeed& Seed : Seeds)
	{
		FDroneData DroneData;
		DroneData.DroneID = Seed.DroneID;
		DroneData.SysID = Seed.SysID;
		DroneData.GPSLat = Seed.Latitude;
		DroneData.GPSLon = Seed.Longitude;
		DroneData.Altitude = Seed.Altitude;
		DroneData.Battery = Seed.Battery;
		DroneData.Mode = Seed.Mode;
		DroneData.RSSI = Seed.RSSI;
		DroneData.bOnline = true;
		DroneData.LEDColor = Seed.LEDColor;
		DroneData.bSelected = Seed.SysID == 1;
		Snapshot.Drones.Add(MoveTemp(DroneData));
	}

	return Adapter.PushFleetSnapshot(Snapshot);
}

void UMockDroneDataSubsystem::Stop()
{
	AdapterRef.Reset();
}
