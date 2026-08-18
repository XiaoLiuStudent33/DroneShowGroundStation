#pragma once

#include "CoreMinimal.h"

#include <array>

namespace RouteArtifactContract
{
	inline constexpr uint16 ArtifactSchemaVersion = 1;
}

/** Fixed-size SHA-256 value used only by the Route Artifact domain. */
struct DRONESHOWPLATFORM1_API FRouteArtifactDigest256
{
	static constexpr int32 ByteCount = 32;
	static constexpr int32 LowerHexCharacterCount = ByteCount * 2;

	bool IsValid() const { return bIsSet; }
	void Reset();
	FString ToLowerHex() const;
	const uint8* GetData() const { return Bytes.data(); }
	int32 Num() const { return ByteCount; }

	static bool ParseLowerHex(
		const FString& LowerHex,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);
	static bool FromBytes(
		const uint8* Data,
		int32 DataBytes,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);

	bool operator==(const FRouteArtifactDigest256& Other) const
	{
		return bIsSet == Other.bIsSet && Bytes == Other.Bytes;
	}
	bool operator!=(const FRouteArtifactDigest256& Other) const
	{
		return !(*this == Other);
	}

private:
	std::array<uint8, ByteCount> Bytes{};
	bool bIsSet = false;
};

/**
 * Incremental Artifact-domain SHA-256 builder backed by the platform crypto
 * provider. It has no Frozen Assignment payload-size or wire-domain ceiling.
 */
class DRONESHOWPLATFORM1_API FRouteArtifactSha256Builder final
{
public:
	FRouteArtifactSha256Builder();
	~FRouteArtifactSha256Builder();

	FRouteArtifactSha256Builder(const FRouteArtifactSha256Builder&) = delete;
	FRouteArtifactSha256Builder& operator=(const FRouteArtifactSha256Builder&) = delete;

	bool Initialize(FString& OutError);
	bool Update(const void* Data, uint64 ByteCount, FString& OutError);
	bool Finalize(FRouteArtifactDigest256& OutDigest, FString& OutError);

private:
	struct FImpl;
	TUniquePtr<FImpl> Impl;
};

class DRONESHOWPLATFORM1_API FRouteArtifactSha256 final
{
public:
	/** Technical IO buffer size only; not an Artifact business limit. */
	static constexpr int32 StreamingBufferBytes = 64 * 1024;

	static bool HashBytes(
		const TArray<uint8>& Bytes,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);
	static bool HashBytes(
		const void* Data,
		uint64 ByteCount,
		FRouteArtifactDigest256& OutDigest,
		FString& OutError);

	/** Hashes one opened file handle incrementally and returns bytes actually read. */
	static bool HashFileStreaming(
		const FString& FilePath,
		FRouteArtifactDigest256& OutDigest,
		int64& OutFileByteSize,
		FString& OutError);
};

/** Exact Frozen Assignment authority binding consumed by an Artifact build. */
struct DRONESHOWPLATFORM1_API FRouteArtifactAssignmentBinding
{
	FString FleetSessionID;
	FString AssignmentAuthoritySessionID;
	int64 AssignmentRevision = 0;
	FRouteArtifactDigest256 AssignmentDigest;

	bool Validate(FString& OutError) const;
};

/** Source identity captured from the exact NPZ bytes parsed by Route Preview. */
struct DRONESHOWPLATFORM1_API FRouteArtifactSourceIdentity
{
	FRouteArtifactDigest256 SourceDatasetDigest;
	int64 SourceByteSize = 0;
	FString SourceDisplayFileName;

	bool IsValid() const;
	void Reset();

	/** Retains only the clean basename from SourcePath. */
	static bool Create(
		const FRouteArtifactDigest256& Digest,
		int64 ByteSize,
		const FString& SourcePath,
		FRouteArtifactSourceIdentity& OutIdentity,
		FString& OutError);
};

/** Deterministic inputs that identify one immutable Artifact build. */
struct DRONESHOWPLATFORM1_API FRouteArtifactBuildIdentity
{
	uint16 ArtifactSchemaVersion = RouteArtifactContract::ArtifactSchemaVersion;
	FRouteArtifactAssignmentBinding AssignmentBinding;
	FRouteArtifactDigest256 SourceDatasetDigest;
	FString WaypointGeneratorVersion;
	FRouteArtifactDigest256 GeneratorInputDigest;

	bool Validate(FString& OutError) const;
};

namespace RouteArtifactIdentity
{
	/** Explicit, big-endian, length-prefixed UTF-8 canonical serialization. */
	DRONESHOWPLATFORM1_API bool SerializeCanonicalBuildIdentity(
		const FRouteArtifactBuildIdentity& Identity,
		TArray<uint8>& OutBytes,
		FString& OutError);

	/** 64 lowercase hex characters: SHA-256(canonical build identity bytes). */
	DRONESHOWPLATFORM1_API bool ComputeArtifactSetID(
		const FRouteArtifactBuildIdentity& Identity,
		FString& OutArtifactSetID,
		FString& OutError);
}
