#pragma once

#include "CoreMinimal.h"

/** One live drone sampled in the fixed-pile local east/north frame. */
struct DRONESHOWPLATFORM1_API FGCSBenchmarkExportRow
{
	int32 DisplayID = INDEX_NONE;
	double EastMeters = 0.0;
	double NorthMeters = 0.0;
};

namespace GCSBenchmarkExport
{
	/**
	 * Builds the legacy BGC2 benchmark.txt payload.
	 *
	 * Rows are sorted by their effective display number. The physical fixed-pile
	 * row receives the locked origin latitude/longitude as E7 integers.
	 */
	DRONESHOWPLATFORM1_API bool BuildLegacyBenchmarkText(
		const TArray<FGCSBenchmarkExportRow>& Rows,
		int32 OriginDisplayID,
		double OriginLatitude,
		double OriginLongitude,
		FString& OutText,
		FString& OutError);
}
