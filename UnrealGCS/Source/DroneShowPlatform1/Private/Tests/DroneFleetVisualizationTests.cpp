#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CesiumGeoreference.h"
#include "DroneFleetVisualizationActor.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FleetManagerSubsystem.h"
#include "GCSDataTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDroneFleetVisualizationBridgeTest,
	"DroneGCS.Visualization.FleetNiagaraBridge.CountOrderAndOfflineStability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	FGCSFleetSnapshot MakeSnapshot(const int32 DroneCount, const int64 Revision)
	{
		FGCSFleetSnapshot Snapshot;
		Snapshot.Header.SessionID = TEXT("FLEET-VIZ-TEST");
		Snapshot.FleetRevision = Revision;
		Snapshot.Drones.Reserve(DroneCount);
		for (int32 Index = 1; Index <= DroneCount; ++Index)
		{
			FDroneData Drone;
			Drone.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
			Drone.bOnline = true;
			Snapshot.Drones.Add(MoveTemp(Drone));
		}
		return Snapshot;
	}

	FDroneData MakeGPSDrone(
		const int32 DroneNumber,
		const double Latitude,
		const double Longitude,
		const bool bGPSValid,
		const bool bAltitudeValid = true)
	{
		FDroneData Drone;
		Drone.DroneID = FString::Printf(TEXT("Drone%03d"), DroneNumber);
		Drone.bOnline = true;
		Drone.GPSLat = Latitude;
		Drone.GPSLon = Longitude;
		Drone.bGPSValid = bGPSValid;
		Drone.GPSFixType = bGPSValid ? DroneGPS::MinimumValidFixType : 0;
		Drone.AltitudeAMSL = 30.0f + static_cast<float>(DroneNumber) * 0.1f;
		Drone.bAltitudeValid = bAltitudeValid;
		return Drone;
	}

	FGCSFleetSnapshot MakeGPSSnapshot(const int32 DroneCount, const int64 Revision)
	{
		FGCSFleetSnapshot Snapshot;
		Snapshot.Header.SessionID = TEXT("FLEET-VIZ-TEST");
		Snapshot.FleetRevision = Revision;
		Snapshot.Drones.Reserve(DroneCount);
		for (int32 Index = 1; Index <= DroneCount; ++Index)
		{
			Snapshot.Drones.Add(MakeGPSDrone(
				Index,
				31.932471 + static_cast<double>(Index - 1) * 0.00001,
				118.638531 + static_cast<double>(Index - 1) * 0.00001,
				true));
		}
		return Snapshot;
	}
}

bool FDroneFleetVisualizationBridgeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}

	GameInstance->InitializeStandalone(TEXT("DroneFleetVisualizationTestWorld"));
	UWorld* World = GameInstance->GetWorld();
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	ACesiumGeoreference* Georeference = World
		? World->SpawnActor<ACesiumGeoreference>()
		: nullptr;
	ADroneFleetVisualizationActor* Visualization = World
		? World->SpawnActor<ADroneFleetVisualizationActor>()
		: nullptr;
	if (!TestNotNull(TEXT("Test world created"), World)
		|| !TestNotNull(TEXT("FleetManager initialized"), Fleet)
		|| !TestNotNull(TEXT("CesiumGeoreference spawned"), Georeference)
		|| !TestNotNull(TEXT("Visualization actor spawned"), Visualization))
	{
		GameInstance->Shutdown();
		return false;
	}

	TestTrue(TEXT("Zero-drone snapshot applied"), Fleet->ApplySnapshot(MakeSnapshot(0, 1)));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Zero drones produce zero positions"), Visualization->GetVisualizationDroneCount(), 0);

	TestTrue(TEXT("One-drone snapshot applied"), Fleet->ApplySnapshot(MakeSnapshot(1, 2)));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("One drone produces one position"), Visualization->GetVisualizationDroneCount(), 1);
	TestEqual(TEXT("Particle zero maps to Drone001"), Visualization->GetParticleDroneIDs()[0], FString(TEXT("Drone001")));
	TestEqual(TEXT("Default label uses numeric DroneID without subtracting one"),
		Visualization->GetDroneDisplayIDsForTest()[0], 1.0f);
	TestEqual(TEXT("Phase 3 position is UE origin"), Visualization->GetDronePositions()[0], FVector::ZeroVector);

	TestTrue(TEXT("100-drone snapshot applied"), Fleet->ApplySnapshot(MakeSnapshot(100, 3)));
	Visualization->RefreshFromFleet();
	const TArray<FString> InitialIDs = Visualization->GetParticleDroneIDs();
	TestEqual(TEXT("100 drones produce 100 positions"), Visualization->GetVisualizationDroneCount(), 100);
	TestEqual(TEXT("Stable first particle"), InitialIDs[0], FString(TEXT("Drone001")));
	TestEqual(TEXT("Stable middle particle"), InitialIDs[49], FString(TEXT("Drone050")));
	TestEqual(TEXT("Stable final particle"), InitialIDs[99], FString(TEXT("Drone100")));
	TMap<FString, int32> ConfirmedRouteSlots;
	ConfirmedRouteSlots.Add(TEXT("Drone005"), 37);
	Visualization->SetConfirmedRouteSlotDisplayOverrides(ConfirmedRouteSlots);
	const TArray<float> ConfirmedDisplayIDs = Visualization->GetDroneDisplayIDsForTest();
	TestEqual(TEXT("Confirmed RouteSlot is displayed without changing particle identity"),
		ConfirmedDisplayIDs[4], 37.0f);
	TestEqual(TEXT("Drone005 network identity remains unchanged"),
		Visualization->GetParticleDroneIDs()[4], FString(TEXT("Drone005")));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Confirmed display survives Fleet refresh"),
		Visualization->GetDroneDisplayIDsForTest()[4], 37.0f);
	Visualization->ClearConfirmedRouteSlotDisplayOverrides();
	TestEqual(TEXT("Clear restores direct numeric DroneID display"),
		Visualization->GetDroneDisplayIDsForTest()[4], 5.0f);
	TestEqual(TEXT("Clear still preserves Drone005 identity"),
		Visualization->GetParticleDroneIDs()[4], FString(TEXT("Drone005")));
	for (const FVector& Position : Visualization->GetDronePositions())
	{
		TestEqual(TEXT("All Phase 3 positions remain at UE origin"), Position, FVector::ZeroVector);
	}

	FGCSTelemetryBatch OfflineBatch;
	OfflineBatch.FleetRevision = 4;
	FDroneDataDelta OfflineDelta;
	OfflineDelta.DroneID = TEXT("Drone050");
	OfflineDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
	OfflineDelta.Data.bOnline = false;
	OfflineBatch.Deltas.Add(OfflineDelta);
	TestTrue(TEXT("Offline delta applied"), Fleet->ApplyDeltaBatch(OfflineBatch));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Offline drone remains a particle"), Visualization->GetVisualizationDroneCount(), 100);
	TestEqual(TEXT("Offline does not change ParticleIndex"), Visualization->GetParticleDroneIDs()[49], FString(TEXT("Drone050")));
	TestTrue(TEXT("Offline does not reorder fleet"), Visualization->GetParticleDroneIDs() == InitialIDs);

	FGCSTelemetryBatch OnlineBatch;
	OnlineBatch.FleetRevision = 5;
	FDroneDataDelta OnlineDelta = OfflineDelta;
	OnlineDelta.Data.bOnline = true;
	OnlineBatch.Deltas.Add(OnlineDelta);
	TestTrue(TEXT("Online recovery delta applied"), Fleet->ApplyDeltaBatch(OnlineBatch));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Recovery keeps particle count"), Visualization->GetVisualizationDroneCount(), 100);
	TestTrue(TEXT("Recovery keeps ParticleIndex mapping"), Visualization->GetParticleDroneIDs() == InitialIDs);

	TestTrue(TEXT("101-drone snapshot applied"), Fleet->ApplySnapshot(MakeSnapshot(101, 6)));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Fleet growth produces 101 positions"), Visualization->GetVisualizationDroneCount(), 101);
	TestEqual(TEXT("Fleet growth preserves existing ParticleIndex"), Visualization->GetParticleDroneIDs()[49], FString(TEXT("Drone050")));
	TestEqual(TEXT("Fleet growth appends Drone101"), Visualization->GetParticleDroneIDs()[100], FString(TEXT("Drone101")));

	FGCSFleetSnapshot WaitingForOriginSnapshot;
	WaitingForOriginSnapshot.Header.SessionID = TEXT("FLEET-VIZ-TEST");
	WaitingForOriginSnapshot.FleetRevision = 7;
	WaitingForOriginSnapshot.Drones.Add(MakeGPSDrone(1, 0.0, 0.0, false));
	WaitingForOriginSnapshot.Drones.Add(MakeGPSDrone(2, 31.932481, 118.638541, true));
	TestTrue(TEXT("Snapshot waiting for Drone001 origin applied"), Fleet->ApplySnapshot(WaitingForOriginSnapshot));
	Visualization->RefreshFromFleet();
	TestFalse(TEXT("Drone002 cannot initialize FleetGeoOrigin"), Fleet->GetFleetGeoOrigin().bOriginInitialized);
	TestFalse(TEXT("Origin world position waits for FleetGeoOrigin"), Visualization->IsOriginWorldPositionInitialized());
	TestEqual(TEXT("Drone001 stays at zero before origin"), Visualization->GetDronePositions()[0], FVector::ZeroVector);
	TestEqual(TEXT("Drone002 stays at zero before origin"), Visualization->GetDronePositions()[1], FVector::ZeroVector);

	TestTrue(TEXT("100 valid GPS snapshot applied"), Fleet->ApplySnapshot(MakeGPSSnapshot(100, 8)));
	Visualization->RefreshFromFleet();
	const FFleetGeoOrigin LockedFleetOrigin = Fleet->GetFleetGeoOrigin();
	const FVector LockedOriginWorldPosition = Visualization->GetOriginWorldPosition();
	const TArray<FVector> InitialGPSPositions = Visualization->GetDronePositions();
	TestTrue(TEXT("Drone001 initializes FleetGeoOrigin"), LockedFleetOrigin.bOriginInitialized);
	TestEqual(TEXT("Drone001 is the fixed origin drone"), LockedFleetOrigin.OriginDroneID, FString(TEXT("Drone001")));
	TestTrue(TEXT("Origin world position initialized"), Visualization->IsOriginWorldPositionInitialized());
	TestEqual(TEXT("100 GPS drones produce 100 positions"), InitialGPSPositions.Num(), 100);
	TestNotEqual(TEXT("Different GPS values produce different Cesium positions"), InitialGPSPositions[0], InitialGPSPositions[1]);
	for (const FVector& Position : InitialGPSPositions)
	{
		TestNotEqual(TEXT("Valid GPS produces non-zero Cesium position"), Position, FVector::ZeroVector);
	}

	FGCSTelemetryBatch MoveOriginDroneBatch;
	MoveOriginDroneBatch.FleetRevision = 9;
	FDroneDataDelta MoveOriginDroneDelta;
	MoveOriginDroneDelta.DroneID = TEXT("Drone001");
	MoveOriginDroneDelta.ChangedFields = static_cast<int32>(
		EGCSDroneDataField::GPS | EGCSDroneDataField::Altitude);
	MoveOriginDroneDelta.Data = MakeGPSDrone(1, 31.933471, 118.639531, true);
	MoveOriginDroneBatch.Deltas.Add(MoveOriginDroneDelta);
	TestTrue(TEXT("Moving Drone001 delta applied"), Fleet->ApplyDeltaBatch(MoveOriginDroneBatch));
	Visualization->RefreshFromFleet();
	TestNotEqual(TEXT("Drone001 particle follows its changed GPS"), Visualization->GetDronePositions()[0], InitialGPSPositions[0]);
	TestEqual(TEXT("FleetGeoOrigin remains locked after Drone001 moves"), Fleet->GetFleetGeoOrigin().OriginLatitude, LockedFleetOrigin.OriginLatitude);
	TestEqual(TEXT("OriginWorldPosition remains locked after Drone001 moves"), Visualization->GetOriginWorldPosition(), LockedOriginWorldPosition);

	FGCSTelemetryBatch InvalidGPSBatch;
	InvalidGPSBatch.FleetRevision = 10;
	FDroneDataDelta InvalidGPSDelta;
	InvalidGPSDelta.DroneID = TEXT("Drone050");
	InvalidGPSDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::GPS);
	InvalidGPSDelta.Data = MakeGPSDrone(50, 0.0, 0.0, false);
	InvalidGPSBatch.Deltas.Add(InvalidGPSDelta);
	TestTrue(TEXT("Drone050 invalid GPS delta applied"), Fleet->ApplyDeltaBatch(InvalidGPSBatch));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Invalid GPS returns Particle49 to zero"), Visualization->GetDronePositions()[49], FVector::ZeroVector);
	TestEqual(TEXT("Invalid GPS keeps Particle49 mapping"), Visualization->GetParticleDroneIDs()[49], FString(TEXT("Drone050")));

	FGCSTelemetryBatch GPSRecoveryBatch;
	GPSRecoveryBatch.FleetRevision = 11;
	FDroneDataDelta GPSRecoveryDelta;
	GPSRecoveryDelta.DroneID = TEXT("Drone050");
	GPSRecoveryDelta.ChangedFields = static_cast<int32>(
		EGCSDroneDataField::GPS | EGCSDroneDataField::Altitude);
	GPSRecoveryDelta.Data = MakeGPSDrone(50, 31.932961, 118.639021, true, false);
	GPSRecoveryBatch.Deltas.Add(GPSRecoveryDelta);
	TestTrue(TEXT("Drone050 GPS recovery applied"), Fleet->ApplyDeltaBatch(GPSRecoveryBatch));
	Visualization->RefreshFromFleet();
	TestNotEqual(TEXT("Recovered GPS restores Particle49 Cesium position"), Visualization->GetDronePositions()[49], FVector::ZeroVector);
	TestEqual(TEXT("GPS recovery keeps Particle49 mapping"), Visualization->GetParticleDroneIDs()[49], FString(TEXT("Drone050")));

	FGCSTelemetryBatch OriginOfflineBatch;
	OriginOfflineBatch.FleetRevision = 12;
	FDroneDataDelta OriginOfflineDelta;
	OriginOfflineDelta.DroneID = TEXT("Drone001");
	OriginOfflineDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
	OriginOfflineDelta.Data.bOnline = false;
	OriginOfflineBatch.Deltas.Add(OriginOfflineDelta);
	TestTrue(TEXT("Drone001 offline delta applied"), Fleet->ApplyDeltaBatch(OriginOfflineBatch));
	Visualization->RefreshFromFleet();
	TestEqual(TEXT("Offline origin drone remains in fleet"), Visualization->GetVisualizationDroneCount(), 100);
	TestEqual(TEXT("Offline origin drone keeps ParticleIndex zero"), Visualization->GetParticleDroneIDs()[0], FString(TEXT("Drone001")));
	TestEqual(TEXT("Offline does not change OriginWorldPosition"), Visualization->GetOriginWorldPosition(), LockedOriginWorldPosition);

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[DRONE_FLEET_VIZ_TEST] cesium=PASS originLocked=PASS invalidGPSZero=PASS gpsRecovery=PASS fleet100=PASS"));

	World->DestroyActor(Visualization);
	GameInstance->Shutdown();
	return true;
}

#endif
