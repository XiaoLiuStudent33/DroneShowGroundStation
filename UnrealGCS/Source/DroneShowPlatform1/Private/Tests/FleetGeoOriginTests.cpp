#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DroneData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSDataTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFleetGeoOriginTest,
	"DroneGCS.Data.FleetGeoOrigin.LockingAndGPSValidity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
	FDroneData MakeDrone(
		const TCHAR* DroneID,
		const double Latitude,
		const double Longitude,
		const bool bGPSValid,
		const int32 GPSFixType,
		const float AltitudeAMSL,
		const bool bAltitudeValid)
	{
		FDroneData Drone;
		Drone.DroneID = DroneID;
		Drone.GPSLat = Latitude;
		Drone.GPSLon = Longitude;
		Drone.bGPSValid = bGPSValid;
		Drone.GPSFixType = GPSFixType;
		Drone.Altitude = 0.0f;
		Drone.AltitudeAMSL = AltitudeAMSL;
		Drone.bAltitudeValid = bAltitudeValid;
		Drone.bOnline = true;
		return Drone;
	}

	FGCSTelemetryBatch MakeGPSDelta(
		const FDroneData& Drone,
		const int64 Revision)
	{
		FGCSTelemetryBatch Batch;
		Batch.FleetRevision = Revision;
		FDroneDataDelta Delta;
		Delta.DroneID = Drone.DroneID;
		Delta.ChangedFields = static_cast<int32>(
			EGCSDroneDataField::GPS | EGCSDroneDataField::Altitude);
		Delta.Data = Drone;
		Batch.Deltas.Add(MoveTemp(Delta));
		return Batch;
	}
}

bool FFleetGeoOriginTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	if (!TestNotNull(TEXT("GameInstance created"), GameInstance))
	{
		return false;
	}

	GameInstance->InitializeStandalone(TEXT("FleetGeoOriginTestWorld"));
	UFleetManagerSubsystem* Fleet =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	if (!TestNotNull(TEXT("FleetManager initialized"), Fleet))
	{
		GameInstance->Shutdown();
		return false;
	}

	FGCSFleetSnapshot Snapshot;
	Snapshot.Header.SessionID = TEXT("FLEET-SESSION-A");
	Snapshot.FleetRevision = 1;
	Snapshot.Drones.Add(MakeDrone(
		TEXT("Drone001"), 0.0, 0.0, false, 0, 0.0f, false));
	Snapshot.Drones.Add(MakeDrone(
		TEXT("Drone002"), 31.932500, 118.638600, true,
		DroneGPS::MinimumValidFixType, 15.0f, true));
	TestTrue(TEXT("Initial snapshot applied"), Fleet->ApplySnapshot(Snapshot));
	TestFalse(
		TEXT("Drone002 cannot replace invalid first logical drone as origin"),
		Fleet->GetFleetGeoOrigin().bOriginInitialized);

	FDroneData InvalidFix = MakeDrone(
		TEXT("Drone001"), 31.932471, 118.638531, true,
		DroneGPS::MinimumValidFixType - 1, 12.0f, true);
	TestTrue(TEXT("Invalid-fix delta applied"), Fleet->ApplyDeltaBatch(MakeGPSDelta(InvalidFix, 2)));
	TestFalse(TEXT("Fix below threshold does not initialize origin"), Fleet->GetFleetGeoOrigin().bOriginInitialized);

	FDroneData ZeroCoordinates = MakeDrone(
		TEXT("Drone001"), 0.0, 0.0, true,
		DroneGPS::MinimumValidFixType, 12.0f, true);
	TestTrue(TEXT("Zero-coordinate delta applied"), Fleet->ApplyDeltaBatch(MakeGPSDelta(ZeroCoordinates, 3)));
	TestFalse(TEXT("Latitude and longitude both zero are invalid"), Fleet->GetFleetGeoOrigin().bOriginInitialized);

	const FDroneData ValidOrigin = MakeDrone(
		TEXT("Drone001"), 31.932471, 118.638531, true,
		DroneGPS::MinimumValidFixType, 13.1f, true);
	TestTrue(TEXT("Valid origin delta applied"), Fleet->ApplyDeltaBatch(MakeGPSDelta(ValidOrigin, 4)));
	const FFleetGeoOrigin EstablishedOrigin = Fleet->GetFleetGeoOrigin();
	TestTrue(TEXT("Origin initialized"), EstablishedOrigin.bOriginInitialized);
	TestEqual(TEXT("Origin drone is first logical drone"), EstablishedOrigin.OriginDroneID, FString(TEXT("Drone001")));
	TestEqual(TEXT("Origin latitude captured"), EstablishedOrigin.OriginLatitude, ValidOrigin.GPSLat);
	TestEqual(TEXT("Origin longitude captured"), EstablishedOrigin.OriginLongitude, ValidOrigin.GPSLon);
	TestTrue(TEXT("Origin AMSL validity captured"), EstablishedOrigin.bOriginAltitudeValid);
	TestEqual(TEXT("Origin AMSL captured"), EstablishedOrigin.OriginAltitudeAMSL, ValidOrigin.AltitudeAMSL);

	FDroneData MovedOrigin = ValidOrigin;
	MovedOrigin.GPSLat += 0.01;
	MovedOrigin.GPSLon += 0.01;
	MovedOrigin.AltitudeAMSL += 100.0f;
	TestTrue(TEXT("Moved origin drone delta applied"), Fleet->ApplyDeltaBatch(MakeGPSDelta(MovedOrigin, 5)));
	const FFleetGeoOrigin LockedAfterMove = Fleet->GetFleetGeoOrigin();
	TestEqual(TEXT("Origin latitude remains locked"), LockedAfterMove.OriginLatitude, EstablishedOrigin.OriginLatitude);
	TestEqual(TEXT("Origin longitude remains locked"), LockedAfterMove.OriginLongitude, EstablishedOrigin.OriginLongitude);
	TestEqual(TEXT("Origin altitude remains locked"), LockedAfterMove.OriginAltitudeAMSL, EstablishedOrigin.OriginAltitudeAMSL);

	FGCSTelemetryBatch OfflineBatch;
	OfflineBatch.FleetRevision = 6;
	FDroneDataDelta OfflineDelta;
	OfflineDelta.DroneID = TEXT("Drone001");
	OfflineDelta.ChangedFields = static_cast<int32>(EGCSDroneDataField::Online);
	OfflineDelta.Data.bOnline = false;
	OfflineBatch.Deltas.Add(OfflineDelta);
	TestTrue(TEXT("Origin drone offline delta applied"), Fleet->ApplyDeltaBatch(OfflineBatch));
	const FFleetGeoOrigin LockedAfterOffline = Fleet->GetFleetGeoOrigin();
	TestTrue(TEXT("Origin survives offline state"), LockedAfterOffline.bOriginInitialized);
	TestEqual(TEXT("Offline does not change origin latitude"), LockedAfterOffline.OriginLatitude, EstablishedOrigin.OriginLatitude);

	FDroneData DeltaMapped;
	TestTrue(TEXT("Drone001 can be read after deltas"), Fleet->GetDroneByID(TEXT("Drone001"), DeltaMapped));
	TestTrue(TEXT("GPS validity copied by delta"), DeltaMapped.bGPSValid);
	TestEqual(TEXT("GPS fix type copied by delta"), DeltaMapped.GPSFixType, DroneGPS::MinimumValidFixType);
	TestTrue(TEXT("Altitude validity copied by delta"), DeltaMapped.bAltitudeValid);
	TestEqual(TEXT("AMSL copied by delta"), DeltaMapped.AltitudeAMSL, MovedOrigin.AltitudeAMSL);

	FGCSFleetSnapshot NewSessionSnapshot;
	NewSessionSnapshot.Header.SessionID = TEXT("FLEET-SESSION-B");
	NewSessionSnapshot.FleetRevision = 1;
	const FDroneData ZeroAltitudeOrigin = MakeDrone(
		TEXT("Drone001"), 32.0, 119.0, true,
		DroneGPS::MinimumValidFixType, 0.0f, true);
	NewSessionSnapshot.Drones.Add(ZeroAltitudeOrigin);
	TestTrue(TEXT("New fleet session snapshot applied"), Fleet->ApplySnapshot(NewSessionSnapshot));
	const FFleetGeoOrigin NewSessionOrigin = Fleet->GetFleetGeoOrigin();
	TestTrue(TEXT("New session establishes a new origin"), NewSessionOrigin.bOriginInitialized);
	TestEqual(TEXT("New session origin latitude used"), NewSessionOrigin.OriginLatitude, ZeroAltitudeOrigin.GPSLat);
	TestTrue(TEXT("Zero AMSL remains a valid altitude"), NewSessionOrigin.bOriginAltitudeValid);
	TestEqual(TEXT("Zero AMSL is preserved"), NewSessionOrigin.OriginAltitudeAMSL, 0.0f);

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[FLEET_GEO_ORIGIN_TEST] waitForDrone001=PASS initialize=PASS lockAfterMove=PASS lockAfterOffline=PASS deltaSemantics=PASS zeroAMSL=PASS"));

	GameInstance->Shutdown();
	return true;
}

#endif
