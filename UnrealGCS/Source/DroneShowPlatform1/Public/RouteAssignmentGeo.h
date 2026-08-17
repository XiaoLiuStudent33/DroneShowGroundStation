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

	/** Converts WGS84 geodetic coordinates to ECEF meters using double precision. */
	DRONESHOWPLATFORM1_API FEcefPosition GeodeticToEcef(
		double LatitudeDegrees,
		double LongitudeDegrees,
		double EllipsoidHeightMeters);

	/** Builds one horizontal Local EN value per input drone without mutating the source snapshot. */
	DRONESHOWPLATFORM1_API bool BuildDroneLocalPositions(
		const FRouteAssignmentSourceSnapshot& SourceSnapshot,
		TArray<FRouteAssignmentDroneLocalPosition>& OutPositions,
		FString& OutError);
}
