#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentContract.h"
#include "RouteAssignmentTypes.h"
#include "RouteAssignmentMatcher.generated.h"

namespace RouteAssignmentMatcher
{
	inline constexpr double ProductionMaxMatchDistanceMeters =
		RouteAssignmentContract::MaxHorizontalMatchDistanceMeters;
}

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentMatcherConfig
{
	GENERATED_BODY()

	/** Invalid by default so every caller must choose a distance contract explicitly. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Route Assignment")
	double MaxMatchDistanceMeters = 0.0;

	static FRouteAssignmentMatcherConfig Production()
	{
		FRouteAssignmentMatcherConfig Config;
		Config.MaxMatchDistanceMeters =
			RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters;
		return Config;
	}

	bool IsProductionContract() const
	{
		return MaxMatchDistanceMeters
			== RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters;
	}
};

USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentCandidateEdge
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotIndex = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double HorizontalDistanceMeters = 0.0;
};

/** Pure candidate graph; no edge in this value object is an assignment decision. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentCandidateGraph
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 EligibleDroneCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 CandidateEdgeCount = 0;

	/** Canonical DroneID order, including eligible drones with zero edges. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FString> EligibleDroneIDs;

	/** EdgeOffsets[i]..EdgeOffsets[i+1] is the edge range for EligibleDroneIDs[i]. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<int32> EdgeOffsets;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentCandidateEdge> Edges;
};

/** One edge selected as a maximum-cardinality witness, not a final assignment. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentMatchingPair
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotIndex = INDEX_NONE;
};

/**
 * Deterministic witness for maximum cardinality only.
 * This is not a final production assignment and carries no distance-cost decision.
 */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentMaximumMatchingResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 EligibleDroneCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 MaximumCardinality = 0;

	/** Canonical DroneID order; every pair is an input candidate edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentMatchingPair> WitnessPairs;
};

/** One original candidate edge selected by the minimum-cost optimal witness. */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentMinimumCostPair
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	FString DroneID;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotIndex = INDEX_NONE;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double HorizontalDistanceMeters = 0.0;
};

/**
 * Minimum-total-horizontal-distance witness at verified maximum cardinality.
 * This is not a final FRouteAssignmentEntry or authority materialization.
 */
USTRUCT(BlueprintType)
struct DRONESHOWPLATFORM1_API FRouteAssignmentMinimumCostMatchingResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 EligibleDroneCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 RouteSlotCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	int32 MatchedCardinality = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	double TotalHorizontalDistanceMeters = 0.0;

	/** Canonical DroneID order; every pair copies one original candidate edge. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Route Assignment")
	TArray<FRouteAssignmentMinimumCostPair> Pairs;
};

namespace RouteAssignmentMatcher
{
	/** Builds every legal horizontal edge without making any assignment decision. */
	DRONESHOWPLATFORM1_API bool BuildCandidateEdges(
		const FRouteAssignmentSourceSnapshot& SourceSnapshot,
		const TArray<FRouteAssignmentDroneLocalPosition>& DroneLocalPositions,
		const FRouteAssignmentMatcherConfig& Config,
		FRouteAssignmentCandidateGraph& OutGraph,
		FString& OutError);

	/**
	 * Solves sparse maximum cardinality only. Horizontal distance is validated as
	 * candidate-graph data but never used for traversal or optimization.
	 */
	DRONESHOWPLATFORM1_API bool SolveMaximumCardinality(
		const FRouteAssignmentCandidateGraph& CandidateGraph,
		FRouteAssignmentMaximumMatchingResult& OutResult,
		FString& OutError);

	/**
	 * Finds the minimum physical distance witness while preserving the verified
	 * maximum cardinality. The supplied maximum witness is never trusted blindly.
	 */
	DRONESHOWPLATFORM1_API bool SolveMinimumTotalDistance(
		const FRouteAssignmentCandidateGraph& CandidateGraph,
		const FRouteAssignmentMaximumMatchingResult& MaximumMatchingResult,
		FRouteAssignmentMinimumCostMatchingResult& OutResult,
		FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
	namespace Testing
	{
		/** Direct coverage for the production reduced-cost roundoff guard. */
		DRONESHOWPLATFORM1_API bool NormalizeReducedCost(
			double PhysicalCost,
			double FromPotential,
			double ToPotential,
			double& OutReducedCost,
			FString& OutError);
	}
#endif
}
