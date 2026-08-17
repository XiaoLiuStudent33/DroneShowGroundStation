#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentMatcher.h"
#include "RouteAssignmentTypes.h"

namespace RouteAssignmentMaterializer
{
	/**
	 * Converts one already-optimal matching witness into a deterministic business
	 * candidate. This pure function does not mutate assignment authority state.
	 */
	DRONESHOWPLATFORM1_API bool MaterializeAssignmentCandidate(
		const FRouteAssignmentSourceSnapshot& SourceSnapshot,
		const TArray<FRouteAssignmentDroneLocalPosition>& DroneLocalPositions,
		const FRouteAssignmentCandidateGraph& CandidateGraph,
		const FRouteAssignmentMinimumCostMatchingResult& OptimalMatching,
		FRouteAssignmentCandidate& OutCandidate,
		FString& OutError);
}
