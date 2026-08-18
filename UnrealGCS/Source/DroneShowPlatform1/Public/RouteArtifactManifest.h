#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactTypes.h"
#include "RouteAssignmentTypes.h"

/** Frozen Assignment projection retained for audit, not a second authority. */
struct DRONESHOWPLATFORM1_API FRouteArtifactMatchedMapping
{
	FString DroneID;
	int32 RouteSlotIndex = INDEX_NONE;
};

/** Integrity identity for one zero-based RouteSlot Artifact file. */
struct DRONESHOWPLATFORM1_API FRouteArtifactRecord
{
	int32 RouteSlotIndex = INDEX_NONE;
	FString RelativePath;
	int64 FileByteSize = 0;
	FRouteArtifactDigest256 RouteArtifactDigest;
};

/** Immutable and customer-auditable description of one ArtifactSet instance. */
struct DRONESHOWPLATFORM1_API FRouteArtifactManifest
{
	uint16 ArtifactSchemaVersion = RouteArtifactContract::ArtifactSchemaVersion;
	FString ArtifactSetID;
	/** Unix epoch milliseconds; audit-only and excluded from ArtifactSetID. */
	int64 CreatedAtUtc = 0;

	FRouteArtifactAssignmentBinding AssignmentBinding;
	int64 FleetRevisionAtCapture = 0;
	int64 RouteDatasetGeneration = 0;
	ERouteAssignmentCompletionMode CompletionMode = ERouteAssignmentCompletionMode::Unset;
	ERouteAssignmentFreezePolicy FreezePolicy = ERouteAssignmentFreezePolicy::Unset;
	int32 MatchedCount = 0;
	int32 SpareCount = 0;
	int32 ExcludedCount = 0;
	int32 VacantCount = 0;

	FRouteArtifactSourceIdentity SourceIdentity;
	int32 FrameCount = 0;
	int32 RouteSlotCount = 0;
	bool bHasRange = false;
	int32 RangeStart = 0;
	int32 RangeEnd = 0;

	FString WaypointGeneratorVersion;
	FRouteArtifactDigest256 GeneratorInputDigest;
	TArray<FRouteArtifactMatchedMapping> MatchedMapping;
	TArray<FRouteArtifactRecord> Routes;
	FRouteArtifactDigest256 ArtifactSetDigest;
};

/** Current authorities/generator identity expected by a Manifest consumer. */
struct DRONESHOWPLATFORM1_API FRouteArtifactExpectedInputs
{
	FRouteArtifactBuildIdentity BuildIdentity;
	FRouteArtifactSourceIdentity SourceIdentity;
};

namespace RouteArtifactManifest
{
	inline constexpr const TCHAR* ManifestFilename = TEXT("manifest.json");

	DRONESHOWPLATFORM1_API bool BuildIdentityFromManifest(
		const FRouteArtifactManifest& Manifest,
		FRouteArtifactBuildIdentity& OutIdentity,
		FString& OutError);

	/** SHA-256 of canonical protected fields with ArtifactSetDigest omitted. */
	DRONESHOWPLATFORM1_API bool ComputeArtifactSetDigest(
		const FRouteArtifactManifest& Manifest,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);

	/** Compact UTF-8 JSON, no BOM/newline, explicit field order. */
	DRONESHOWPLATFORM1_API bool SerializeCanonicalManifest(
		const FRouteArtifactManifest& Manifest,
		TArray<uint8>& OutUtf8Bytes,
		FString& OutError);

	/** Strict schema parse plus byte-for-byte canonical reserialization check. */
	DRONESHOWPLATFORM1_API bool ParseCanonicalManifest(
		const TArray<uint8>& Utf8Bytes,
		FRouteArtifactManifest& OutManifest,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool ValidateManifestStructure(
		const FRouteArtifactManifest& Manifest,
		FString& OutError);

	DRONESHOWPLATFORM1_API bool ValidateManifestAgainstExpectedInputs(
		const FRouteArtifactManifest& Manifest,
		const FRouteArtifactExpectedInputs& Expected,
		FString& OutError);

	/** Validates expected route files only; the Workspace layer owns the strict extra-file allowlist. */
	DRONESHOWPLATFORM1_API bool ValidateArtifactFiles(
		const FString& ArtifactRoot,
		const FRouteArtifactManifest& Manifest,
		FString& OutError);
}
