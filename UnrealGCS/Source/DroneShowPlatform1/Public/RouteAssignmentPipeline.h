#pragma once

#include "CoreMinimal.h"
#include "RouteAssignmentTypes.h"

namespace RouteAssignmentPipeline
{
	/**
	 * Runs the pure production Geo/Graph/Matching/Materialization chain against
	 * one immutable source snapshot. This function never reads or mutates a subsystem.
	 */
	DRONESHOWPLATFORM1_API bool BuildCandidateFromSourceSnapshot(
		const FRouteAssignmentSourceSnapshot& SourceSnapshot,
		FRouteAssignmentCandidate& OutCandidate,
		FString& OutError);
}
