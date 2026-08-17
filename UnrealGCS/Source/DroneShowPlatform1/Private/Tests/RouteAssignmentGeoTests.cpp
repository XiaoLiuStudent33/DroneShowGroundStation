#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "RouteAssignmentGeo.h"

#include <limits>

namespace
{
	constexpr double CoordinateToleranceMeters = 1.0e-6;
	constexpr double EcefToleranceMeters = 1.0e-6;

	FRouteAssignmentSourceSnapshot MakeSnapshot(
		const double OriginLatitude,
		const double OriginLongitude)
	{
		FRouteAssignmentSourceSnapshot Snapshot;
		Snapshot.SourceIdentity.bShowOriginSet = true;
		Snapshot.SourceIdentity.Latitude = OriginLatitude;
		Snapshot.SourceIdentity.Longitude = OriginLongitude;
		Snapshot.SourceIdentity.Altitude = 1234.5;
		return Snapshot;
	}

	FRouteAssignmentDroneSource MakeDrone(
		const TCHAR* DroneID,
		const double Latitude,
		const double Longitude)
	{
		FRouteAssignmentDroneSource Drone;
		Drone.DroneID = DroneID;
		Drone.Latitude = Latitude;
		Drone.Longitude = Longitude;
		Drone.AltitudeAMSL = 9876.5f;
		Drone.bGPSValid = true;
		Drone.GPSFixType = 3;
		Drone.bAltitudeValid = true;
		Drone.bOnline = true;
		return Drone;
	}

	bool IsNearZero(const double Value)
	{
		return FMath::Abs(Value) <= CoordinateToleranceMeters;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoSameOriginTest,
	"DroneGCS.RouteAssignment.Geo.01SameOrigin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoSameOriginTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.932471, 118.638531);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneSame"), 31.932471, 118.638531));
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (!TestEqual(TEXT("One output"), Positions.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("Drone is eligible"), Positions[0].bAssignmentGeoEligible);
	TestTrue(TEXT("East is approximately zero"), IsNearZero(Positions[0].EastMeters));
	TestTrue(TEXT("North is approximately zero"), IsNearZero(Positions[0].NorthMeters));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoEastTest,
	"DroneGCS.RouteAssignment.Geo.02LongitudeIncreasesEast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoEastTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(0.0, 0.0);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneEast"), 0.0, 0.001));
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (Positions.Num() != 1)
	{
		AddError(TEXT("Expected one output."));
		return false;
	}
	TestTrue(TEXT("East is positive"), Positions[0].EastMeters > 0.0);
	TestTrue(TEXT("North is approximately zero"), IsNearZero(Positions[0].NorthMeters));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoNorthTest,
	"DroneGCS.RouteAssignment.Geo.03LatitudeIncreasesNorth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoNorthTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(0.0, 0.0);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneNorth"), 0.001, 0.0));
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (Positions.Num() != 1)
	{
		AddError(TEXT("Expected one output."));
		return false;
	}
	TestTrue(TEXT("North is positive"), Positions[0].NorthMeters > 0.0);
	TestTrue(TEXT("East is approximately zero"), IsNearZero(Positions[0].EastMeters));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoEcefEquatorPrimeMeridianTest,
	"DroneGCS.RouteAssignment.Geo.04EcefEquatorPrimeMeridian",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoEcefEquatorPrimeMeridianTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const RouteAssignmentGeo::FEcefPosition Ecef = RouteAssignmentGeo::GeodeticToEcef(0.0, 0.0, 0.0);
	TestTrue(TEXT("X is WGS84 semi-major axis"),
		FMath::Abs(Ecef.X - RouteAssignmentGeo::WGS84SemiMajorAxisMeters) <= EcefToleranceMeters);
	TestTrue(TEXT("Y is approximately zero"), FMath::Abs(Ecef.Y) <= EcefToleranceMeters);
	TestTrue(TEXT("Z is approximately zero"), FMath::Abs(Ecef.Z) <= EcefToleranceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoEcefNinetyLongitudeTest,
	"DroneGCS.RouteAssignment.Geo.05EcefNinetyLongitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoEcefNinetyLongitudeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const RouteAssignmentGeo::FEcefPosition Ecef = RouteAssignmentGeo::GeodeticToEcef(0.0, 90.0, 0.0);
	TestTrue(TEXT("X is approximately zero"), FMath::Abs(Ecef.X) <= EcefToleranceMeters);
	TestTrue(TEXT("Y is WGS84 semi-major axis"),
		FMath::Abs(Ecef.Y - RouteAssignmentGeo::WGS84SemiMajorAxisMeters) <= EcefToleranceMeters);
	TestTrue(TEXT("Z is approximately zero"), FMath::Abs(Ecef.Z) <= EcefToleranceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoProductionOriginTest,
	"DroneGCS.RouteAssignment.Geo.06ProductionOriginSamePosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoProductionOriginTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.932471, 118.638531);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneProduction"), 31.932471, 118.638531));
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (Positions.Num() != 1)
	{
		AddError(TEXT("Expected one output."));
		return false;
	}
	TestTrue(TEXT("East is approximately zero"), IsNearZero(Positions[0].EastMeters));
	TestTrue(TEXT("North is approximately zero"), IsNearZero(Positions[0].NorthMeters));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoOfflineTest,
	"DroneGCS.RouteAssignment.Geo.07OfflineRetainedIneligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoOfflineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.0, 118.0);
	FRouteAssignmentDroneSource Drone = MakeDrone(TEXT("DroneOffline"), 31.001, 118.001);
	Drone.bOnline = false;
	Snapshot.Drones.Add(Drone);
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (Positions.Num() != 1)
	{
		AddError(TEXT("Expected one retained output."));
		return false;
	}
	TestEqual(TEXT("DroneID retained"), Positions[0].DroneID, FString(TEXT("DroneOffline")));
	TestFalse(TEXT("Offline drone is ineligible"), Positions[0].bAssignmentGeoEligible);
	TestEqual(TEXT("Safe East default"), Positions[0].EastMeters, 0.0);
	TestEqual(TEXT("Safe North default"), Positions[0].NorthMeters, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoFixTypeTest,
	"DroneGCS.RouteAssignment.Geo.08InsufficientFixRetainedIneligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoFixTypeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.0, 118.0);
	FRouteAssignmentDroneSource Drone = MakeDrone(TEXT("DroneLowFix"), 31.001, 118.001);
	Drone.GPSFixType = 2;
	Snapshot.Drones.Add(Drone);
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (Positions.Num() != 1)
	{
		AddError(TEXT("Expected one retained output."));
		return false;
	}
	TestFalse(TEXT("Fix below three is ineligible"), Positions[0].bAssignmentGeoEligible);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoInvalidCoordinatesTest,
	"DroneGCS.RouteAssignment.Geo.09InvalidCoordinatesRetainedIneligible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoInvalidCoordinatesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.0, 118.0);
	Snapshot.Drones.Add(MakeDrone(
		TEXT("DroneNaN"), std::numeric_limits<double>::quiet_NaN(), 118.0));
	Snapshot.Drones.Add(MakeDrone(
		TEXT("DroneInfinity"), 31.0, std::numeric_limits<double>::infinity()));
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneRange"), 91.0, 118.0));
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneZero"), 0.0, 0.0));
	FRouteAssignmentDroneSource InvalidGps = MakeDrone(TEXT("DroneInvalidGPS"), 31.0, 118.0);
	InvalidGps.bGPSValid = false;
	Snapshot.Drones.Add(InvalidGps);
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (!TestEqual(TEXT("All invalid drones retained"), Positions.Num(), 5))
	{
		return false;
	}
	for (const FRouteAssignmentDroneLocalPosition& Position : Positions)
	{
		TestFalse(*FString::Printf(TEXT("%s is ineligible"), *Position.DroneID), Position.bAssignmentGeoEligible);
		TestEqual(*FString::Printf(TEXT("%s East is safe"), *Position.DroneID), Position.EastMeters, 0.0);
		TestEqual(*FString::Printf(TEXT("%s North is safe"), *Position.DroneID), Position.NorthMeters, 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoOrderTest,
	"DroneGCS.RouteAssignment.Geo.10CountAndOrderPreserved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.0, 118.0);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneC"), 31.001, 118.001));
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneA"), 31.002, 118.002));
	FRouteAssignmentDroneSource Offline = MakeDrone(TEXT("DroneB"), 31.003, 118.003);
	Offline.bOnline = false;
	Snapshot.Drones.Add(Offline);
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (!TestEqual(TEXT("Output count equals input count"), Positions.Num(), Snapshot.Drones.Num()))
	{
		return false;
	}
	for (int32 Index = 0; Index < Snapshot.Drones.Num(); ++Index)
	{
		TestEqual(
			*FString::Printf(TEXT("DroneID order at %d"), Index),
			Positions[Index].DroneID,
			Snapshot.Drones[Index].DroneID);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoInvalidOriginTest,
	"DroneGCS.RouteAssignment.Geo.11InvalidOriginRejectsBuild",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoInvalidOriginTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(91.0, 118.0);
	Snapshot.Drones.Add(MakeDrone(TEXT("DroneValid"), 31.0, 118.0));
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestFalse(TEXT("Invalid origin rejects build"),
		RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	TestTrue(TEXT("Rejected build has no partial output"), Positions.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentGeoAltitudeAuditOnlyTest,
	"DroneGCS.RouteAssignment.Geo.12AltitudeIsAuditOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentGeoAltitudeAuditOnlyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(31.0, 118.0);
	Snapshot.SourceIdentity.Altitude = 5000.0;
	FRouteAssignmentDroneSource LowAltitude = MakeDrone(TEXT("DroneLow"), 31.001, 118.001);
	LowAltitude.AltitudeAMSL = -1000.0f;
	LowAltitude.bAltitudeValid = false;
	FRouteAssignmentDroneSource HighAltitude = MakeDrone(TEXT("DroneHigh"), 31.001, 118.001);
	HighAltitude.AltitudeAMSL = 10000.0f;
	HighAltitude.bAltitudeValid = true;
	Snapshot.Drones.Add(LowAltitude);
	Snapshot.Drones.Add(HighAltitude);
	TArray<FRouteAssignmentDroneLocalPosition> Positions;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentGeo::BuildDroneLocalPositions(Snapshot, Positions, Error));
	if (!TestEqual(TEXT("Both outputs retained"), Positions.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("Altitude validity does not affect eligibility"),
		Positions[0].bAssignmentGeoEligible && Positions[1].bAssignmentGeoEligible);
	TestEqual(TEXT("Altitude does not affect East"), Positions[0].EastMeters, Positions[1].EastMeters);
	TestEqual(TEXT("Altitude does not affect North"), Positions[0].NorthMeters, Positions[1].NorthMeters);
	return true;
}

#endif
