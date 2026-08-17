#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"

#include "RouteAssignmentMatcher.h"

#include <cmath>
#include <limits>

namespace
{
	FRouteAssignmentMatcherConfig ProductionConfig()
	{
		return FRouteAssignmentMatcherConfig::Production();
	}

	void AddDrone(
		FRouteAssignmentSourceSnapshot& Snapshot,
		TArray<FRouteAssignmentDroneLocalPosition>& LocalPositions,
		const TCHAR* DroneID,
		const double EastMeters,
		const double NorthMeters,
		const bool bEligible = true)
	{
		FRouteAssignmentDroneSource SourceDrone;
		SourceDrone.DroneID = DroneID;
		SourceDrone.AltitudeAMSL = 5000.0f;
		SourceDrone.bAltitudeValid = false;
		Snapshot.Drones.Add(SourceDrone);

		FRouteAssignmentDroneLocalPosition LocalPosition;
		LocalPosition.DroneID = DroneID;
		LocalPosition.EastMeters = EastMeters;
		LocalPosition.NorthMeters = NorthMeters;
		LocalPosition.bAssignmentGeoEligible = bEligible;
		LocalPositions.Add(LocalPosition);
	}

	void AddRoute(
		FRouteAssignmentSourceSnapshot& Snapshot,
		const int32 RouteSlotIndex,
		const double EastMeters,
		const double NorthMeters,
		const double UpMeters = 0.0)
	{
		FRouteAssignmentRouteSlotSource Route;
		Route.RouteSlotIndex = RouteSlotIndex;
		Route.EastMeters = EastMeters;
		Route.NorthMeters = NorthMeters;
		Route.UpMeters = UpMeters;
		Snapshot.RouteSlots.Add(Route);
	}

	bool GraphsEquivalent(
		const FRouteAssignmentCandidateGraph& Left,
		const FRouteAssignmentCandidateGraph& Right)
	{
		if (Left.EligibleDroneCount != Right.EligibleDroneCount
			|| Left.RouteSlotCount != Right.RouteSlotCount
			|| Left.CandidateEdgeCount != Right.CandidateEdgeCount
			|| Left.EligibleDroneIDs != Right.EligibleDroneIDs
			|| Left.EdgeOffsets != Right.EdgeOffsets
			|| Left.Edges.Num() != Right.Edges.Num())
		{
			return false;
		}

		for (int32 EdgeIndex = 0; EdgeIndex < Left.Edges.Num(); ++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& LeftEdge = Left.Edges[EdgeIndex];
			const FRouteAssignmentCandidateEdge& RightEdge = Right.Edges[EdgeIndex];
			if (LeftEdge.DroneID != RightEdge.DroneID
				|| LeftEdge.RouteSlotIndex != RightEdge.RouteSlotIndex
				|| LeftEdge.HorizontalDistanceMeters != RightEdge.HorizontalDistanceMeters)
			{
				return false;
			}
		}
		return true;
	}

	FRouteAssignmentCandidateGraph MakeCandidateGraph(
		const TArray<FString>& DroneIDs,
		const int32 RouteSlotCount,
		const TArray<TArray<int32>>& Adjacency,
		const TArray<TArray<double>>* Distances = nullptr)
	{
		FRouteAssignmentCandidateGraph Graph;
		Graph.EligibleDroneCount = DroneIDs.Num();
		Graph.RouteSlotCount = RouteSlotCount;
		Graph.EligibleDroneIDs = DroneIDs;
		Graph.EdgeOffsets.Reserve(DroneIDs.Num() + 1);
		for (int32 DroneIndex = 0; DroneIndex < DroneIDs.Num(); ++DroneIndex)
		{
			Graph.EdgeOffsets.Add(Graph.Edges.Num());
			for (int32 AdjacencyIndex = 0;
				AdjacencyIndex < Adjacency[DroneIndex].Num();
				++AdjacencyIndex)
			{
				FRouteAssignmentCandidateEdge Edge;
				Edge.DroneID = DroneIDs[DroneIndex];
				Edge.RouteSlotIndex = Adjacency[DroneIndex][AdjacencyIndex];
				Edge.HorizontalDistanceMeters = Distances
					? (*Distances)[DroneIndex][AdjacencyIndex]
					: 0.10;
				Graph.Edges.Add(MoveTemp(Edge));
			}
		}
		Graph.EdgeOffsets.Add(Graph.Edges.Num());
		Graph.CandidateEdgeCount = Graph.Edges.Num();
		return Graph;
	}

	bool MatchingResultsEquivalent(
		const FRouteAssignmentMaximumMatchingResult& Left,
		const FRouteAssignmentMaximumMatchingResult& Right)
	{
		if (Left.EligibleDroneCount != Right.EligibleDroneCount
			|| Left.RouteSlotCount != Right.RouteSlotCount
			|| Left.MaximumCardinality != Right.MaximumCardinality
			|| Left.WitnessPairs.Num() != Right.WitnessPairs.Num())
		{
			return false;
		}

		for (int32 PairIndex = 0; PairIndex < Left.WitnessPairs.Num(); ++PairIndex)
		{
			if (Left.WitnessPairs[PairIndex].DroneID
					!= Right.WitnessPairs[PairIndex].DroneID
				|| Left.WitnessPairs[PairIndex].RouteSlotIndex
					!= Right.WitnessPairs[PairIndex].RouteSlotIndex)
			{
				return false;
			}
		}
		return true;
	}

	bool IsInputEdge(
		const FRouteAssignmentCandidateGraph& Graph,
		const FRouteAssignmentMatchingPair& Pair)
	{
		const int32 DroneIndex = Graph.EligibleDroneIDs.IndexOfByKey(Pair.DroneID);
		if (DroneIndex == INDEX_NONE)
		{
			return false;
		}

		for (int32 EdgeIndex = Graph.EdgeOffsets[DroneIndex];
			EdgeIndex < Graph.EdgeOffsets[DroneIndex + 1];
			++EdgeIndex)
		{
			if (Graph.Edges[EdgeIndex].RouteSlotIndex == Pair.RouteSlotIndex)
			{
				return true;
			}
		}
		return false;
	}

	bool IsInputEdge(
		const FRouteAssignmentCandidateGraph& Graph,
		const FRouteAssignmentMinimumCostPair& Pair)
	{
		const int32 DroneIndex = Graph.EligibleDroneIDs.IndexOfByKey(Pair.DroneID);
		if (DroneIndex == INDEX_NONE)
		{
			return false;
		}

		for (int32 EdgeIndex = Graph.EdgeOffsets[DroneIndex];
			EdgeIndex < Graph.EdgeOffsets[DroneIndex + 1];
			++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& Edge = Graph.Edges[EdgeIndex];
			if (Edge.RouteSlotIndex == Pair.RouteSlotIndex
				&& Edge.HorizontalDistanceMeters == Pair.HorizontalDistanceMeters)
			{
				return true;
			}
		}
		return false;
	}

	bool SolveMinimumForTest(
		const FRouteAssignmentCandidateGraph& Graph,
		FRouteAssignmentMinimumCostMatchingResult& OutResult,
		FString& OutError)
	{
		FRouteAssignmentMaximumMatchingResult MaximumResult;
		if (!RouteAssignmentMatcher::SolveMaximumCardinality(
			Graph,
			MaximumResult,
			OutError))
		{
			return false;
		}
		return RouteAssignmentMatcher::SolveMinimumTotalDistance(
			Graph,
			MaximumResult,
			OutResult,
			OutError);
	}

	bool MinimumResultsEquivalent(
		const FRouteAssignmentMinimumCostMatchingResult& Left,
		const FRouteAssignmentMinimumCostMatchingResult& Right)
	{
		if (Left.EligibleDroneCount != Right.EligibleDroneCount
			|| Left.RouteSlotCount != Right.RouteSlotCount
			|| Left.MatchedCardinality != Right.MatchedCardinality
			|| Left.TotalHorizontalDistanceMeters != Right.TotalHorizontalDistanceMeters
			|| Left.Pairs.Num() != Right.Pairs.Num())
		{
			return false;
		}

		for (int32 PairIndex = 0; PairIndex < Left.Pairs.Num(); ++PairIndex)
		{
			const FRouteAssignmentMinimumCostPair& LeftPair = Left.Pairs[PairIndex];
			const FRouteAssignmentMinimumCostPair& RightPair = Right.Pairs[PairIndex];
			if (LeftPair.DroneID != RightPair.DroneID
				|| LeftPair.RouteSlotIndex != RightPair.RouteSlotIndex
				|| LeftPair.HorizontalDistanceMeters != RightPair.HorizontalDistanceMeters)
			{
				return false;
			}
		}
		return true;
	}

	int32 FindMatchedRoute(
		const FRouteAssignmentMinimumCostMatchingResult& Result,
		const FString& DroneID)
	{
		for (const FRouteAssignmentMinimumCostPair& Pair : Result.Pairs)
		{
			if (Pair.DroneID == DroneID)
			{
				return Pair.RouteSlotIndex;
			}
		}
		return INDEX_NONE;
	}

	double CompensatedSum(const TArray<double>& Values)
	{
		double Sum = 0.0;
		double Compensation = 0.0;
		for (const double Value : Values)
		{
			const double Next = Sum + Value;
			if (FMath::Abs(Sum) >= FMath::Abs(Value))
			{
				Compensation += (Sum - Next) + Value;
			}
			else
			{
				Compensation += (Value - Next) + Sum;
			}
			Sum = Next;
		}
		return Sum + Compensation;
	}

	struct FBruteForceOptimum
	{
		int32 MaximumCardinality = -1;
		double MinimumTotalDistanceMeters =
			std::numeric_limits<double>::infinity();
	};

	void EnumerateMatchings(
		const FRouteAssignmentCandidateGraph& Graph,
		const int32 DroneIndex,
		TArray<bool>& UsedRoutes,
		TArray<double>& SelectedCosts,
		FBruteForceOptimum& InOutOptimum)
	{
		if (DroneIndex == Graph.EligibleDroneCount)
		{
			const int32 Cardinality = SelectedCosts.Num();
			const double Total = CompensatedSum(SelectedCosts);
			if (Cardinality > InOutOptimum.MaximumCardinality
				|| (Cardinality == InOutOptimum.MaximumCardinality
					&& Total < InOutOptimum.MinimumTotalDistanceMeters))
			{
				InOutOptimum.MaximumCardinality = Cardinality;
				InOutOptimum.MinimumTotalDistanceMeters = Total;
			}
			return;
		}

		EnumerateMatchings(
			Graph,
			DroneIndex + 1,
			UsedRoutes,
			SelectedCosts,
			InOutOptimum);
		for (int32 EdgeIndex = Graph.EdgeOffsets[DroneIndex];
			EdgeIndex < Graph.EdgeOffsets[DroneIndex + 1];
			++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& Edge = Graph.Edges[EdgeIndex];
			if (UsedRoutes[Edge.RouteSlotIndex])
			{
				continue;
			}
			UsedRoutes[Edge.RouteSlotIndex] = true;
			SelectedCosts.Add(Edge.HorizontalDistanceMeters);
			EnumerateMatchings(
				Graph,
				DroneIndex + 1,
				UsedRoutes,
				SelectedCosts,
				InOutOptimum);
			SelectedCosts.Pop();
			UsedRoutes[Edge.RouteSlotIndex] = false;
		}
	}

	FBruteForceOptimum SolveBruteForceOracle(
		const FRouteAssignmentCandidateGraph& Graph)
	{
		FBruteForceOptimum Optimum;
		TArray<bool> UsedRoutes;
		UsedRoutes.Init(false, Graph.RouteSlotCount);
		TArray<double> SelectedCosts;
		EnumerateMatchings(Graph, 0, UsedRoutes, SelectedCosts, Optimum);
		return Optimum;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateSamePointTest,
	"DroneGCS.RouteAssignment.CandidateEdges.01SamePoint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateSamePointTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 10.0, 20.0);
	AddRoute(Snapshot, 0, 10.0, 20.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	if (!TestEqual(TEXT("One edge"), Graph.Edges.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Distance is exactly zero"), Graph.Edges[0].HorizontalDistanceMeters, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateExactBoundaryTest,
	"DroneGCS.RouteAssignment.CandidateEdges.02ExactBoundaryIncluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateExactBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.20, 0.0);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	if (!TestEqual(TEXT("Exact boundary edge included"), Graph.Edges.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Stored distance is exact boundary"),
		Graph.Edges[0].HorizontalDistanceMeters,
		RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateBelowBoundaryTest,
	"DroneGCS.RouteAssignment.CandidateEdges.03JustBelowBoundaryIncluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateBelowBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double JustBelow = std::nextafter(
		RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters,
		0.0);
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), JustBelow, 0.0);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestEqual(TEXT("Next representable value below boundary included"), Graph.Edges.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateAboveBoundaryTest,
	"DroneGCS.RouteAssignment.CandidateEdges.04JustAboveBoundaryExcluded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateAboveBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double JustAbove = std::nextafter(
		RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters,
		std::numeric_limits<double>::infinity());
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), JustAbove, 0.0);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestTrue(TEXT("Next representable value above boundary excluded"), Graph.Edges.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateMultipleTest,
	"DroneGCS.RouteAssignment.CandidateEdges.05MultipleCandidatesRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateMultipleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	AddRoute(Snapshot, 0, 0.10, 0.0);
	AddRoute(Snapshot, 1, 0.15, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	if (!TestEqual(TEXT("Both legal edges retained"), Graph.Edges.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Nearest edge first"), Graph.Edges[0].RouteSlotIndex, 0);
	TestEqual(TEXT("Second edge retained"), Graph.Edges[1].RouteSlotIndex, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateCompetitionTest,
	"DroneGCS.RouteAssignment.CandidateEdges.06CompetingDronesRetained",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateCompetitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneB"), 0.10, 0.0);
	AddDrone(Snapshot, Drones, TEXT("DroneA"), -0.10, 0.0);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	if (!TestEqual(TEXT("Both competing edges retained"), Graph.Edges.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("Canonical DroneA first"), Graph.Edges[0].DroneID, FString(TEXT("DroneA")));
	TestEqual(TEXT("Canonical DroneB second"), Graph.Edges[1].DroneID, FString(TEXT("DroneB")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateZeroEdgesTest,
	"DroneGCS.RouteAssignment.CandidateEdges.07EligibleZeroCandidatesVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateZeroEdgesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	AddRoute(Snapshot, 0, 0.21, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestEqual(TEXT("Eligible drone retained"), Graph.EligibleDroneCount, 1);
	TestTrue(TEXT("No candidate edges"), Graph.Edges.IsEmpty());
	TestTrue(TEXT("Zero-edge range represented"), Graph.EdgeOffsets == TArray<int32>({ 0, 0 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateIneligibleTest,
	"DroneGCS.RouteAssignment.CandidateEdges.08IneligibleProducesNoEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateIneligibleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0, false);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestEqual(TEXT("No eligible drones"), Graph.EligibleDroneCount, 0);
	TestTrue(TEXT("No candidate edges"), Graph.Edges.IsEmpty());
	TestTrue(TEXT("Empty graph has terminal offset"), Graph.EdgeOffsets == TArray<int32>({ 0 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateCountMismatchTest,
	"DroneGCS.RouteAssignment.CandidateEdges.09InputCountMismatchFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateCountMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	FRouteAssignmentDroneSource SourceDrone;
	SourceDrone.DroneID = TEXT("DroneA");
	Snapshot.Drones.Add(SourceDrone);
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestFalse(TEXT("Count mismatch fails"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestTrue(TEXT("Error provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateIdentityMismatchTest,
	"DroneGCS.RouteAssignment.CandidateEdges.10InputIdentityMismatchFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateIdentityMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	Drones[0].DroneID = TEXT("DroneB");
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestFalse(TEXT("Identity mismatch fails"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestTrue(TEXT("Error provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateCanonicalOrderTest,
	"DroneGCS.RouteAssignment.CandidateEdges.11CanonicalOrdering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateCanonicalOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("Charlie"), 0.0, 0.0);
	AddDrone(Snapshot, Drones, TEXT("Alpha"), 0.05, 0.0);
	AddRoute(Snapshot, 2, 0.10, 0.0);
	AddRoute(Snapshot, 0, -0.10, 0.0);
	AddRoute(Snapshot, 1, 0.05, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestTrue(TEXT("DroneIDs canonical"),
		Graph.EligibleDroneIDs == TArray<FString>({ TEXT("Alpha"), TEXT("Charlie") }));
	if (!TestEqual(TEXT("Six edges"), Graph.Edges.Num(), 6))
	{
		return false;
	}
	const TArray<int32> ExpectedSlots{ 1, 2, 0, 1, 0, 2 };
	for (int32 EdgeIndex = 0; EdgeIndex < ExpectedSlots.Num(); ++EdgeIndex)
	{
		TestEqual(
			*FString::Printf(TEXT("Canonical slot at edge %d"), EdgeIndex),
			Graph.Edges[EdgeIndex].RouteSlotIndex,
			ExpectedSlots[EdgeIndex]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateRepeatTest,
	"DroneGCS.RouteAssignment.CandidateEdges.12RepeatEquivalent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateRepeatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneB"), 0.02, 0.0);
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.01, 0.0);
	AddRoute(Snapshot, 1, 0.10, 0.0);
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph First;
	FRouteAssignmentCandidateGraph Second;
	FString Error;
	TestTrue(TEXT("First build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), First, Error));
	TestTrue(TEXT("Second build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Second, Error));
	TestTrue(TEXT("Graphs are equivalent"), GraphsEquivalent(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateAltitudeUpTest,
	"DroneGCS.RouteAssignment.CandidateEdges.13AltitudeAndUpIgnored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateAltitudeUpTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	Snapshot.Drones[0].AltitudeAMSL = std::numeric_limits<float>::infinity();
	Snapshot.Drones[0].bAltitudeValid = false;
	AddRoute(Snapshot, 0, 0.10, 0.0, std::numeric_limits<double>::quiet_NaN());
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Build ignores altitude and Up"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestEqual(TEXT("Horizontal edge retained"), Graph.Edges.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateEmptyRouteTest,
	"DroneGCS.RouteAssignment.CandidateEdges.14EmptyRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateEmptyRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Empty Route build succeeds"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestEqual(TEXT("RouteSlotCount is zero"), Graph.RouteSlotCount, 0);
	TestEqual(TEXT("Eligible drone retained"), Graph.EligibleDroneCount, 1);
	TestTrue(TEXT("No edges"), Graph.Edges.IsEmpty());
	TestTrue(TEXT("Zero-edge range represented"), Graph.EdgeOffsets == TArray<int32>({ 0, 0 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateRouteValidationTest,
	"DroneGCS.RouteAssignment.CandidateEdges.15MalformedRouteFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateRouteValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneA"), 0.0, 0.0);
	AddRoute(Snapshot, 1, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestFalse(TEXT("Out-of-range RouteSlot fails"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	TestTrue(TEXT("Error provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateConfigValidationTest,
	"DroneGCS.RouteAssignment.CandidateEdges.16ConfigValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateConfigValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	const FRouteAssignmentMatcherConfig DefaultConfig;
	TestFalse(TEXT("Invalid default config rejected"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, DefaultConfig, Graph, Error));
	const FRouteAssignmentMatcherConfig Config = ProductionConfig();
	TestTrue(TEXT("Production contract identified"), Config.IsProductionContract());
	TestEqual(TEXT("Production threshold is exactly 0.20m"),
		Config.MaxMatchDistanceMeters,
		0.20);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentCandidateFleetIdentityTest,
	"DroneGCS.RouteAssignment.CandidateEdges.17FleetCompatibleIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentCandidateFleetIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Drones;
	AddDrone(Snapshot, Drones, TEXT("DroneAlpha"), 0.0, 0.0);
	Drones[0].DroneID = TEXT("dronealpha");
	AddRoute(Snapshot, 0, 0.0, 0.0);
	FRouteAssignmentCandidateGraph Graph;
	FString Error;
	TestTrue(TEXT("Case-insensitive identity accepted"), RouteAssignmentMatcher::BuildCandidateEdges(
		Snapshot, Drones, ProductionConfig(), Graph, Error));
	if (!TestEqual(TEXT("One edge"), Graph.Edges.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("Authoritative source casing preserved"),
		Graph.Edges[0].DroneID,
		FString(TEXT("DroneAlpha")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumOneByOneTest,
	"DroneGCS.RouteAssignment.MaximumMatching.01OneDroneOneRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumOneByOneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum is one"), Result.MaximumCardinality, 1);
	TestEqual(TEXT("Witness count equals maximum"), Result.WitnessPairs.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumOneByTwoTest,
	"DroneGCS.RouteAssignment.MaximumMatching.02OneDroneTwoRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumOneByTwoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 2, { { 1, 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum is one"), Result.MaximumCardinality, 1);
	if (Result.WitnessPairs.Num() == 1)
	{
		TestEqual(TEXT("Canonical route traversal chooses zero"),
			Result.WitnessPairs[0].RouteSlotIndex, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumTwoByOneTest,
	"DroneGCS.RouteAssignment.MaximumMatching.03TwoDronesOneRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumTwoByOneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 1, { { 0 }, { 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum is one"), Result.MaximumCardinality, 1);
	if (Result.WitnessPairs.Num() == 1)
	{
		TestEqual(TEXT("Canonical Drone wins witness"),
			Result.WitnessPairs[0].DroneID, FString(TEXT("DroneA")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumGreedyTrapTest,
	"DroneGCS.RouteAssignment.MaximumMatching.04GreedyTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumGreedyTrapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Augmenting path reaches maximum two"), Result.MaximumCardinality, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumComplexAugmentTest,
	"DroneGCS.RouteAssignment.MaximumMatching.05ComplexAugmentingPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumComplexAugmentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 1, 2 }, { 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Multi-hop augmenting path reaches three"),
		Result.MaximumCardinality, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumZeroCandidateTest,
	"DroneGCS.RouteAssignment.MaximumMatching.06ZeroCandidateDrone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumZeroCandidateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 1, { {}, { 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Zero-candidate Drone does not fail solve"),
		Result.MaximumCardinality, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumEmptyDroneTest,
	"DroneGCS.RouteAssignment.MaximumMatching.07EmptyDroneSide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumEmptyDroneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph({}, 3, {});
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Empty Drone side succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestEqual(TEXT("Maximum is zero"), Result.MaximumCardinality, 0);
	TestTrue(TEXT("Witness is empty"), Result.WitnessPairs.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumEmptyRouteTest,
	"DroneGCS.RouteAssignment.MaximumMatching.08EmptyRouteSide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumEmptyRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 0, { {} });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Empty Route side succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestEqual(TEXT("Maximum is zero"), Result.MaximumCardinality, 0);
	TestTrue(TEXT("Witness is empty"), Result.WitnessPairs.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumDisconnectedTest,
	"DroneGCS.RouteAssignment.MaximumMatching.09DisconnectedGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumDisconnectedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 0, 1 }, { 2 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Component maxima sum to three"), Result.MaximumCardinality, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumAllToAllTest,
	"DroneGCS.RouteAssignment.MaximumMatching.10AllToAll",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumAllToAllTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		2,
		{ { 0, 1 }, { 0, 1 }, { 0, 1 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum equals min(E,R)"), Result.MaximumCardinality, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMoreDronesTest,
	"DroneGCS.RouteAssignment.MaximumMatching.11EligibleGreaterThanRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMoreDronesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		2,
		{ { 0 }, { 1 }, { 0, 1 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("All Routes can be covered"), Result.MaximumCardinality, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMoreRoutesTest,
	"DroneGCS.RouteAssignment.MaximumMatching.12EligibleLessThanRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMoreRoutesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 3, { { 0, 2 }, { 1, 2 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("All eligible Drones can be covered"), Result.MaximumCardinality, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumDeficientGraphTest,
	"DroneGCS.RouteAssignment.MaximumMatching.13DeficientGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumDeficientGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0 }, { 0 }, { 1 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum is below min(E,R)"), Result.MaximumCardinality, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumDroneUniqueTest,
	"DroneGCS.RouteAssignment.MaximumMatching.14WitnessDroneUnique",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumDroneUniqueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 1, 2 }, { 0, 2 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TSet<FString> WitnessDrones;
	for (const FRouteAssignmentMatchingPair& Pair : Result.WitnessPairs)
	{
		WitnessDrones.Add(Pair.DroneID);
	}
	TestEqual(TEXT("Every witness Drone is unique"),
		WitnessDrones.Num(), Result.WitnessPairs.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumRouteUniqueTest,
	"DroneGCS.RouteAssignment.MaximumMatching.15WitnessRouteUnique",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumRouteUniqueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 1, 2 }, { 0, 2 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TSet<int32> WitnessRoutes;
	for (const FRouteAssignmentMatchingPair& Pair : Result.WitnessPairs)
	{
		WitnessRoutes.Add(Pair.RouteSlotIndex);
	}
	TestEqual(TEXT("Every witness Route is unique"),
		WitnessRoutes.Num(), Result.WitnessPairs.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumWitnessEdgesTest,
	"DroneGCS.RouteAssignment.MaximumMatching.16WitnessUsesInputEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumWitnessEdgesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 3, { { 0, 2 }, { 1 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	for (const FRouteAssignmentMatchingPair& Pair : Result.WitnessPairs)
	{
		TestTrue(*FString::Printf(TEXT("Witness edge %s -> %d exists"),
			*Pair.DroneID, Pair.RouteSlotIndex), IsInputEdge(Graph, Pair));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumRepeatTest,
	"DroneGCS.RouteAssignment.MaximumMatching.17RepeatDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumRepeatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 2, 0, 1 }, { 2, 0, 1 }, { 2, 0, 1 } });
	FRouteAssignmentMaximumMatchingResult First;
	FRouteAssignmentMaximumMatchingResult Second;
	FString Error;
	TestTrue(TEXT("First solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, First, Error));
	TestTrue(TEXT("Second solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Second, Error));
	TestTrue(TEXT("Repeated witness is identical"),
		MatchingResultsEquivalent(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumSourceOrderTest,
	"DroneGCS.RouteAssignment.MaximumMatching.18SourceOrderIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumSourceOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot FirstSnapshot;
	TArray<FRouteAssignmentDroneLocalPosition> FirstDrones;
	AddDrone(FirstSnapshot, FirstDrones, TEXT("DroneA"), 0.0, 0.0);
	AddDrone(FirstSnapshot, FirstDrones, TEXT("DroneB"), 0.1, 0.0);
	AddRoute(FirstSnapshot, 0, 0.0, 0.0);
	AddRoute(FirstSnapshot, 1, 0.1, 0.0);

	FRouteAssignmentSourceSnapshot SecondSnapshot;
	TArray<FRouteAssignmentDroneLocalPosition> SecondDrones;
	AddDrone(SecondSnapshot, SecondDrones, TEXT("DroneB"), 0.1, 0.0);
	AddDrone(SecondSnapshot, SecondDrones, TEXT("DroneA"), 0.0, 0.0);
	AddRoute(SecondSnapshot, 0, 0.0, 0.0);
	AddRoute(SecondSnapshot, 1, 0.1, 0.0);

	FRouteAssignmentCandidateGraph FirstGraph;
	FRouteAssignmentCandidateGraph SecondGraph;
	FString Error;
	TestTrue(TEXT("First candidate graph builds"), RouteAssignmentMatcher::BuildCandidateEdges(
		FirstSnapshot, FirstDrones, ProductionConfig(), FirstGraph, Error));
	TestTrue(TEXT("Second candidate graph builds"), RouteAssignmentMatcher::BuildCandidateEdges(
		SecondSnapshot, SecondDrones, ProductionConfig(), SecondGraph, Error));
	FRouteAssignmentMaximumMatchingResult FirstResult;
	FRouteAssignmentMaximumMatchingResult SecondResult;
	TestTrue(TEXT("First solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(FirstGraph, FirstResult, Error));
	TestTrue(TEXT("Second solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(SecondGraph, SecondResult, Error));
	TestTrue(TEXT("Canonical equivalent graphs produce identical witness"),
		MatchingResultsEquivalent(FirstResult, SecondResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMultipleSolutionsTest,
	"DroneGCS.RouteAssignment.MaximumMatching.19MultipleMaximumDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMultipleSolutionsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 1, 0 }, { 1, 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), RouteAssignmentMatcher::SolveMaximumCardinality(
		Graph, Result, Error));
	TestEqual(TEXT("Maximum is two"), Result.MaximumCardinality, 2);
	if (Result.WitnessPairs.Num() == 2)
	{
		TestEqual(TEXT("Canonical DroneA route"), Result.WitnessPairs[0].RouteSlotIndex, 0);
		TestEqual(TEXT("Canonical DroneB route"), Result.WitnessPairs[1].RouteSlotIndex, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMalformedOffsetsTest,
	"DroneGCS.RouteAssignment.MaximumMatching.20MalformedOffsetsFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMalformedOffsetsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0 }, { 1 } });
	Graph.EdgeOffsets[1] = 3;
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Malformed offsets fail"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumDuplicateEdgeTest,
	"DroneGCS.RouteAssignment.MaximumMatching.21DuplicateEdgeFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumDuplicateEdgeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0, 0 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Duplicate edge fails"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMalformedRouteTest,
	"DroneGCS.RouteAssignment.MaximumMatching.22MalformedRouteFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMalformedRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 1 } });
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Out-of-range RouteSlotIndex fails"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumDistanceIndependentTest,
	"DroneGCS.RouteAssignment.MaximumMatching.23DistanceIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumDistanceIndependentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<FString> DroneIDs{ TEXT("DroneA"), TEXT("DroneB") };
	const TArray<TArray<int32>> Adjacency{ { 1, 0 }, { 0, 1 } };
	const TArray<TArray<double>> FirstDistances{ { 0.19, 0.01 }, { 0.02, 0.18 } };
	const TArray<TArray<double>> SecondDistances{ { 0.01, 0.19 }, { 0.18, 0.02 } };
	const FRouteAssignmentCandidateGraph FirstGraph = MakeCandidateGraph(
		DroneIDs, 2, Adjacency, &FirstDistances);
	const FRouteAssignmentCandidateGraph SecondGraph = MakeCandidateGraph(
		DroneIDs, 2, Adjacency, &SecondDistances);
	FRouteAssignmentMaximumMatchingResult FirstResult;
	FRouteAssignmentMaximumMatchingResult SecondResult;
	FString Error;
	TestTrue(TEXT("First solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(FirstGraph, FirstResult, Error));
	TestTrue(TEXT("Second solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(SecondGraph, SecondResult, Error));
	TestTrue(TEXT("Distance changes do not change cardinality or witness"),
		MatchingResultsEquivalent(FirstResult, SecondResult));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMalformedDistanceTest,
	"DroneGCS.RouteAssignment.MaximumMatching.24MalformedDistanceFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMalformedDistanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	Graph.Edges[0].HorizontalDistanceMeters = std::numeric_limits<double>::quiet_NaN();
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Non-finite distance fails validation"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMalformedIdentityTest,
	"DroneGCS.RouteAssignment.MaximumMatching.25MalformedEdgeIdentityFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMalformedIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	Graph.Edges[0].DroneID = TEXT("DroneB");
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Adjacency identity mismatch fails"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMaximumMalformedCountTest,
	"DroneGCS.RouteAssignment.MaximumMatching.26MalformedCountFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMaximumMalformedCountTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	Graph.CandidateEdgeCount = 2;
	FRouteAssignmentMaximumMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Header count mismatch fails"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Result, Error));
	TestTrue(TEXT("Error is provided"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumOneByOneTest,
	"DroneGCS.RouteAssignment.MinimumCost.01OneDroneOneRoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumOneByOneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.125 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Cardinality is one"), Result.MatchedCardinality, 1);
	TestEqual(TEXT("Cost is original edge cost"),
		Result.TotalHorizontalDistanceMeters, 0.125);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumOneByTwoTest,
	"DroneGCS.RouteAssignment.MinimumCost.02OneDroneChoosesLowerCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumOneByTwoTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.15, 0.05 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 2, { { 0, 1 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Lower-cost Route selected"), FindMatchedRoute(Result, TEXT("DroneA")), 1);
	TestEqual(TEXT("Minimum total"), Result.TotalHorizontalDistanceMeters, 0.05);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumTwoByOneTest,
	"DroneGCS.RouteAssignment.MinimumCost.03TwoDronesChoosesLowerCost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumTwoByOneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.125 }, { 0.0625 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 1, { { 0 }, { 0 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Cardinality remains one"), Result.MatchedCardinality, 1);
	TestEqual(TEXT("Lower-cost Drone selected"), FindMatchedRoute(Result, TEXT("DroneB")), 0);
	TestEqual(TEXT("Higher-cost Drone not selected"),
		FindMatchedRoute(Result, TEXT("DroneA")), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumGreedyCostTrapTest,
	"DroneGCS.RouteAssignment.MinimumCost.04GreedyCostTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumGreedyCostTrapTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.01, 0.02 }, { 0.015, 0.19 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0, 1 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Maximum cardinality preserved"), Result.MatchedCardinality, 2);
	TestEqual(TEXT("DroneA uses Route1"), FindMatchedRoute(Result, TEXT("DroneA")), 1);
	TestEqual(TEXT("DroneB uses Route0"), FindMatchedRoute(Result, TEXT("DroneB")), 0);
	TestEqual(TEXT("Global minimum total"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum({ 0.02, 0.015 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumPreservesCardinalityTest,
	"DroneGCS.RouteAssignment.MinimumCost.05PreservesMaximumCardinality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumPreservesCardinalityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.0, 0.20 }, { 0.20 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Returns two despite cheaper one-pair solution"),
		Result.MatchedCardinality, 2);
	TestEqual(TEXT("Required two-pair total"), Result.TotalHorizontalDistanceMeters, 0.40);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumResidualReassignmentTest,
	"DroneGCS.RouteAssignment.MinimumCost.06ResidualReassignment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumResidualReassignmentTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.01, 0.02 }, { 0.015 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Reverse edge moves DroneA to Route1"),
		FindMatchedRoute(Result, TEXT("DroneA")), 1);
	TestEqual(TEXT("DroneB obtains Route0"), FindMatchedRoute(Result, TEXT("DroneB")), 0);
	TestEqual(TEXT("Minimum total after reassignment"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum({ 0.02, 0.015 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumDisconnectedTest,
	"DroneGCS.RouteAssignment.MinimumCost.07DisconnectedComponents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumDisconnectedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{
		{ 0.10, 0.01 }, { 0.02, 0.10 }, { 0.04 }
	};
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 0, 1 }, { 2 } },
		&Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("All components matched"), Result.MatchedCardinality, 3);
	TestEqual(TEXT("Component optima sum"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum({ 0.01, 0.02, 0.04 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumEqualCostTest,
	"DroneGCS.RouteAssignment.MinimumCost.08EqualCostDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumEqualCostTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.10, 0.10 }, { 0.10, 0.10 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0, 1 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult First;
	FRouteAssignmentMinimumCostMatchingResult Second;
	FString Error;
	TestTrue(TEXT("First solve succeeds"), SolveMinimumForTest(Graph, First, Error));
	TestTrue(TEXT("Second solve succeeds"), SolveMinimumForTest(Graph, Second, Error));
	TestTrue(TEXT("Equal-cost witness repeats exactly"),
		MinimumResultsEquivalent(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumInputOrderTest,
	"DroneGCS.RouteAssignment.MinimumCost.09CandidateInputOrderIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumInputOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> FirstCosts{ { 0.15, 0.05 }, { 0.04, 0.16 } };
	const TArray<TArray<double>> SecondCosts{ { 0.05, 0.15 }, { 0.16, 0.04 } };
	const FRouteAssignmentCandidateGraph FirstGraph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 1, 0 }, { 0, 1 } }, &FirstCosts);
	const FRouteAssignmentCandidateGraph SecondGraph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 1, 0 } }, &SecondCosts);
	FRouteAssignmentMinimumCostMatchingResult First;
	FRouteAssignmentMinimumCostMatchingResult Second;
	FString Error;
	TestTrue(TEXT("First solve succeeds"), SolveMinimumForTest(FirstGraph, First, Error));
	TestTrue(TEXT("Second solve succeeds"), SolveMinimumForTest(SecondGraph, Second, Error));
	TestTrue(TEXT("Equivalent graph ordering gives identical result"),
		MinimumResultsEquivalent(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumPairUniqueTest,
	"DroneGCS.RouteAssignment.MinimumCost.10PairUniqueness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumPairUniqueTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.10, 0.11 }, { 0.12, 0.09 }, { 0.08, 0.13 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0, 1 }, { 1, 2 }, { 0, 2 } },
		&Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TSet<FString> Drones;
	TSet<int32> Routes;
	for (const FRouteAssignmentMinimumCostPair& Pair : Result.Pairs)
	{
		Drones.Add(Pair.DroneID);
		Routes.Add(Pair.RouteSlotIndex);
	}
	TestEqual(TEXT("Drone unique"), Drones.Num(), Result.Pairs.Num());
	TestEqual(TEXT("Route unique"), Routes.Num(), Result.Pairs.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumCandidateEdgeTest,
	"DroneGCS.RouteAssignment.MinimumCost.11WitnessUsesCandidateEdges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumCandidateEdgeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.03, 0.04 }, { 0.05 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 3, { { 0, 2 }, { 1 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	for (const FRouteAssignmentMinimumCostPair& Pair : Result.Pairs)
	{
		TestTrue(*FString::Printf(TEXT("Pair %s -> %d is an exact input edge"),
			*Pair.DroneID, Pair.RouteSlotIndex), IsInputEdge(Graph, Pair));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumCanonicalTotalTest,
	"DroneGCS.RouteAssignment.MinimumCost.12CanonicalTotalRecomputation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumCanonicalTotalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.10 }, { 0.03 }, { 0.07 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 0 }, { 1 }, { 2 } },
		&Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TArray<double> PairCosts;
	for (const FRouteAssignmentMinimumCostPair& Pair : Result.Pairs)
	{
		PairCosts.Add(Pair.HorizontalDistanceMeters);
	}
	TestEqual(TEXT("Total equals canonical compensated recomputation"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum(PairCosts));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumBoundaryTest,
	"DroneGCS.RouteAssignment.MinimumCost.13ExactBoundaryEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.20 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Boundary edge solves"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Boundary cost retained exactly"),
		Result.TotalHorizontalDistanceMeters, 0.20);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumZeroMaximumTest,
	"DroneGCS.RouteAssignment.MinimumCost.14ZeroMaximumCardinality",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumZeroMaximumTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { {} });
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Zero maximum succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Matched cardinality zero"), Result.MatchedCardinality, 0);
	TestTrue(TEXT("Pairs empty"), Result.Pairs.IsEmpty());
	TestEqual(TEXT("Total zero"), Result.TotalHorizontalDistanceMeters, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumMoreDronesTest,
	"DroneGCS.RouteAssignment.MinimumCost.15EligibleGreaterThanRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumMoreDronesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.10 }, { 0.04 }, { 0.03, 0.02 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		2,
		{ { 0 }, { 1 }, { 0, 1 } },
		&Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("All Routes covered"), Result.MatchedCardinality, 2);
	TestEqual(TEXT("Minimum subset total"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum({ 0.04, 0.03 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumMoreRoutesTest,
	"DroneGCS.RouteAssignment.MinimumCost.16EligibleLessThanRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumMoreRoutesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{ { 0.08, 0.02 }, { 0.03, 0.09 } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 3, { { 0, 2 }, { 1, 2 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("All eligible Drones covered"), Result.MatchedCardinality, 2);
	TestEqual(TEXT("Minimum total"),
		Result.TotalHorizontalDistanceMeters,
		CompensatedSum({ 0.02, 0.03 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumMalformedMaximumWitnessTest,
	"DroneGCS.RouteAssignment.MinimumCost.17MalformedMaximumWitnessFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumMalformedMaximumWitnessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0 }, { 1 } });
	FRouteAssignmentMaximumMatchingResult MaximumResult;
	FString Error;
	TestTrue(TEXT("Maximum solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, MaximumResult, Error));
	MaximumResult.WitnessPairs[1].RouteSlotIndex = 0;
	FRouteAssignmentMinimumCostMatchingResult Result;
	TestFalse(TEXT("Malformed maximum witness fails"),
		RouteAssignmentMatcher::SolveMinimumTotalDistance(
			Graph, MaximumResult, Result, Error));
	TestTrue(TEXT("Cardinality category returned"),
		Error.StartsWith(TEXT("CardinalityContractMismatch:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumCardinalityMismatchTest,
	"DroneGCS.RouteAssignment.MinimumCost.18CardinalityMismatchFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumCardinalityMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0 }, { 1 } });
	FRouteAssignmentMaximumMatchingResult Supplied;
	Supplied.EligibleDroneCount = 2;
	Supplied.RouteSlotCount = 2;
	Supplied.MaximumCardinality = 1;
	FRouteAssignmentMatchingPair Pair;
	Pair.DroneID = TEXT("DroneA");
	Pair.RouteSlotIndex = 0;
	Supplied.WitnessPairs.Add(Pair);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Smaller supplied cardinality fails"),
		RouteAssignmentMatcher::SolveMinimumTotalDistance(Graph, Supplied, Result, Error));
	TestTrue(TEXT("Cardinality category returned"),
		Error.StartsWith(TEXT("CardinalityContractMismatch:")));
	TestTrue(TEXT("Failure result cleared"), Result.Pairs.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumMalformedGraphTest,
	"DroneGCS.RouteAssignment.MinimumCost.19MalformedCandidateGraphFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumMalformedGraphTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	FRouteAssignmentMaximumMatchingResult MaximumResult;
	FString Error;
	TestTrue(TEXT("Maximum solve succeeds before corruption"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, MaximumResult, Error));
	Graph.EdgeOffsets[1] = 2;
	FRouteAssignmentMinimumCostMatchingResult Result;
	TestFalse(TEXT("Malformed graph fails"),
		RouteAssignmentMatcher::SolveMinimumTotalDistance(
			Graph, MaximumResult, Result, Error));
	TestTrue(TEXT("Malformed graph category returned"),
		Error.StartsWith(TEXT("MalformedCandidateGraph:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumNonFiniteTest,
	"DroneGCS.RouteAssignment.MinimumCost.20NonFiniteDistanceFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumNonFiniteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA") }, 1, { { 0 } });
	FRouteAssignmentMaximumMatchingResult MaximumResult;
	MaximumResult.EligibleDroneCount = 1;
	MaximumResult.RouteSlotCount = 1;
	MaximumResult.MaximumCardinality = 1;
	FRouteAssignmentMatchingPair Pair;
	Pair.DroneID = TEXT("DroneA");
	Pair.RouteSlotIndex = 0;
	MaximumResult.WitnessPairs.Add(Pair);
	Graph.Edges[0].HorizontalDistanceMeters = std::numeric_limits<double>::infinity();
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestFalse(TEXT("Non-finite distance fails"),
		RouteAssignmentMatcher::SolveMinimumTotalDistance(
			Graph, MaximumResult, Result, Error));
	TestTrue(TEXT("Malformed graph category returned"),
		Error.StartsWith(TEXT("MalformedCandidateGraph:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumRepeatTest,
	"DroneGCS.RouteAssignment.MinimumCost.21RepeatDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumRepeatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{
		{ 0.08, 0.03, 0.07 }, { 0.04, 0.09, 0.02 }, { 0.06, 0.05, 0.01 }
	};
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC") },
		3,
		{ { 2, 0, 1 }, { 1, 2, 0 }, { 0, 1, 2 } },
		&Costs);
	FRouteAssignmentMinimumCostMatchingResult First;
	FRouteAssignmentMinimumCostMatchingResult Second;
	FString Error;
	TestTrue(TEXT("First solve succeeds"), SolveMinimumForTest(Graph, First, Error));
	TestTrue(TEXT("Second solve succeeds"), SolveMinimumForTest(Graph, Second, Error));
	TestTrue(TEXT("Repeated result is exact"), MinimumResultsEquivalent(First, Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumDistanceChangesWitnessTest,
	"DroneGCS.RouteAssignment.MinimumCost.22DistanceChangesOptimalWitness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumDistanceChangesWitnessTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<int32>> Adjacency{ { 0, 1 }, { 0, 1 } };
	const TArray<TArray<double>> DiagonalCosts{ { 0.01, 0.10 }, { 0.10, 0.01 } };
	const TArray<TArray<double>> CrossCosts{ { 0.10, 0.01 }, { 0.01, 0.10 } };
	const FRouteAssignmentCandidateGraph DiagonalGraph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, Adjacency, &DiagonalCosts);
	const FRouteAssignmentCandidateGraph CrossGraph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, Adjacency, &CrossCosts);
	FRouteAssignmentMinimumCostMatchingResult DiagonalResult;
	FRouteAssignmentMinimumCostMatchingResult CrossResult;
	FString Error;
	TestTrue(TEXT("Diagonal solve succeeds"),
		SolveMinimumForTest(DiagonalGraph, DiagonalResult, Error));
	TestTrue(TEXT("Cross solve succeeds"),
		SolveMinimumForTest(CrossGraph, CrossResult, Error));
	TestEqual(TEXT("Diagonal chooses Route0 for DroneA"),
		FindMatchedRoute(DiagonalResult, TEXT("DroneA")), 0);
	TestEqual(TEXT("Cross chooses Route1 for DroneA"),
		FindMatchedRoute(CrossResult, TEXT("DroneA")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumOracleFixedTest,
	"DroneGCS.RouteAssignment.MinimumCost.23BruteForceOracleFixed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumOracleFixedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const TArray<TArray<double>> Costs{
		{ 0.12, 0.03 }, { 0.04, 0.11, 0.07 }, { 0.02, 0.09 }, { 0.06 }
	};
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB"), TEXT("DroneC"), TEXT("DroneD") },
		4,
		{ { 0, 1 }, { 0, 2, 3 }, { 1, 2 }, { 3 } },
		&Costs);
	const FBruteForceOptimum Oracle = SolveBruteForceOracle(Graph);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("Cardinality matches brute-force oracle"),
		Result.MatchedCardinality, Oracle.MaximumCardinality);
	TestEqual(TEXT("Total matches brute-force oracle"),
		Result.TotalHorizontalDistanceMeters, Oracle.MinimumTotalDistanceMeters);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumOracleSyntheticTest,
	"DroneGCS.RouteAssignment.MinimumCost.24BruteForceOracleSynthetic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumOracleSyntheticTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	uint32 State = 0x4D595DF4u;
	auto NextRandom = [&State]()
	{
		State = State * 1664525u + 1013904223u;
		return State;
	};

	for (int32 CaseIndex = 0; CaseIndex < 24; ++CaseIndex)
	{
		const int32 DroneCount = 1 + static_cast<int32>(NextRandom() % 5u);
		const int32 RouteCount = 1 + static_cast<int32>(NextRandom() % 5u);
		TArray<FString> DroneIDs;
		TArray<TArray<int32>> Adjacency;
		TArray<TArray<double>> Costs;
		Adjacency.SetNum(DroneCount);
		Costs.SetNum(DroneCount);
		for (int32 DroneIndex = 0; DroneIndex < DroneCount; ++DroneIndex)
		{
			DroneIDs.Add(FString::Printf(TEXT("Drone%02d"), DroneIndex));
			for (int32 RouteIndex = 0; RouteIndex < RouteCount; ++RouteIndex)
			{
				if (NextRandom() % 100u >= 48u)
				{
					continue;
				}
				Adjacency[DroneIndex].Add(RouteIndex);
				const uint32 CostUnits = 1u + NextRandom() % 200u;
				Costs[DroneIndex].Add(static_cast<double>(CostUnits) / 1024.0);
			}
		}

		const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
			DroneIDs, RouteCount, Adjacency, &Costs);
		const FBruteForceOptimum Oracle = SolveBruteForceOracle(Graph);
		FRouteAssignmentMinimumCostMatchingResult Result;
		FString Error;
		TestTrue(*FString::Printf(TEXT("Synthetic case %d solves"), CaseIndex),
			SolveMinimumForTest(Graph, Result, Error));
		TestEqual(*FString::Printf(TEXT("Synthetic case %d cardinality"), CaseIndex),
			Result.MatchedCardinality, Oracle.MaximumCardinality);
		TestEqual(*FString::Printf(TEXT("Synthetic case %d total"), CaseIndex),
			Result.TotalHorizontalDistanceMeters, Oracle.MinimumTotalDistanceMeters);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumCloseDoubleTest,
	"DroneGCS.RouteAssignment.MinimumCost.25CloseDoubleCostsRemainDistinct",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumCloseDoubleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double Low = 0.10;
	const double High = std::nextafter(Low, std::numeric_limits<double>::infinity());
	const TArray<TArray<double>> Costs{ { Low, High }, { Low, Low } };
	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		{ TEXT("DroneA"), TEXT("DroneB") }, 2, { { 0, 1 }, { 0, 1 } }, &Costs);
	FRouteAssignmentMinimumCostMatchingResult Result;
	FString Error;
	TestTrue(TEXT("Solve succeeds"), SolveMinimumForTest(Graph, Result, Error));
	TestEqual(TEXT("True lower double edge selected for DroneA"),
		FindMatchedRoute(Result, TEXT("DroneA")), 0);
	TestEqual(TEXT("Other Drone uses Route1"), FindMatchedRoute(Result, TEXT("DroneB")), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumRoundoffClampTest,
	"DroneGCS.RouteAssignment.MinimumCost.26RoundoffScaleReducedCostClamps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumRoundoffClampTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const double Epsilon = std::numeric_limits<double>::epsilon();
	double ReducedCost = 1.0;
	FString Error;
	TestTrue(TEXT("Roundoff-scale negative reduced cost accepted"),
		RouteAssignmentMatcher::Testing::NormalizeReducedCost(
			0.0, 0.0, 8.0 * Epsilon, ReducedCost, Error));
	TestEqual(TEXT("Roundoff-scale value clamps to zero"), ReducedCost, 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumNumericalFailureTest,
	"DroneGCS.RouteAssignment.MinimumCost.27MaterialNegativeReducedCostFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumNumericalFailureTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	double ReducedCost = 0.0;
	FString Error;
	TestFalse(TEXT("Material negative reduced cost rejected"),
		RouteAssignmentMatcher::Testing::NormalizeReducedCost(
			0.0, 0.0, 1.0e-6, ReducedCost, Error));
	TestTrue(TEXT("Numerical failure category returned"),
		Error.StartsWith(TEXT("NumericalInvariantFailure:")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentMinimumSparseBaselineTest,
	"DroneGCS.RouteAssignment.MinimumCost.28Sparse1000Baseline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentMinimumSparseBaselineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	constexpr int32 NodeCount = 1000;
	TArray<FString> DroneIDs;
	TArray<TArray<int32>> Adjacency;
	TArray<TArray<double>> Costs;
	DroneIDs.Reserve(NodeCount);
	Adjacency.SetNum(NodeCount);
	Costs.SetNum(NodeCount);
	for (int32 Index = 0; Index < NodeCount; ++Index)
	{
		DroneIDs.Add(FString::Printf(TEXT("Drone%04d"), Index));
		Adjacency[Index].Add(Index);
		Adjacency[Index].Add((Index + 1) % NodeCount);
		Costs[Index].Add(0.01);
		Costs[Index].Add(0.02);
	}

	const FRouteAssignmentCandidateGraph Graph = MakeCandidateGraph(
		DroneIDs, NodeCount, Adjacency, &Costs);
	FRouteAssignmentMaximumMatchingResult MaximumResult;
	FString Error;
	TestTrue(TEXT("Maximum solve succeeds"),
		RouteAssignmentMatcher::SolveMaximumCardinality(Graph, MaximumResult, Error));
	TestEqual(TEXT("Maximum is 1000"), MaximumResult.MaximumCardinality, NodeCount);

	FRouteAssignmentMinimumCostMatchingResult Result;
	const double StartSeconds = FPlatformTime::Seconds();
	const bool bSolved = RouteAssignmentMatcher::SolveMinimumTotalDistance(
		Graph, MaximumResult, Result, Error);
	const double ElapsedMilliseconds = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("Sparse1000 SSAP elapsed %.3f ms for %d edges."),
		ElapsedMilliseconds,
		Graph.CandidateEdgeCount));
	TestTrue(TEXT("Sparse 1000 solve succeeds"), bSolved);
	TestEqual(TEXT("Matched cardinality is 1000"), Result.MatchedCardinality, NodeCount);
	TestEqual(TEXT("Minimum total is canonical own-route sum"),
		Result.TotalHorizontalDistanceMeters,
		10.0);
	return true;
}

#endif
