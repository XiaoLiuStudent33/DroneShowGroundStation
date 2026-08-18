#include "RouteAssignmentGeo.h"

namespace
{
	constexpr double DegreesToRadians = UE_DOUBLE_PI / 180.0;
	constexpr double RadiansToDegrees = 180.0 / UE_DOUBLE_PI;
	constexpr double MinimumNonDegenerateEcefRadiusMeters = 1.0;
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

bool RouteAssignmentGeo::EcefToGeodetic(
	const FEcefPosition& Ecef,
	FGeodeticPosition& OutGeodetic,
	FString& OutError)
{
	OutGeodetic = FGeodeticPosition{};
	OutError.Reset();
	if (!FMath::IsFinite(Ecef.X) || !FMath::IsFinite(Ecef.Y) || !FMath::IsFinite(Ecef.Z))
	{
		OutError = TEXT("ECEF coordinates must be finite.");
		return false;
	}

	const double RadiusSquared = Ecef.X * Ecef.X + Ecef.Y * Ecef.Y + Ecef.Z * Ecef.Z;
	if (!FMath::IsFinite(RadiusSquared)
		|| RadiusSquared < MinimumNonDegenerateEcefRadiusMeters * MinimumNonDegenerateEcefRadiusMeters)
	{
		OutError = TEXT("ECEF coordinates are degenerate near the Earth center.");
		return false;
	}

	const double SemiMinorAxisMeters = WGS84SemiMajorAxisMeters * (1.0 - WGS84Flattening);
	const double SecondEccentricitySquared =
		(WGS84SemiMajorAxisMeters * WGS84SemiMajorAxisMeters
			- SemiMinorAxisMeters * SemiMinorAxisMeters)
		/ (SemiMinorAxisMeters * SemiMinorAxisMeters);
	const double HorizontalRadius = FMath::Sqrt(Ecef.X * Ecef.X + Ecef.Y * Ecef.Y);

	double LatitudeRadians = 0.0;
	double LongitudeRadians = 0.0;
	double HeightMeters = 0.0;
	if (HorizontalRadius <= UE_DOUBLE_SMALL_NUMBER)
	{
		LatitudeRadians = Ecef.Z >= 0.0 ? UE_DOUBLE_PI * 0.5 : -UE_DOUBLE_PI * 0.5;
		LongitudeRadians = 0.0;
		HeightMeters = FMath::Abs(Ecef.Z) - SemiMinorAxisMeters;
	}
	else
	{
		LongitudeRadians = FMath::Atan2(Ecef.Y, Ecef.X);
		const double Theta = FMath::Atan2(
			Ecef.Z * WGS84SemiMajorAxisMeters,
			HorizontalRadius * SemiMinorAxisMeters);
		const double SinTheta = FMath::Sin(Theta);
		const double CosTheta = FMath::Cos(Theta);
		LatitudeRadians = FMath::Atan2(
			Ecef.Z + SecondEccentricitySquared * SemiMinorAxisMeters
				* SinTheta * SinTheta * SinTheta,
			HorizontalRadius - WGS84EccentricitySquared * WGS84SemiMajorAxisMeters
				* CosTheta * CosTheta * CosTheta);

		const double SinLatitude = FMath::Sin(LatitudeRadians);
		const double CosLatitude = FMath::Cos(LatitudeRadians);
		const double PrimeVerticalRadius = WGS84SemiMajorAxisMeters
			/ FMath::Sqrt(1.0 - WGS84EccentricitySquared * SinLatitude * SinLatitude);
		HeightMeters = FMath::Abs(CosLatitude) >= FMath::Abs(SinLatitude)
			? HorizontalRadius / CosLatitude - PrimeVerticalRadius
			: Ecef.Z / SinLatitude - PrimeVerticalRadius * (1.0 - WGS84EccentricitySquared);
	}

	OutGeodetic.LatitudeDegrees = LatitudeRadians * RadiansToDegrees;
	OutGeodetic.LongitudeDegrees = LongitudeRadians * RadiansToDegrees;
	OutGeodetic.EllipsoidHeightMeters = HeightMeters;
	if (!IsLatitudeValid(OutGeodetic.LatitudeDegrees)
		|| !IsLongitudeValid(OutGeodetic.LongitudeDegrees)
		|| !FMath::IsFinite(OutGeodetic.EllipsoidHeightMeters))
	{
		OutGeodetic = FGeodeticPosition{};
		OutError = TEXT("ECEF conversion produced an invalid geodetic coordinate.");
		return false;
	}
	return true;
}

bool RouteAssignmentGeo::BuildLocalEnuOriginContext(
	const double LatitudeDegrees,
	const double LongitudeDegrees,
	const double EllipsoidHeightMeters,
	FLocalEnuOriginContext& OutContext,
	FString& OutError)
{
	OutContext = FLocalEnuOriginContext{};
	OutError.Reset();
	if (!IsLatitudeValid(LatitudeDegrees)
		|| !IsLongitudeValid(LongitudeDegrees)
		|| !FMath::IsFinite(EllipsoidHeightMeters))
	{
		OutError = TEXT("Local ENU origin must contain finite, in-range geodetic coordinates.");
		return false;
	}

	const double LatitudeRadians = LatitudeDegrees * DegreesToRadians;
	const double LongitudeRadians = LongitudeDegrees * DegreesToRadians;
	OutContext.OriginEcef = GeodeticToEcef(
		LatitudeDegrees, LongitudeDegrees, EllipsoidHeightMeters);
	OutContext.LatitudeDegrees = LatitudeDegrees;
	OutContext.LongitudeDegrees = LongitudeDegrees;
	OutContext.EllipsoidHeightMeters = EllipsoidHeightMeters;
	OutContext.SinLatitude = FMath::Sin(LatitudeRadians);
	OutContext.CosLatitude = FMath::Cos(LatitudeRadians);
	OutContext.SinLongitude = FMath::Sin(LongitudeRadians);
	OutContext.CosLongitude = FMath::Cos(LongitudeRadians);
	OutContext.bIsValid = FMath::IsFinite(OutContext.OriginEcef.X)
		&& FMath::IsFinite(OutContext.OriginEcef.Y)
		&& FMath::IsFinite(OutContext.OriginEcef.Z);
	if (!OutContext.bIsValid)
	{
		OutContext = FLocalEnuOriginContext{};
		OutError = TEXT("Local ENU origin ECEF conversion failed.");
		return false;
	}
	return true;
}

bool RouteAssignmentGeo::EnuToEcef(
	const FLocalEnuOriginContext& Context,
	const double EastMeters,
	const double NorthMeters,
	const double UpMeters,
	FEcefPosition& OutEcef,
	FString& OutError)
{
	OutEcef = FEcefPosition{};
	OutError.Reset();
	if (!Context.bIsValid)
	{
		OutError = TEXT("Local ENU origin context is invalid.");
		return false;
	}
	if (!FMath::IsFinite(EastMeters)
		|| !FMath::IsFinite(NorthMeters)
		|| !FMath::IsFinite(UpMeters))
	{
		OutError = TEXT("Local ENU coordinates must be finite.");
		return false;
	}

	OutEcef.X = Context.OriginEcef.X
		- Context.SinLongitude * EastMeters
		- Context.SinLatitude * Context.CosLongitude * NorthMeters
		+ Context.CosLatitude * Context.CosLongitude * UpMeters;
	OutEcef.Y = Context.OriginEcef.Y
		+ Context.CosLongitude * EastMeters
		- Context.SinLatitude * Context.SinLongitude * NorthMeters
		+ Context.CosLatitude * Context.SinLongitude * UpMeters;
	OutEcef.Z = Context.OriginEcef.Z
		+ Context.CosLatitude * NorthMeters
		+ Context.SinLatitude * UpMeters;
	if (!FMath::IsFinite(OutEcef.X) || !FMath::IsFinite(OutEcef.Y) || !FMath::IsFinite(OutEcef.Z))
	{
		OutEcef = FEcefPosition{};
		OutError = TEXT("Local ENU conversion overflowed ECEF coordinates.");
		return false;
	}
	return true;
}

bool RouteAssignmentGeo::LocalEnuToGeodetic(
	const FLocalEnuOriginContext& Context,
	const double EastMeters,
	const double NorthMeters,
	const double UpMeters,
	FGeodeticPosition& OutGeodetic,
	FString& OutError)
{
	if (Context.bIsValid && EastMeters == 0.0 && NorthMeters == 0.0 && UpMeters == 0.0)
	{
		OutError.Reset();
		OutGeodetic.LatitudeDegrees = Context.LatitudeDegrees;
		OutGeodetic.LongitudeDegrees = Context.LongitudeDegrees;
		OutGeodetic.EllipsoidHeightMeters = Context.EllipsoidHeightMeters;
		return true;
	}
	FEcefPosition Ecef;
	return EnuToEcef(Context, EastMeters, NorthMeters, UpMeters, Ecef, OutError)
		&& EcefToGeodetic(Ecef, OutGeodetic, OutError);
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
