#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactManifest.h"
#include "RouteArtifactTypesV2.h"

/** Immutable pure-route Route Artifact v2 manifest. */
struct DRONESHOWPLATFORM1_API FRouteArtifactManifestV2
{
	uint16 ArtifactSchemaVersion = RouteArtifactContractV2::ArtifactSchemaVersion;
	FString ArtifactSetID;
	/** Unix epoch milliseconds; audit-only and excluded from ArtifactSetID. */
	int64 CreatedAtUtc = 0;

	FRouteArtifactSourceIdentity SourceIdentity;
	int32 FrameCount = 0;
	int32 RouteSlotCount = 0;
	bool bHasRange = false;
	int32 RangeStart = 0;
	int32 RangeEnd = 0;
	int32 ShowOriginLatitudeE7 = 0;
	int32 ShowOriginLongitudeE7 = 0;

	FString WaypointGeneratorVersion;
	FRouteArtifactDigest256 GeneratorInputDigest;
	TArray<FRouteArtifactRecord> Routes;
	FRouteArtifactDigest256 ArtifactSetDigest;
};

struct DRONESHOWPLATFORM1_API FRouteArtifactExpectedInputsV2
{
	FRouteArtifactBuildIdentityV2 BuildIdentity;
	FRouteArtifactSourceIdentity SourceIdentity;
};

namespace RouteArtifactManifestV2
{
	DRONESHOWPLATFORM1_API bool BuildIdentityFromManifest(
		const FRouteArtifactManifestV2& Manifest,
		FRouteArtifactBuildIdentityV2& OutIdentity,
		FString& OutError);
	DRONESHOWPLATFORM1_API bool ComputeArtifactSetDigest(
		const FRouteArtifactManifestV2& Manifest,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);
	DRONESHOWPLATFORM1_API bool SerializeCanonicalManifest(
		const FRouteArtifactManifestV2& Manifest,
		TArray<uint8>& OutUtf8Bytes,
		FString& OutError);
	DRONESHOWPLATFORM1_API bool ParseCanonicalManifest(
		const TArray<uint8>& Utf8Bytes,
		FRouteArtifactManifestV2& OutManifest,
		FString& OutError);
	DRONESHOWPLATFORM1_API bool ValidateManifestStructure(
		const FRouteArtifactManifestV2& Manifest,
		FString& OutError);
	DRONESHOWPLATFORM1_API bool ValidateManifestAgainstExpectedInputs(
		const FRouteArtifactManifestV2& Manifest,
		const FRouteArtifactExpectedInputsV2& Expected,
		FString& OutError);
}
