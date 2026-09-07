#pragma once

#include "CoreMinimal.h"

/** One drone projected into a horizontal, meter-based local frame. */
struct FHoverLayerPosition
{
	FString DroneID;
	double EastMeters = 0.0;
	double NorthMeters = 0.0;
};

/** Frozen result of one hover-layer generation pass. */
struct FHoverLayerPlan
{
	double MinimumSpacingMeters = 0.6;
	TArray<FString> BaseDroneIDs;
	TArray<TArray<FString>> Layers;
};

namespace HoverLayerSelection
{
	/**
	 * Deterministic greedy coloring compatible with the legacy ground station:
	 * drones in the same layer are always farther apart than the safety spacing.
	 */
	DRONESHOWPLATFORM1_API bool BuildPlan(
		const TArray<FHoverLayerPosition>& Positions,
		double MinimumSpacingMeters,
		FHoverLayerPlan& OutPlan,
		FString& OutError);

	/** Re-checks one frozen layer against current positions before a command. */
	DRONESHOWPLATFORM1_API bool ValidateLayerSpacing(
		const TArray<FHoverLayerPosition>& Positions,
		double MinimumSpacingMeters,
		FString& OutError);
}
