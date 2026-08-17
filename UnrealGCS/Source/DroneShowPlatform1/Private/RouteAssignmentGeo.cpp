#include "RouteAssignmentGeo.h"

namespace
{
	constexpr double DegreesToRadians = UE_DOUBLE_PI / 180.0;
	constexpr int32 MinimumAssignmentGpsFixType = 3;

	bool IsLatitudeValid(const double Latitude)
	{
		return FMath::IsFinite(Latitude) && Latitude >= -90.0 && Latitude <= 90.0;
	}

	bool IsLongitudeValid(const double Longitude)
	{
		return FMath::IsFinite(Longitude) && Longitude >= -180.0 && Longitude <= 180.0;
	}

	bool IsDroneGeoEligible(const FRouteAssignmentDroneSource& Drone)
	{
		return Drone.bOnline
			&& Drone.bGPSValid
			&& Drone.GPSFixType >= MinimumAssignmentGpsFixType
			&& !(Drone.Latitude == 0.0 && Drone.Longitude == 0.0)
			&& IsLatitudeValid(Drone.Latitude)
			&& IsLongitudeValid(Drone.Longitude);
	}
}

RouteAssignmentGeo::FEcefPosition RouteAssignmentGeo::GeodeticToEcef(
	const double LatitudeDegrees,
	const double LongitudeDegrees,
	const double EllipsoidHeightMeters)
{
	const double LatitudeRadians = LatitudeDegrees * DegreesToRadians;
	const double LongitudeRadians = LongitudeDegrees * DegreesToRadians;
	const double SinLatitude = FMath::Sin(LatitudeRadians);
	const double CosLatitude = FMath::Cos(LatitudeRadians);
	const double SinLongitude = FMath::Sin(LongitudeRadians);
	const double CosLongitude = FMath::Cos(LongitudeRadians);
	const double PrimeVerticalRadius = WGS84SemiMajorAxisMeters
		/ FMath::Sqrt(1.0 - WGS84EccentricitySquared * SinLatitude * SinLatitude);

	FEcefPosition Result;
	Result.X = (PrimeVerticalRadius + EllipsoidHeightMeters) * CosLatitude * CosLongitude;
	Result.Y = (PrimeVerticalRadius + EllipsoidHeightMeters) * CosLatitude * SinLongitude;
	Result.Z = (PrimeVerticalRadius * (1.0 - WGS84EccentricitySquared)
		+ EllipsoidHeightMeters) * SinLatitude;
	return Result;
}

bool RouteAssignmentGeo::BuildDroneLocalPositions(
	const FRouteAssignmentSourceSnapshot& SourceSnapshot,
	TArray<FRouteAssignmentDroneLocalPosition>& OutPositions,
	FString& OutError)
{
	OutPositions.Reset();
	OutError.Reset();

	const FRouteAssignmentSourceIdentity& SourceIdentity = SourceSnapshot.SourceIdentity;
	if (!SourceIdentity.bShowOriginSet)
	{
		OutError = TEXT("Route ShowOrigin must be set before building assignment coordinates.");
		return false;
	}

	if (!IsLatitudeValid(SourceIdentity.Latitude))
	{
		OutError = TEXT("Route ShowOrigin latitude must be finite and in [-90, 90].");
		return false;
	}

	if (!IsLongitudeValid(SourceIdentity.Longitude))
	{
		OutError = TEXT("Route ShowOrigin longitude must be finite and in [-180, 180].");
		return false;
	}

	// Horizontal assignment deliberately projects both origin and drones at
	// zero ellipsoid height. AMSL values remain audit-only raw source data.
	const FEcefPosition OriginEcef = GeodeticToEcef(
		SourceIdentity.Latitude,
		SourceIdentity.Longitude,
		0.0);
	const double OriginLatitudeRadians = SourceIdentity.Latitude * DegreesToRadians;
	const double OriginLongitudeRadians = SourceIdentity.Longitude * DegreesToRadians;
	const double SinOriginLatitude = FMath::Sin(OriginLatitudeRadians);
	const double CosOriginLatitude = FMath::Cos(OriginLatitudeRadians);
	const double SinOriginLongitude = FMath::Sin(OriginLongitudeRadians);
	const double CosOriginLongitude = FMath::Cos(OriginLongitudeRadians);

	OutPositions.Reserve(SourceSnapshot.Drones.Num());
	for (const FRouteAssignmentDroneSource& Drone : SourceSnapshot.Drones)
	{
		FRouteAssignmentDroneLocalPosition LocalPosition;
		LocalPosition.DroneID = Drone.DroneID;
		if (IsDroneGeoEligible(Drone))
		{
			const FEcefPosition DroneEcef = GeodeticToEcef(
				Drone.Latitude,
				Drone.Longitude,
				0.0);
			const double DeltaX = DroneEcef.X - OriginEcef.X;
			const double DeltaY = DroneEcef.Y - OriginEcef.Y;
			const double DeltaZ = DroneEcef.Z - OriginEcef.Z;
			const double EastMeters = -SinOriginLongitude * DeltaX
				+ CosOriginLongitude * DeltaY;
			const double NorthMeters = -SinOriginLatitude * CosOriginLongitude * DeltaX
				- SinOriginLatitude * SinOriginLongitude * DeltaY
				+ CosOriginLatitude * DeltaZ;

			if (FMath::IsFinite(EastMeters) && FMath::IsFinite(NorthMeters))
			{
				LocalPosition.EastMeters = EastMeters;
				LocalPosition.NorthMeters = NorthMeters;
				LocalPosition.bAssignmentGeoEligible = true;
			}
		}

		OutPositions.Add(MoveTemp(LocalPosition));
	}

	return true;
}
