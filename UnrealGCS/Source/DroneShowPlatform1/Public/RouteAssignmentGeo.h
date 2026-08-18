#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentTypes.h"

namespace RouteAssignmentGeo
{
	inline constexpr double WGS84SemiMajorAxisMeters = 6378137.0;
	inline constexpr double WGS84InverseFlattening = 298.257223563;
	inline constexpr double WGS84Flattening = 1.0 / WGS84InverseFlattening;
	inline constexpr double WGS84EccentricitySquared =
		WGS84Flattening * (2.0 - WGS84Flattening);

	struct FEcefPosition
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
	};

	struct FGeodeticPosition
	{
		double LatitudeDegrees = 0.0;
		double LongitudeDegrees = 0.0;
		double EllipsoidHeightMeters = 0.0;
	};

	/**
	 * Precomputed Local ENU origin. Build once, then share read-only across route
	 * workers; it contains no route-specific mutable state.
	 */
	struct FLocalEnuOriginContext
	{
		FEcefPosition OriginEcef;
		double LatitudeDegrees = 0.0;
		double LongitudeDegrees = 0.0;
		double EllipsoidHeightMeters = 0.0;
		double SinLatitude = 0.0;
		double CosLatitude = 1.0;
		double SinLongitude = 0.0;
		double CosLongitude = 1.0;
		bool bIsValid = false;
	};

	/** Converts WGS84 geodetic coordinates to ECEF meters using double precision. */
	DRONESHOWPLATFORM1_API FEcefPosition GeodeticToEcef(
		double LatitudeDegrees,
		double LongitudeDegrees,
		double EllipsoidHeightMeters);

	/** Inverts WGS84 ECEF using double precision; degenerate near-center input fails. */
	DRONESHOWPLATFORM1_API bool EcefToGeodetic(
		const FEcefPosition& Ecef,
		FGeodeticPosition& OutGeodetic,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool BuildLocalEnuOriginContext(
		double LatitudeDegrees,
		double LongitudeDegrees,
		double EllipsoidHeightMeters,
		FLocalEnuOriginContext& OutContext,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool EnuToEcef(
		const FLocalEnuOriginContext& Context,
		double EastMeters,
		double NorthMeters,
		double UpMeters,
		FEcefPosition& OutEcef,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool LocalEnuToGeodetic(
		const FLocalEnuOriginContext& Context,
		double EastMeters,
		double NorthMeters,
		double UpMeters,
		FGeodeticPosition& OutGeodetic,
		FString& OutError);

	/** Builds one horizontal Local EN value per input drone without mutating the source snapshot. */
	DRONESHOWPLATFORM1_API bool BuildDroneLocalPositions(
		const FRouteAssignmentSourceSnapshot& SourceSnapshot,
		TArray<FRouteAssignmentDroneLocalPosition>& OutPositions,
		FString& OutError);
}
