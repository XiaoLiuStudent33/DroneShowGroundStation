#pragma once

#include "CoreMinimal.h"
#include "RouteWaypointGeneratorTypes.h"

class FRouteArtifactWorkspaceManager;

namespace RouteWaypointGenerator
{
	/** Normalizes raw origin to toward-zero E7 and computes the v1 input digest. */
	DRONESHOWPLATFORM1_API bool BuildContext(
		double ShowOriginLatitude,
		double ShowOriginLongitude,
		FRouteWaypointGeneratorContext& OutContext,
		FRouteWaypointGeneratorError& OutError);

	/** Locale-independent fixed-E7 formatter, including safe INT64_MIN handling. */
	DRONESHOWPLATFORM1_API FString FormatScaledDecimal(int64 ScaledValue);

	/** Formats exactly 12 tab-delimited fields and never appends a newline. */
	DRONESHOWPLATFORM1_API bool FormatWaypointRow(
		int32 Sequence,
		uint8 Red,
		uint8 Green,
		uint8 Blue,
		int32 LatitudeE7,
		int32 LongitudeE7,
		int64 AltitudeE7,
		TArray<uint8>& OutRowBytes,
		FRouteWaypointGeneratorError& OutError);

	/** Generates exactly one route into a caller-owned sink. */
	DRONESHOWPLATFORM1_API FRouteWaypointGenerationResult GenerateRoute(
		const FRouteWaypointGeneratorInput& Input,
		IRouteWaypointByteSink& Sink,
		const TFunction<bool()>& IsCancelled = TFunction<bool()>());

	/** Production adapter: resolves only the active Workspace staging route path. */
	DRONESHOWPLATFORM1_API FRouteWaypointGenerationResult GenerateRouteToStagingFile(
		FRouteArtifactWorkspaceManager& WorkspaceManager,
		const FRouteWaypointGeneratorInput& Input,
		const TFunction<bool()>& IsCancelled = TFunction<bool()>(),
		FRouteWaypointFileActivityTracker* FileActivityTracker = nullptr);
}
