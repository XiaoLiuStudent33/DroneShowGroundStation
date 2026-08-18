#pragma once

#include "CoreMinimal.h"
#include "Math/Float16Color.h"
#include "RouteArtifactTypes.h"

/**
 * Compact, frame-major route data loaded once from a legacy show NPZ.
 *
 * RouteSlotIndex is preview-only identity. It is deliberately not a Fleet
 * DroneID and never creates a DroneSession or FleetManager entry.
 */
struct DRONESHOWPLATFORM1_API FRoutePreviewData
{
	int32 FrameCount = 0;
	int32 DroneCount = 0;

	/** Unknown until supplied by authoritative external metadata. */
	double SampleRateHz = 0.0;

	double ShowOriginLatitude = 0.0;
	double ShowOriginLongitude = 0.0;
	double ShowOriginAltitude = 0.0;
	bool bShowOriginSet = false;

	/** SHA-256 identity of the exact original NPZ bytes parsed for this data. */
	FRouteArtifactSourceIdentity SourceIdentity;

	/** Frame-major Local ENU meters: FrameIndex * DroneCount + RouteSlotIndex. */
	TArray<FVector3f> PositionsENU;

	/** Frame-major compact linear RGBA. Preview output always uses alpha 1.0. */
	TArray<FFloat16Color> Colors;

	int32 OptionalRangeStart = INDEX_NONE;
	int32 OptionalRangeEnd = INDEX_NONE;

	bool IsLoaded() const
	{
		const int64 ExpectedSamples = static_cast<int64>(FrameCount) * DroneCount;
		return FrameCount > 0
			&& DroneCount > 0
			&& SourceIdentity.IsValid()
			&& PositionsENU.Num() == ExpectedSamples
			&& Colors.Num() == ExpectedSamples;
	}

	bool IsValidSample(const int32 FrameIndex, const int32 RouteSlotIndex) const
	{
		return IsLoaded()
			&& FrameIndex >= 0
			&& FrameIndex < FrameCount
			&& RouteSlotIndex >= 0
			&& RouteSlotIndex < DroneCount;
	}

	int64 GetFlatIndex(const int32 FrameIndex, const int32 RouteSlotIndex) const
	{
		return static_cast<int64>(FrameIndex) * DroneCount + RouteSlotIndex;
	}

	const FVector3f& GetPosition(const int32 FrameIndex, const int32 RouteSlotIndex) const
	{
		check(IsValidSample(FrameIndex, RouteSlotIndex));
		return PositionsENU[GetFlatIndex(FrameIndex, RouteSlotIndex)];
	}

	FLinearColor GetColor(const int32 FrameIndex, const int32 RouteSlotIndex) const
	{
		check(IsValidSample(FrameIndex, RouteSlotIndex));
		FLinearColor Color = Colors[GetFlatIndex(FrameIndex, RouteSlotIndex)].GetFloats();
		Color.R = FMath::Clamp(Color.R, 0.0f, 1.0f);
		Color.G = FMath::Clamp(Color.G, 0.0f, 1.0f);
		Color.B = FMath::Clamp(Color.B, 0.0f, 1.0f);
		Color.A = 1.0f;
		return Color;
	}

	uint64 GetPositionMemoryBytes() const
	{
		return static_cast<uint64>(PositionsENU.GetAllocatedSize());
	}

	uint64 GetColorMemoryBytes() const
	{
		return static_cast<uint64>(Colors.GetAllocatedSize());
	}
};
