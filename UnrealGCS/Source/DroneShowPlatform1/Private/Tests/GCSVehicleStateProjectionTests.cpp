#if WITH_DEV_AUTOMATION_TESTS
#include "GCSVehicleStateProjection.h"
#include "GCSServerAdapterSubsystem.h"
#include "FleetManagerSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Async/TaskGraphInterfaces.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSVehicleStateProjectionTest,
    "DroneGCS.Telemetry.VehicleState.ProjectionWithoutWidget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSVehicleStateProjectionTest::RunTest(const FString& Parameters)
{
    (void)Parameters;
    dspgcs::TelemetryDTO Raw;
    Raw.droneId = "Drone007"; Raw.sysId = 1;
    Raw.connection.linkId = "192.0.2.7:14550"; Raw.connection.online = true;
    Raw.gps.latitudeDeg = 31.25; Raw.gps.longitudeDeg = 118.75;
    Raw.gps.valid = true; Raw.gps.fixType = 6;
    Raw.sampleTimestampUtcMs = 1700000000123;
    Raw.altitude.relativeMeters = 12.5f; Raw.altitude.amslMeters = 42.5f;
    Raw.altitude.valid = true;
    Raw.battery.valid = true; Raw.battery.remainingPercent = 81.0f;
    Raw.rssi.valid = true; Raw.rssi.valueDbm = -67;
    Raw.mode.modeName = "GUIDED";
    Raw.led.red = 255; Raw.led.green = 128; Raw.led.blue = 0; Raw.led.confirmed = true;
    const FDroneData State = GCSVehicleStateProjection::FromTransport(Raw, true);
    TestEqual(TEXT("Logical identity"), State.DroneID, FString(TEXT("Drone007")));
    TestEqual(TEXT("SYSID remains a separate value"), State.SysID, 1);
    TestEqual(TEXT("Endpoint identity preserved"), State.EndpointIdentity, FString(TEXT("192.0.2.7:14550")));
    TestEqual(TEXT("Latitude"), State.GPSLat, 31.25);
    TestEqual(TEXT("Longitude"), State.GPSLon, 118.75);
    TestTrue(TEXT("GPS validity"), State.bGPSValid);
    TestEqual(TEXT("Fix type"), State.GPSFixType, 6);
    TestEqual(TEXT("Sample timestamp unchanged"), State.GPSSampleTimestampUtcMs, int64(1700000000123));
    TestEqual(TEXT("Relative altitude"), State.Altitude, 12.5f);
    TestEqual(TEXT("AMSL altitude"), State.AltitudeAMSL, 42.5f);
    TestTrue(TEXT("Altitude validity"), State.bAltitudeValid);
    TestEqual(TEXT("Battery"), State.Battery, 81.0f);
    TestEqual(TEXT("RSSI"), State.RSSI, -67);
    TestEqual(TEXT("Mode label unchanged"), State.Mode, FString(TEXT("GUIDED")));
    TestTrue(TEXT("Online"), State.bOnline);
    TestTrue(TEXT("Initial detail selection hint"), State.bSelected);
    TestEqual(TEXT("Confirmed RGB normalized without formatting"), State.LEDColor, FLinearColor(1.f,128.f/255.f,0.f,1.f));
    TestTrue(TEXT("LED validity"), State.bLEDColorValid);

    Raw.battery.valid = false; Raw.rssi.valid = false; Raw.gps.valid = false;
    Raw.altitude.valid = false; Raw.led.confirmed = false;
    const FDroneData Invalid = GCSVehicleStateProjection::FromTransport(Raw, false);
    TestEqual(TEXT("Existing invalid battery fallback"), Invalid.Battery, 0.f);
    TestEqual(TEXT("Existing invalid RSSI fallback"), Invalid.RSSI, 0);
    TestFalse(TEXT("GPS validity is independent of stored coordinate"), Invalid.bGPSValid);
    TestEqual(TEXT("Invalid GPS retains original numeric sample"), Invalid.GPSLat, State.GPSLat);
    TestFalse(TEXT("Invalid altitude is marked"), Invalid.bAltitudeValid);
    TestEqual(TEXT("Invalid altitude retains original value"), Invalid.Altitude, State.Altitude);
    TestFalse(TEXT("Unconfirmed color is marked"), Invalid.bLEDColorValid);
    TestFalse(TEXT("Projection accepts independent selection hint"), Invalid.bSelected);

    UGameInstance* GI = NewObject<UGameInstance>(GEngine);
    GI->InitializeStandalone(TEXT("VehicleStateProjectionWorld"));
    UGCSServerAdapterSubsystem* Adapter = GI->GetSubsystem<UGCSServerAdapterSubsystem>();
    UFleetManagerSubsystem* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
    if (!TestNotNull(TEXT("Adapter"), Adapter) || !TestNotNull(TEXT("Fleet state owner"), Fleet))
    { GI->Shutdown(); return false; }
    FGCSFleetSnapshot Snapshot;
    Snapshot.Header.SourceID = TEXT("projection-test");
    Snapshot.Header.SessionID = TEXT("projection-session");
    Snapshot.FleetRevision = 7; Snapshot.Drones.Add(State);
    TestTrue(TEXT("Protocol-neutral snapshot accepted"), Adapter->PushFleetSnapshot(Snapshot));
    FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
    FDroneData Read;
    TestTrue(TEXT("State query requires no widget/provider access"), Fleet->GetDroneByID(State.DroneID, Read));
    TestEqual(TEXT("Revision committed"), Fleet->GetFleetRevision(), int64(7));
    TestEqual(TEXT("Session committed"), Fleet->GetActiveFleetSessionID(), Snapshot.Header.SessionID);
    TestEqual(TEXT("Projection reaches public Fleet state"), Read.LEDColor, State.LEDColor);
    Read.GPSLat = 0;
    TestTrue(TEXT("Can query again"), Fleet->GetDroneByID(State.DroneID, Read));
    TestEqual(TEXT("Query returns an isolated state copy"), Read.GPSLat, State.GPSLat);
    TestEqual(TEXT("Paging uses same state boundary"), Fleet->GetDronePage(0, 10).Num(), 1);
    GI->Shutdown();
    return true;
}
#endif
