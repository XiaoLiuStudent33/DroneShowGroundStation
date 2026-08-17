#include "RouteAssignmentPipeline.h"

#include "RouteAssignmentGeo.h"
#include "RouteAssignmentMatcher.h"
#include "RouteAssignmentMaterializer.h"

namespace
{
	bool FailStage(
		const TCHAR* Stage,
		const FString& StageError,
		FRouteAssignmentCandidate& OutCandidate,
		FString& OutError)
	{
		OutCandidate = FRouteAssignmentCandidate{};
		OutError = FString::Printf(TEXT("%s: %s"), Stage, *StageError);
		return false;
	}
}

bool RouteAssignmentPipeline::BuildCandidateFromSourceSnapshot(
	const FRouteAssignmentSourceSnapshot& SourceSnapshot,
	FRouteAssignmentCandidate& OutCandidate,
	FString& OutError)
{
	OutCandidate = FRouteAssignmentCandidate{};
	OutError.Reset();

	FString StageError;
	TArray<FRouteAssignmentDroneLocalPosition> DroneLocalPositions;
	if (!RouteAssignmentGeo::BuildDroneLocalPositions(
		SourceSnapshot, DroneLocalPositions, StageError))
	{
		return FailStage(TEXT("Geo"), StageError, OutCandidate, OutError);
	}

	FRouteAssignmentCandidateGraph CandidateGraph;
	if (!RouteAssignmentMatcher::BuildCandidateEdges(
		SourceSnapshot,
		DroneLocalPositions,
		FRouteAssignmentMatcherConfig::Production(),
		CandidateGraph,
		StageError))
	{
		return FailStage(TEXT("CandidateGraph"), StageError, OutCandidate, OutError);
	}

	FRouteAssignmentMaximumMatchingResult MaximumMatching;
	if (!RouteAssignmentMatcher::SolveMaximumCardinality(
		CandidateGraph, MaximumMatching, StageError))
	{
		return FailStage(TEXT("MaximumMatching"), StageError, OutCandidate, OutError);
	}

	FRouteAssignmentMinimumCostMatchingResult MinimumCostMatching;
	if (!RouteAssignmentMatcher::SolveMinimumTotalDistance(
		CandidateGraph, MaximumMatching, MinimumCostMatching, StageError))
	{
		return FailStage(TEXT("MinimumCost"), StageError, OutCandidate, OutError);
	}

	if (!RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		SourceSnapshot,
		DroneLocalPositions,
		CandidateGraph,
		MinimumCostMatching,
		OutCandidate,
		StageError))
	{
		return FailStage(TEXT("Materializer"), StageError, OutCandidate, OutError);
	}

	return true;
}
