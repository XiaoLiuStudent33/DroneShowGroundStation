#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "CesiumGeoreference.h"
#include "Components/SceneComponent.h"
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
	TestEqual(
		TEXT("The locked origin drone is rebased to the visualization anchor"),
		InitialGPSPositions[0],
		Visualization->GetActorLocation());
	TArray<FVector3f> OverlayENUPositions;
	OverlayENUPositions.Add(FVector3f::ZeroVector);
	OverlayENUPositions.Add(FVector3f(1.0f, 0.0f, 0.0f));
	TArray<FVector> OverlayLiveWorldPositions;
	FVector OverlayLiveOriginWorld = FVector::ZeroVector;
	FString OverlayConversionError;
	// The stable ShowOrigin capture and the latest fixed-pile telemetry are two
	// samples of the same drone, so allow a small real-world sampling delta. ENU
	// zero must still be exactly the fixed-pile display anchor.
	const double CapturedShowOriginLatitude =
		LockedFleetOrigin.OriginLatitude + 0.00000005;
	const double CapturedShowOriginLongitude =
		LockedFleetOrigin.OriginLongitude - 0.00000005;
	const double CapturedShowOriginAltitude =
		LockedFleetOrigin.OriginAltitudeAMSL + 0.02;
	TestTrue(
		TEXT("Route overlay converts through the Live display coordinate space"),
		Visualization->ConvertRouteENUPositionsToLiveWorld(
			OverlayENUPositions,
			CapturedShowOriginLatitude,
			CapturedShowOriginLongitude,
			CapturedShowOriginAltitude,
			OverlayLiveWorldPositions,
			OverlayLiveOriginWorld,
			OverlayConversionError));
	TestTrue(
		TEXT("Route overlay ShowOrigin matches the Live visualization anchor"),
		OverlayLiveOriginWorld.Equals(Visualization->GetActorLocation(), 0.01));
	TestTrue(
		TEXT("Route overlay ENU zero matches the fixed-pile display anchor"),
		OverlayLiveWorldPositions.IsValidIndex(0)
			&& OverlayLiveWorldPositions[0].Equals(
				Visualization->GetActorLocation(), 0.01));
	TestEqual(
		TEXT("Route overlay conversion preserves its dynamic target count"),
		OverlayLiveWorldPositions.Num(),
		OverlayENUPositions.Num());
	for (int32 Index = 1; Index < InitialGPSPositions.Num(); ++Index)
	{
		TestNotEqual(
			TEXT("Other valid GPS drones retain non-zero relative offsets"),
			InitialGPSPositions[Index],
			Visualization->GetActorLocation());
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

	const TArray<FVector> PositionsBeforeFixedPile = Visualization->GetDronePositions();
	FString FixedPileError;
	TestFalse(
		TEXT("An offline drone cannot become the fixed pile"),
		Fleet->SetGeoOriginFromDroneID(TEXT("Drone001"), FixedPileError));
	TestTrue(TEXT("Offline fixed-pile failure explains the cause"), FixedPileError.Contains(TEXT("离线")));
	TestFalse(
		TEXT("An unknown drone cannot become the fixed pile"),
		Fleet->SetGeoOriginFromDroneID(TEXT("Drone999"), FixedPileError));
	TestTrue(TEXT("Unknown fixed-pile failure explains the cause"), FixedPileError.Contains(TEXT("不存在")));
	TestTrue(
		TEXT("A valid online drone can become the fixed pile"),
		Fleet->SetGeoOriginFromDroneID(TEXT("Drone050"), FixedPileError));
	Visualization->SetFixedPileDroneID(TEXT("Drone050"));
	Visualization->RefreshFromFleet();
	TestEqual(
		TEXT("Fixed pile becomes the authoritative fleet origin"),
		Fleet->GetFleetGeoOrigin().OriginDroneID,
		FString(TEXT("Drone050")));
	TestEqual(
		TEXT("Fixed-pile particle is rebased exactly to the coordinate origin"),
		Visualization->GetDronePositions()[49],
		Visualization->GetActorLocation());
	TestNotEqual(
		TEXT("Changing the pile rebases the rest of the fleet"),
		Visualization->GetDronePositions()[0],
		PositionsBeforeFixedPile[0]);
	TestEqual(
		TEXT("Fixed pile gets display number zero when no NPZ number exists"),
		Visualization->GetDroneDisplayIDsForTest()[49],
		0.0f);
	TMap<FString, int32> NpzPriorityOverride;
	NpzPriorityOverride.Add(TEXT("Drone050"), 12);
	Visualization->SetConfirmedRouteSlotDisplayOverrides(NpzPriorityOverride);
	TestEqual(
		TEXT("Confirmed NPZ numbering has priority over fixed-pile numbering"),
		Visualization->GetDroneDisplayIDsForTest()[49],
		12.0f);
	Visualization->ClearConfirmedRouteSlotDisplayOverrides();
	TestEqual(
		TEXT("Clearing NPZ numbering reveals fixed-pile numbering again"),
		Visualization->GetDroneDisplayIDsForTest()[49],
		0.0f);

	FGCSFleetSnapshot PileSwapSnapshot;
	PileSwapSnapshot.Header.SessionID = TEXT("FLEET-VIZ-TEST");
	PileSwapSnapshot.FleetRevision = 13;
	PileSwapSnapshot.Drones.Add(MakeGPSDrone(0, 31.932471, 118.638531, true));
	PileSwapSnapshot.Drones.Add(MakeGPSDrone(50, 31.932961, 118.639021, true));
	TestTrue(TEXT("Two-drone pile swap snapshot applied"), Fleet->ApplySnapshot(PileSwapSnapshot));
	TestTrue(
		TEXT("Drone050 reapplied as fixed pile for the swap test"),
		Fleet->SetGeoOriginFromDroneID(TEXT("Drone050"), FixedPileError));
	Visualization->SetFixedPileDroneID(TEXT("Drone050"));
	Visualization->RefreshFromFleet();
	TestEqual(
		TEXT("The drone previously displaying zero receives the pile's old number"),
		Visualization->GetDroneDisplayIDsForTest()[0],
		50.0f);
	TestEqual(
		TEXT("The selected pile displays zero"),
		Visualization->GetDroneDisplayIDsForTest()[1],
		0.0f);
	FString ResolvedDisplayDroneID;
	TestTrue(
		TEXT("A swapped display number resolves to the physical drone currently showing it"),
		Visualization->TryResolveDroneIDFromDisplayID(50, ResolvedDisplayDroneID));
	TestEqual(
		TEXT("Display number 50 now belongs to physical Drone000"),
		ResolvedDisplayDroneID,
		FString(TEXT("Drone000")));
	TestTrue(
		TEXT("The currently displayed 50 can become the next fixed pile"),
		Fleet->SetGeoOriginFromDroneID(ResolvedDisplayDroneID, FixedPileError));
	Visualization->SetFixedPileDroneID(ResolvedDisplayDroneID);
	Visualization->RefreshFromFleet();
	TestEqual(
		TEXT("Applying the swapped display number moves its physical drone to the origin"),
		Visualization->GetDronePositions()[0],
		Visualization->GetActorLocation());
	TestEqual(
		TEXT("The newly selected physical drone displays zero"),
		Visualization->GetDroneDisplayIDsForTest()[0],
		0.0f);
	TestEqual(
		TEXT("The previous pile receives the selected display number"),
		Visualization->GetDroneDisplayIDsForTest()[1],
		50.0f);
	NpzPriorityOverride.Reset();
	NpzPriorityOverride.Add(TEXT("Drone000"), 7);
	NpzPriorityOverride.Add(TEXT("Drone050"), 12);
	Visualization->SetConfirmedRouteSlotDisplayOverrides(NpzPriorityOverride);
	TestEqual(
		TEXT("NPZ number overrides the zero-holder side of the pile swap"),
		Visualization->GetDroneDisplayIDsForTest()[0],
		7.0f);
	TestEqual(
		TEXT("NPZ number overrides the selected-pile side of the pile swap"),
		Visualization->GetDroneDisplayIDsForTest()[1],
		12.0f);

	const FDateTime LiveSampleBaseUtc = FDateTime::UtcNow();
	const int64 LiveSampleBaseUtcMs = LiveSampleBaseUtc.ToUnixTimestamp() * 1000
		+ LiveSampleBaseUtc.GetMillisecond();
	FGCSTelemetryBatch FirstLiveSampleBatch;
	FirstLiveSampleBatch.FleetRevision = 14;
	FDroneDataDelta FirstLiveSampleDelta;
	FirstLiveSampleDelta.DroneID = TEXT("Drone050");
	FirstLiveSampleDelta.ChangedFields = static_cast<int32>(
		EGCSDroneDataField::GPS
		| EGCSDroneDataField::Altitude
		| EGCSDroneDataField::LEDColor);
	FirstLiveSampleDelta.Data = MakeGPSDrone(50, 31.932961, 118.639021, true);
	FirstLiveSampleDelta.Data.GPSSampleTimestampUtcMs = LiveSampleBaseUtcMs;
	FirstLiveSampleDelta.Data.LEDColor = FLinearColor::Red;
	FirstLiveSampleDelta.Data.bLEDColorValid = true;
	FirstLiveSampleBatch.Deltas.Add(FirstLiveSampleDelta);
	TestTrue(TEXT("First timestamped live sample applied"), Fleet->ApplyDeltaBatch(FirstLiveSampleBatch));
	Visualization->RefreshFromFleet();
	const FVector FirstLivePosition = Visualization->GetDronePositions()[1];
	TestTrue(
		TEXT("Confirmed aircraft LED feedback reaches the visualization color array"),
		Visualization->GetDroneColorsForTest()[1].Equals(FLinearColor::Red));

	FGCSTelemetryBatch SecondLiveSampleBatch;
	SecondLiveSampleBatch.FleetRevision = 15;
	FDroneDataDelta SecondLiveSampleDelta = FirstLiveSampleDelta;
	SecondLiveSampleDelta.Data = MakeGPSDrone(50, 31.933061, 118.639121, true);
	SecondLiveSampleDelta.Data.GPSSampleTimestampUtcMs = LiveSampleBaseUtcMs + 250;
	SecondLiveSampleDelta.Data.LEDColor = FLinearColor::Blue;
	SecondLiveSampleDelta.Data.bLEDColorValid = true;
	SecondLiveSampleBatch.Deltas.Add(SecondLiveSampleDelta);
	TestTrue(TEXT("Second timestamped live sample applied"), Fleet->ApplyDeltaBatch(SecondLiveSampleBatch));
	Visualization->RefreshFromFleet();
	TestTrue(
		TEXT("Stable particle IDs still accept changed confirmed LED feedback"),
		Visualization->GetDroneColorsForTest()[1].Equals(FLinearColor::Blue));
	TestTrue(
		TEXT("Interpolation buffer initially holds the previous real GPS sample"),
		Visualization->GetDronePositions()[1].Equals(FirstLivePosition, 0.01));

	Visualization->AdvanceLiveInterpolationForTest(LiveSampleBaseUtcMs + 400);
	const FVector MidLivePosition = Visualization->GetDronePositions()[1];
	Visualization->AdvanceLiveInterpolationForTest(LiveSampleBaseUtcMs + 550);
	const FVector FinalLivePosition = Visualization->GetDronePositions()[1];
	TestFalse(
		TEXT("A frame between samples advances smoothly from the previous position"),
		MidLivePosition.Equals(FirstLivePosition, 0.01));
	TestFalse(
		TEXT("The intermediate frame has not jumped directly to the latest position"),
		MidLivePosition.Equals(FinalLivePosition, 0.01));

	TArray<FVector> RouteTargets;
	RouteTargets.Add(FVector(10.0, 20.0, 30.0));
	RouteTargets.Add(FVector(40.0, 50.0, 60.0));
	RouteTargets.Add(FVector(70.0, 80.0, 90.0));
	Visualization->SetRouteTargets(RouteTargets, true);
	TestEqual(
		TEXT("Route overlay count follows the submitted dynamic array"),
		Visualization->GetRouteTargetCount(),
		RouteTargets.Num());
	TestTrue(
		TEXT("Atomic route overlay submission applies visibility"),
		Visualization->AreRouteTargetsVisible());
	RouteTargets.SetNum(1);
	Visualization->SetRouteTargets(RouteTargets, true);
	TestEqual(
		TEXT("Route overlay count is not fixed to a fleet-size constant"),
		Visualization->GetRouteTargetCount(),
		RouteTargets.Num());

	TArray<FVector3f> RoutePreviewPositions;
	RoutePreviewPositions.Add(FVector3f(0.0f, 0.0f, 10.0f));
	RoutePreviewPositions.Add(FVector3f(1.0f, 0.0f, 10.0f));
	RoutePreviewPositions.Add(FVector3f(2.0f, 0.0f, 10.0f));
	TArray<FLinearColor> RoutePreviewColors;
	RoutePreviewColors.Init(FLinearColor::White, RoutePreviewPositions.Num());
	FString RoutePreviewError;
	TestTrue(
		TEXT("Full route preview frame is accepted"),
		Visualization->SetRoutePreviewFrame(
			RoutePreviewPositions,
			RoutePreviewColors,
			31.932471,
			118.638531,
			30.0,
			RoutePreviewError));
	TestEqual(
		TEXT("Route preview keeps physical DroneIDs out of preview-only particles"),
		Visualization->GetParticleDroneIDs().Num(),
		0);
	const TArray<float> RoutePreviewDisplayIDs = Visualization->GetDroneDisplayIDsForTest();
	TestEqual(TEXT("RouteSlot zero is displayed in preview"), RoutePreviewDisplayIDs[0], 0.0f);
	TestEqual(TEXT("RouteSlot one is displayed in preview"), RoutePreviewDisplayIDs[1], 1.0f);
	TestEqual(TEXT("RouteSlot two is displayed in preview"), RoutePreviewDisplayIDs[2], 2.0f);

	Visualization->ExitRoutePreview();
	TestEqual(
		TEXT("Exiting preview restores the Live physical DroneIDs"),
		Visualization->GetParticleDroneIDs().Num(),
		2);
	TestEqual(
		TEXT("Exiting preview restores Live display-number rules"),
		Visualization->GetDroneDisplayIDsForTest()[0],
		7.0f);

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[DRONE_FLEET_VIZ_TEST] cesium=PASS originLocked=PASS invalidGPSZero=PASS gpsRecovery=PASS liveInterpolation=PASS liveColor=PASS fleet100=PASS"));

	World->DestroyActor(Visualization);
	GameInstance->Shutdown();
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSFlatMapDisplayTest, "DroneGCS.Visualization.FlatMap.GroundAndPreviewAlignment", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSFlatMapDisplayTest::RunTest(const FString&)
{
 auto* GI = NewObject<UGameInstance>(GEngine);
 GI->InitializeStandalone(TEXT("FlatMapDisplayTest"));
 auto* World = GI->GetWorld();
 auto* Geo = World->SpawnActor<ACesiumGeoreference>();
 auto* Map = World->SpawnActor<AActor>();
 auto* Plane = NewObject<USceneComponent>(Map, TEXT("MapPlane"));
 Map->SetRootComponent(Plane);
 Plane->RegisterComponent();
 Map->Tags.Add(TEXT("GCS.SceneMap"));
 Map->SetActorLocationAndRotation(FVector(0, 0, -4500), FRotator(10, 15, 0));
 const FTransform OriginalMap = Map->GetActorTransform();
 const FTransform OriginalGeo = Geo->GetActorTransform();
 const FVector MapUp = Map->GetActorUpVector();
 auto* Viz = World->SpawnActor<ADroneFleetVisualizationActor>();
 auto* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
 auto Snapshot = MakeGPSSnapshot(3, 1);
 Snapshot.Drones[1].AltitudeAMSL = Snapshot.Drones[0].AltitudeAMSL + 10;
 Snapshot.Drones[2].AltitudeAMSL = Snapshot.Drones[0].AltitudeAMSL - 2;
 AddExpectedError(TEXT("Live altitude below display reference"), EAutomationExpectedErrorFlags::Contains, 1);
 TestTrue(TEXT("Live snapshot applied"), Fleet->ApplySnapshot(Snapshot));
 Viz->RefreshFromFleet();
 const auto Live = Viz->GetDronePositions();
 if (TestEqual(TEXT("Live count"), Live.Num(), 3))
 {
  TestTrue(TEXT("Ground sphere bottom touches actual map plane"), FMath::IsNearlyEqual(FVector::DotProduct(Live[0] - Map->GetActorLocation(), MapUp), 15.0, 0.01));
  TestTrue(TEXT("Live altitude difference preserves ten meters"), FMath::IsNearlyEqual(FVector::DotProduct(Live[1] - Live[0], MapUp), 10.0 * Geo->GetScale(), 0.1));
  TestTrue(TEXT("Negative live display altitude touches ground"), FMath::IsNearlyZero(FVector::DotProduct(Live[2] - Live[0], MapUp), 0.01));
 }
 TArray<FVector3f> ENU = {FVector3f::ZeroVector, FVector3f(4, 5, 10), FVector3f(4, 5, 0), FVector3f(4, 5, -2)};
 TestEqual(TEXT("Raw telemetry altitude remains negative relative to origin"), Fleet->GetDronePage(0, 3)[2].AltitudeAMSL, Snapshot.Drones[2].AltitudeAMSL);
 const auto OriginalENU = ENU;
 TArray<FVector> Overlay, Absolute;
 FVector Origin, AbsoluteOrigin;
 FString Error;
 const auto& D = Snapshot.Drones[0];
 TestTrue(TEXT("Absolute Cesium conversion stays available"), Viz->ConvertRouteENUPositionsToWorld(ENU, D.GPSLat, D.GPSLon, D.AltitudeAMSL, Absolute, AbsoluteOrigin, Error));
 AddExpectedError(TEXT("NPZ contains"), EAutomationExpectedErrorFlags::Contains, 1);
 TestTrue(TEXT("Absolute conversion retains negative source height"), FMath::IsNearlyEqual(FVector::Distance(Absolute[2], Absolute[3]), 2.0 * Geo->GetScale(), 0.1));
 TestTrue(TEXT("Overlay converts"), Viz->ConvertRouteENUPositionsToLiveWorld(ENU, D.GPSLat, D.GPSLon, D.AltitudeAMSL, Overlay, Origin, Error));
 TArray<FLinearColor> Colors; Colors.Init(FLinearColor::White, ENU.Num());
 TestTrue(TEXT("Full preview converts"), Viz->SetRoutePreviewFrame(ENU, Colors, D.GPSLat, D.GPSLon, D.AltitudeAMSL, Error));
 const auto Preview = Viz->GetDronePositions();
 TestTrue(TEXT("Overlay and full playback share exact positions"), Overlay == Preview);
 TestTrue(TEXT("NPZ ground and live origin coincide"), Preview[0].Equals(Live[0], 0.01));
 TestTrue(TEXT("NPZ landing returns to map surface"), FMath::IsNearlyEqual(FVector::DotProduct(Preview[2] - Map->GetActorLocation(), MapUp), 15.0, 0.01));
 TestTrue(TEXT("Negative NPZ display height equals ground at same XY"), Preview[3].Equals(Preview[2], 0.01));
 TestTrue(TEXT("Positive NPZ height remains ten meters"), FMath::IsNearlyEqual(FVector::DotProduct(Preview[1] - Preview[2], MapUp), 10.0 * Geo->GetScale(), 0.01));
 TestTrue(TEXT("Source ENU untouched"), ENU == OriginalENU);
 TestTrue(TEXT("Map transform untouched"), OriginalMap.Equals(Map->GetActorTransform()));
 TestTrue(TEXT("Georeference transform untouched"), OriginalGeo.Equals(Geo->GetActorTransform()));
 Viz->ExitRoutePreview();
 TestTrue(TEXT("Exit preview returns to same ground anchor"), Viz->GetDronePositions()[0].Equals(Live[0], 0.01));
 GI->Shutdown();
 return true;
}
#endif
