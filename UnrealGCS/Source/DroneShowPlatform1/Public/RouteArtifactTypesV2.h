#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactTypes.h"

namespace RouteArtifactContractV2
{
	inline constexpr uint16 ArtifactSchemaVersion = 2;
}

/** Pure-route inputs that identify one immutable Route Artifact v2 build. */
struct DRONESHOWPLATFORM1_API FRouteArtifactBuildIdentityV2
{
	uint16 ArtifactSchemaVersion = RouteArtifactContractV2::ArtifactSchemaVersion;
	FRouteArtifactDigest256 SourceDatasetDigest;
	FString WaypointGeneratorVersion;
	FRouteArtifactDigest256 GeneratorInputDigest;
	int32 ShowOriginLatitudeE7 = 0;
	int32 ShowOriginLongitudeE7 = 0;
	int32 FrameCount = 0;
	int32 RouteSlotCount = 0;
	bool bHasRange = false;
	int32 RangeStart = 0;
	int32 RangeEnd = 0;

	bool Validate(FString& OutError) const;
};

namespace RouteArtifactIdentityV2
{
	/** Explicit, big-endian, length-prefixed UTF-8 canonical serialization. */
	DRONESHOWPLATFORM1_API bool SerializeCanonicalBuildIdentity(
		const FRouteArtifactBuildIdentityV2& Identity,
		TArray<uint8>& OutBytes,
		FString& OutError);

	/** 64 lowercase hex characters: SHA-256(canonical v2 build identity bytes). */
	DRONESHOWPLATFORM1_API bool ComputeArtifactSetID(
		const FRouteArtifactBuildIdentityV2& Identity,
		FString& OutArtifactSetID,
		FString& OutError);
}
