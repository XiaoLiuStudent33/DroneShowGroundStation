#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "RouteAssignmentMaterializer.h"

namespace
{
	FRouteAssignmentSourceSnapshot MakeSnapshot(const int32 RouteCount)
	{
		FRouteAssignmentSourceSnapshot Snapshot;
		Snapshot.SourceIdentity.FleetSessionID = TEXT("MaterializerTestSession");
		Snapshot.SourceIdentity.FleetRevisionAtCapture = 7;
		Snapshot.SourceIdentity.RouteDatasetGeneration = 11;
		Snapshot.SourceIdentity.bShowOriginSet = true;
		Snapshot.SourceIdentity.Latitude = 30.0;
		Snapshot.SourceIdentity.Longitude = 120.0;
		Snapshot.SourceIdentity.Altitude = 50.0;
		Snapshot.SourceIdentity.CapturedAtUtc = FDateTime(2026, 8, 17, 0, 0, 0);

		for (int32 RouteIndex = 0; RouteIndex < RouteCount; ++RouteIndex)
		{
			FRouteAssignmentRouteSlotSource Route;
			Route.RouteSlotIndex = RouteIndex;
			Route.EastMeters = static_cast<double>(RouteIndex);
			Route.NorthMeters = static_cast<double>(RouteIndex) * 0.5;
			Route.UpMeters = 1.0;
			Snapshot.RouteSlots.Add(Route);
		}
		return Snapshot;
	}

	void AddDrone(
		FRouteAssignmentSourceSnapshot& Snapshot,
		TArray<FRouteAssignmentDroneLocalPosition>& LocalPositions,
		const TCHAR* DroneID,
		const bool bEligible = true)
	{
		FRouteAssignmentDroneSource Source;
		Source.DroneID = DroneID;
		Source.Latitude = 30.0 + Snapshot.Drones.Num() * 0.000001;
		Source.Longitude = 120.0 + Snapshot.Drones.Num() * 0.000001;
		Source.AltitudeAMSL = 50.0f;
		Source.bGPSValid = true;
		Source.GPSFixType = 3;
		Source.bAltitudeValid = true;
		Source.bOnline = true;
		Snapshot.Drones.Add(Source);

		FRouteAssignmentDroneLocalPosition Local;
		Local.DroneID = DroneID;
		Local.EastMeters = static_cast<double>(LocalPositions.Num()) * 0.1;
		Local.NorthMeters = static_cast<double>(LocalPositions.Num()) * 0.1;
		Local.bAssignmentGeoEligible = bEligible;
		LocalPositions.Add(Local);
	}

	FRouteAssignmentCandidateGraph MakeGraph(
		const TArray<FString>& DroneIDs,
		const int32 RouteCount,
		const TArray<TArray<int32>>& Adjacency,
		const TArray<TArray<double>>& Costs = {})
	{
		FRouteAssignmentCandidateGraph Graph;
		Graph.EligibleDroneCount = DroneIDs.Num();
		Graph.RouteSlotCount = RouteCount;
		Graph.EligibleDroneIDs = DroneIDs;
		for (int32 DroneIndex = 0; DroneIndex < DroneIDs.Num(); ++DroneIndex)
		{
			Graph.EdgeOffsets.Add(Graph.Edges.Num());
			for (int32 EdgeIndex = 0; EdgeIndex < Adjacency[DroneIndex].Num(); ++EdgeIndex)
			{
				FRouteAssignmentCandidateEdge Edge;
				Edge.DroneID = DroneIDs[DroneIndex];
				Edge.RouteSlotIndex = Adjacency[DroneIndex][EdgeIndex];
				Edge.HorizontalDistanceMeters = Costs.IsEmpty()
					? 0.10
					: Costs[DroneIndex][EdgeIndex];
				Graph.Edges.Add(MoveTemp(Edge));
			}
		}
		Graph.EdgeOffsets.Add(Graph.Edges.Num());
		Graph.CandidateEdgeCount = Graph.Edges.Num();
		return Graph;
	}

	bool SolveGraph(
		const FRouteAssignmentCandidateGraph& Graph,
		FRouteAssignmentMinimumCostMatchingResult& OutOptimal,
		FString& OutError)
	{
		FRouteAssignmentMaximumMatchingResult Maximum;
		return RouteAssignmentMatcher::SolveMaximumCardinality(Graph, Maximum, OutError)
			&& RouteAssignmentMatcher::SolveMinimumTotalDistance(
				Graph, Maximum, OutOptimal, OutError);
	}

	bool MaterializeSolved(
		const FRouteAssignmentSourceSnapshot& Snapshot,
		const TArray<FRouteAssignmentDroneLocalPosition>& LocalPositions,
		const FRouteAssignmentCandidateGraph& Graph,
		FRouteAssignmentCandidate& OutCandidate,
		FString& OutError)
	{
		FRouteAssignmentMinimumCostMatchingResult Optimal;
		return SolveGraph(Graph, Optimal, OutError)
			&& RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
				Snapshot, LocalPositions, Graph, Optimal, OutCandidate, OutError);
	}

	FRouteAssignmentMinimumCostMatchingResult MakeOptimal(
		const int32 EligibleCount,
		const int32 RouteCount,
		const TArray<FRouteAssignmentMinimumCostPair>& Pairs)
	{
		FRouteAssignmentMinimumCostMatchingResult Optimal;
		Optimal.EligibleDroneCount = EligibleCount;
		Optimal.RouteSlotCount = RouteCount;
		Optimal.MatchedCardinality = Pairs.Num();
		Optimal.Pairs = Pairs;
		for (const FRouteAssignmentMinimumCostPair& Pair : Pairs)
		{
			Optimal.TotalHorizontalDistanceMeters += Pair.HorizontalDistanceMeters;
		}
		return Optimal;
	}

	FRouteAssignmentMinimumCostPair Pair(
		const TCHAR* DroneID,
		const int32 RouteIndex,
		const double Distance)
	{
		FRouteAssignmentMinimumCostPair Result;
		Result.DroneID = DroneID;
		Result.RouteSlotIndex = RouteIndex;
		Result.HorizontalDistanceMeters = Distance;
		return Result;
	}

	bool CandidatesEquivalent(
		const FRouteAssignmentCandidate& Left,
		const FRouteAssignmentCandidate& Right)
	{
		if (Left.SourceIdentity.FleetSessionID != Right.SourceIdentity.FleetSessionID
			|| Left.SourceIdentity.FleetRevisionAtCapture
				!= Right.SourceIdentity.FleetRevisionAtCapture
			|| Left.SourceIdentity.RouteDatasetGeneration
				!= Right.SourceIdentity.RouteDatasetGeneration
			|| Left.SourceIdentity.CapturedAtUtc != Right.SourceIdentity.CapturedAtUtc
			|| Left.CandidateDroneCount != Right.CandidateDroneCount
			|| Left.RouteSlotCount != Right.RouteSlotCount
			|| Left.MatchedEntries.Num() != Right.MatchedEntries.Num()
			|| Left.SpareDrones.Num() != Right.SpareDrones.Num()
			|| Left.ExcludedDrones.Num() != Right.ExcludedDrones.Num()
			|| Left.UnresolvedDrones.Num() != Right.UnresolvedDrones.Num()
			|| Left.VacantRouteSlots != Right.VacantRouteSlots
			|| Left.Conflicts.Num() != Right.Conflicts.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.MatchedEntries.Num(); ++Index)
		{
			const FRouteAssignmentEntry& A = Left.MatchedEntries[Index];
			const FRouteAssignmentEntry& B = Right.MatchedEntries[Index];
			if (A.DroneID != B.DroneID || A.RouteSlotIndex != B.RouteSlotIndex
				|| A.HorizontalDistanceMeters != B.HorizontalDistanceMeters
				|| A.bHorizontalDistanceValid != B.bHorizontalDistanceValid)
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < Left.SpareDrones.Num(); ++Index)
		{
			if (Left.SpareDrones[Index].DroneID != Right.SpareDrones[Index].DroneID)
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < Left.ExcludedDrones.Num(); ++Index)
		{
			if (Left.ExcludedDrones[Index].DroneID != Right.ExcludedDrones[Index].DroneID
				|| Left.ExcludedDrones[Index].Reason != Right.ExcludedDrones[Index].Reason)
			{
				return false;
			}
		}
		for (int32 Index = 0; Index < Left.UnresolvedDrones.Num(); ++Index)
		{
			if (Left.UnresolvedDrones[Index].DroneID
					!= Right.UnresolvedDrones[Index].DroneID
				|| Left.UnresolvedDrones[Index].Reason
					!= Right.UnresolvedDrones[Index].Reason)
			{
				return false;
			}
		}
		return true;
	}

	void MakeTwoByTwo(
		FRouteAssignmentSourceSnapshot& Snapshot,
		TArray<FRouteAssignmentDroneLocalPosition>& Locals,
		FRouteAssignmentCandidateGraph& Graph)
	{
		Snapshot = MakeSnapshot(2);
		AddDrone(Snapshot, Locals, TEXT("Alpha"));
		AddDrone(Snapshot, Locals, TEXT("Bravo"));
		Graph = MakeGraph(
			{TEXT("Alpha"), TEXT("Bravo")}, 2, {{0, 1}, {0, 1}},
			{{0.01, 0.15}, {0.14, 0.02}});
	}

	bool RunExcludedCase(
		FAutomationTestBase& Test,
		const ERouteAssignmentExcludedReason ExpectedReason,
		TFunctionRef<void(FRouteAssignmentDroneSource&)> MutateSource)
	{
		FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
		TArray<FRouteAssignmentDroneLocalPosition> Locals;
		AddDrone(Snapshot, Locals, TEXT("Alpha"), false);
		MutateSource(Snapshot.Drones[0]);
		const FRouteAssignmentCandidateGraph Graph = MakeGraph({}, 1, {});
		FRouteAssignmentCandidate Candidate;
		FString Error;
		const bool bResult = MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error);
		Test.TestTrue(TEXT("Materialization succeeds"), bResult);
		Test.TestEqual(TEXT("Exactly one excluded Drone"), Candidate.ExcludedDrones.Num(), 1);
		if (Candidate.ExcludedDrones.Num() == 1)
		{
			Test.TestEqual(TEXT("Excluded reason"), Candidate.ExcludedDrones[0].Reason,
				ExpectedReason);
		}
		return true;
	}
}

#define IMPLEMENT_MATERIALIZER_TEST(ClassName, LeafName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, \
		"DroneGCS.RouteAssignment.Materializer." LeafName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_MATERIALIZER_TEST(FMaterializer01EqualCountsTest, "01EqualCountsMatchedOnly")
bool FMaterializer01EqualCountsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	FRouteAssignmentCandidateGraph Graph;
	MakeTwoByTwo(Snapshot, Locals, Graph);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("All matched"), Candidate.MatchedEntries.Num(), 2);
	TestEqual(TEXT("No spare"), Candidate.SpareDrones.Num(), 0);
	TestEqual(TEXT("No excluded"), Candidate.ExcludedDrones.Num(), 0);
	TestEqual(TEXT("No unresolved"), Candidate.UnresolvedDrones.Num(), 0);
	TestEqual(TEXT("No vacant"), Candidate.VacantRouteSlots.Num(), 0);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer02SpareTest, "02MoreDronesProducesSpare")
bool FMaterializer02SpareTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo"), TEXT("Charlie")}, 2,
		{{0}, {1}, {0}}, {{0.01}, {0.01}, {0.20}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("Two matched"), Candidate.MatchedEntries.Num(), 2);
	TestEqual(TEXT("One spare"), Candidate.SpareDrones.Num(), 1);
	TestEqual(TEXT("Charlie is spare"), Candidate.SpareDrones[0].DroneID, FString(TEXT("Charlie")));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer03VacantTest, "03FewerDronesProducesVacant")
bool FMaterializer03VacantTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(3);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 3, {{0}, {1}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("Two matched"), Candidate.MatchedEntries.Num(), 2);
	TestEqual(TEXT("One vacant"), Candidate.VacantRouteSlots.Num(), 1);
	TestEqual(TEXT("Route two vacant"), Candidate.VacantRouteSlots[0], 2);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer04DeficientTest, "04DeficientGraphProducesUnresolved")
bool FMaterializer04DeficientTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(3);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo"), TEXT("Charlie")}, 3,
		{{0}, {0}, {1}}, {{0.01}, {0.02}, {0.01}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("Deficient cardinality"), Candidate.MatchedEntries.Num(), 2);
	TestEqual(TEXT("One unresolved"), Candidate.UnresolvedDrones.Num(), 1);
	TestEqual(TEXT("No spare"), Candidate.SpareDrones.Num(), 0);
	TestEqual(TEXT("One vacant"), Candidate.VacantRouteSlots.Num(), 1);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer05NoCandidateTest, "05DegreeZeroIsNoCandidate")
bool FMaterializer05NoCandidateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 2, {{0}, {}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("One unresolved"), Candidate.UnresolvedDrones.Num(), 1);
	TestEqual(TEXT("Reason is NoCandidate"), Candidate.UnresolvedDrones[0].Reason,
		ERouteAssignmentUnresolvedReason::NoCandidate);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer06ConflictTest, "06DegreePositiveIsConflict")
bool FMaterializer06ConflictTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(3);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo"), TEXT("Charlie")}, 3,
		{{0}, {0}, {1}}, {{0.01}, {0.02}, {0.01}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("One unresolved"), Candidate.UnresolvedDrones.Num(), 1);
	TestEqual(TEXT("Reason is Conflict"), Candidate.UnresolvedDrones[0].Reason,
		ERouteAssignmentUnresolvedReason::Conflict);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer07NotAmbiguousTest, "07MultipleCandidatesNotAmbiguous")
bool FMaterializer07NotAmbiguousTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	FRouteAssignmentCandidateGraph Graph;
	MakeTwoByTwo(Snapshot, Locals, Graph);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("No unresolved"), Candidate.UnresolvedDrones.Num(), 0);
	TestEqual(TEXT("No diagnostics"), Candidate.Conflicts.Num(), 0);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer08OfflineTest, "08OfflineExcluded")
bool FMaterializer08OfflineTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunExcludedCase(*this, ERouteAssignmentExcludedReason::Offline,
		[](FRouteAssignmentDroneSource& Source) { Source.bOnline = false; });
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer09InvalidGpsTest, "09InvalidGPSExcluded")
bool FMaterializer09InvalidGpsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunExcludedCase(*this, ERouteAssignmentExcludedReason::InvalidGPS,
		[](FRouteAssignmentDroneSource& Source) { Source.bGPSValid = false; });
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer10InsufficientFixTest, "10InsufficientFixExcluded")
bool FMaterializer10InsufficientFixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunExcludedCase(*this, ERouteAssignmentExcludedReason::InsufficientGPSFix,
		[](FRouteAssignmentDroneSource& Source) { Source.GPSFixType = 2; });
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer11InvalidLatLonTest, "11InvalidLatLonExcluded")
bool FMaterializer11InvalidLatLonTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunExcludedCase(*this, ERouteAssignmentExcludedReason::InvalidGeo,
		[](FRouteAssignmentDroneSource& Source) { Source.Latitude = 100.0; });
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer12DerivedInvalidTest, "12DerivedGeoInvalidExcluded")
bool FMaterializer12DerivedInvalidTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	return RunExcludedCase(*this, ERouteAssignmentExcludedReason::InvalidGeo,
		[](FRouteAssignmentDroneSource&) {});
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer13DronePartitionTest, "13DronePartitionComplete")
bool FMaterializer13DronePartitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"), false);
	Snapshot.Drones[1].bOnline = false;
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Charlie")}, 2, {{0}, {}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	const int32 Covered = Candidate.MatchedEntries.Num() + Candidate.SpareDrones.Num()
		+ Candidate.ExcludedDrones.Num() + Candidate.UnresolvedDrones.Num();
	TestEqual(TEXT("Drone partition complete"), Covered, Candidate.CandidateDroneCount);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer14RoutePartitionTest, "14RoutePartitionComplete")
bool FMaterializer14RoutePartitionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(3);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 3, {{1}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("Route partition complete"),
		Candidate.MatchedEntries.Num() + Candidate.VacantRouteSlots.Num(),
		Candidate.RouteSlotCount);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer15DisjointIdentityTest, "15DroneBucketsDisjoint")
bool FMaterializer15DisjointIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"), false);
	Snapshot.Drones[1].bOnline = false;
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Charlie")}, 2, {{0}, {}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TSet<FString> IDs;
	for (const FRouteAssignmentEntry& Item : Candidate.MatchedEntries) { IDs.Add(Item.DroneID.ToLower()); }
	for (const FRouteAssignmentSpareDrone& Item : Candidate.SpareDrones) { IDs.Add(Item.DroneID.ToLower()); }
	for (const FRouteAssignmentExcludedDrone& Item : Candidate.ExcludedDrones) { IDs.Add(Item.DroneID.ToLower()); }
	for (const FRouteAssignmentUnresolvedDrone& Item : Candidate.UnresolvedDrones) { IDs.Add(Item.DroneID.ToLower()); }
	TestEqual(TEXT("No identity appears twice"), IDs.Num(), Candidate.CandidateDroneCount);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer16VacantNoSpareTest, "16VacantForbidsSpare")
bool FMaterializer16VacantNoSpareTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(3);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo"), TEXT("Charlie")}, 3, {{0}, {0}, {1}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestTrue(TEXT("Vacant exists"), !Candidate.VacantRouteSlots.IsEmpty());
	TestEqual(TEXT("Spare is empty"), Candidate.SpareDrones.Num(), 0);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer17ZeroRoutesTest, "17ZeroRoutesMakesEligibleSpare")
bool FMaterializer17ZeroRoutesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(0);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 0, {{}, {}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("All eligible are spare"), Candidate.SpareDrones.Num(), 2);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer18ZeroEligibleTest, "18ZeroEligibleAllRoutesVacant")
bool FMaterializer18ZeroEligibleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"), false);
	Snapshot.Drones[0].bOnline = false;
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({}, 2, {});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("No match"), Candidate.MatchedEntries.Num(), 0);
	TestEqual(TEXT("All routes vacant"), Candidate.VacantRouteSlots.Num(), 2);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer19OverThresholdTest, "19PairOverProductionThresholdFails")
bool FMaterializer19OverThresholdTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha")}, 1, {{0}}, {{0.21}});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(1, 1, {Pair(TEXT("Alpha"), 0, 0.21)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Over-threshold input fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	TestEqual(TEXT("Output remains empty"), Candidate.CandidateDroneCount, 0);
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer20DistanceMismatchTest, "20PairDistanceMismatchFails")
bool FMaterializer20DistanceMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 1, {{0}}, {{0.10}});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(1, 1, {Pair(TEXT("Alpha"), 0, 0.11)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Distance mismatch fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer21AbsentEdgeTest, "21PairAbsentFromGraphFails")
bool FMaterializer21AbsentEdgeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 2, {{0}});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(1, 2, {Pair(TEXT("Alpha"), 1, 0.10)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Absent edge fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer22DuplicateDroneTest, "22DuplicatePairDroneFails")
bool FMaterializer22DuplicateDroneTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	FRouteAssignmentCandidateGraph Graph;
	MakeTwoByTwo(Snapshot, Locals, Graph);
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(2, 2,
		{Pair(TEXT("Alpha"), 0, 0.01), Pair(TEXT("Alpha"), 1, 0.15)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Duplicate Drone fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer23DuplicateRouteTest, "23DuplicatePairRouteFails")
bool FMaterializer23DuplicateRouteTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	FRouteAssignmentCandidateGraph Graph;
	MakeTwoByTwo(Snapshot, Locals, Graph);
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(2, 2,
		{Pair(TEXT("Alpha"), 0, 0.01), Pair(TEXT("Bravo"), 0, 0.14)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Duplicate Route fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer24LocalCountTest, "24LocalCountMismatchFails")
bool FMaterializer24LocalCountTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	Locals.Reset();
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({}, 1, {});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(0, 1, {});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Local count mismatch fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer25IdentityMismatchTest, "25LocalSourceIdentityMismatchFails")
bool FMaterializer25IdentityMismatchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	Locals[0].DroneID = TEXT("Bravo");
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 1, {{0}});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(1, 1, {Pair(TEXT("Alpha"), 0, 0.10)});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Identity mismatch fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer26MissingEligibleTest, "26GraphMissingEligibleDroneFails")
bool FMaterializer26MissingEligibleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 1, {{0}});
	FRouteAssignmentMinimumCostMatchingResult Optimal;
	FString Error;
	TestTrue(TEXT("Reduced graph itself solves"), SolveGraph(Graph, Optimal, Error));
	FRouteAssignmentCandidate Candidate;
	TestFalse(TEXT("Missing eligible Drone fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer27ExtraEligibleTest, "27GraphExtraEligibleDroneFails")
bool FMaterializer27ExtraEligibleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(1);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"), false);
	Snapshot.Drones[1].bOnline = false;
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 1, {{0}, {0}});
	FRouteAssignmentMinimumCostMatchingResult Optimal;
	FString Error;
	TestTrue(TEXT("Expanded graph itself solves"), SolveGraph(Graph, Optimal, Error));
	FRouteAssignmentCandidate Candidate;
	TestFalse(TEXT("Extra eligible Drone fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer28RouteCountTest, "28RouteCountMismatchFails")
bool FMaterializer28RouteCountTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({TEXT("Alpha")}, 1, {{0}});
	FRouteAssignmentMinimumCostMatchingResult Optimal;
	FString Error;
	TestTrue(TEXT("Graph itself solves"), SolveGraph(Graph, Optimal, Error));
	FRouteAssignmentCandidate Candidate;
	TestFalse(TEXT("Route count mismatch fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, Locals, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer29SourceIdentityTest, "29InvalidSourceIdentityFails")
bool FMaterializer29SourceIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(0);
	Snapshot.SourceIdentity.FleetSessionID.Reset();
	const FRouteAssignmentCandidateGraph Graph = MakeGraph({}, 0, {});
	const FRouteAssignmentMinimumCostMatchingResult Optimal = MakeOptimal(0, 0, {});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestFalse(TEXT("Invalid SourceIdentity fails"), RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
		Snapshot, {}, Graph, Optimal, Candidate, Error));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer30CanonicalOrderTest, "30CanonicalOutputOrdering")
bool FMaterializer30CanonicalOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Delta"));
	AddDrone(Snapshot, Locals, TEXT("Echo"), false);
	Snapshot.Drones[1].bGPSValid = false;
	AddDrone(Snapshot, Locals, TEXT("Charlie"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"), false);
	Snapshot.Drones[3].bOnline = false;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	const FRouteAssignmentCandidateGraph Graph = MakeGraph(
		{TEXT("Alpha"), TEXT("Charlie"), TEXT("Delta")}, 2,
		{{0}, {0}, {1}}, {{0.01}, {0.19}, {0.01}});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Materialization succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Candidate, Error));
	TestEqual(TEXT("Matched route zero first"), Candidate.MatchedEntries[0].RouteSlotIndex, 0);
	TestEqual(TEXT("Matched route one second"), Candidate.MatchedEntries[1].RouteSlotIndex, 1);
	TestEqual(TEXT("Canonical spare"), Candidate.SpareDrones[0].DroneID, FString(TEXT("Charlie")));
	TestEqual(TEXT("Excluded Bravo first"), Candidate.ExcludedDrones[0].DroneID, FString(TEXT("Bravo")));
	TestEqual(TEXT("Excluded Echo second"), Candidate.ExcludedDrones[1].DroneID, FString(TEXT("Echo")));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer31RepeatTest, "31RepeatInputEquivalent")
bool FMaterializer31RepeatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot;
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	FRouteAssignmentCandidateGraph Graph;
	MakeTwoByTwo(Snapshot, Locals, Graph);
	FRouteAssignmentCandidate First;
	FRouteAssignmentCandidate Second;
	FString Error;
	TestTrue(TEXT("First succeeds"), MaterializeSolved(Snapshot, Locals, Graph, First, Error));
	TestTrue(TEXT("Second succeeds"), MaterializeSolved(Snapshot, Locals, Graph, Second, Error));
	TestTrue(TEXT("Candidates equivalent"), CandidatesEquivalent(First, Second));
	return true;
}

IMPLEMENT_MATERIALIZER_TEST(FMaterializer32EdgeOrderTest, "32EquivalentEdgeOrderEquivalentCandidate")
bool FMaterializer32EdgeOrderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Snapshot = MakeSnapshot(2);
	TArray<FRouteAssignmentDroneLocalPosition> Locals;
	AddDrone(Snapshot, Locals, TEXT("Alpha"));
	AddDrone(Snapshot, Locals, TEXT("Bravo"));
	const FRouteAssignmentCandidateGraph FirstGraph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 2,
		{{0, 1}, {0, 1}}, {{0.01, 0.15}, {0.14, 0.02}});
	const FRouteAssignmentCandidateGraph SecondGraph = MakeGraph(
		{TEXT("Alpha"), TEXT("Bravo")}, 2,
		{{1, 0}, {1, 0}}, {{0.15, 0.01}, {0.02, 0.14}});
	FRouteAssignmentCandidate First;
	FRouteAssignmentCandidate Second;
	FString Error;
	TestTrue(TEXT("First ordering succeeds"), MaterializeSolved(Snapshot, Locals, FirstGraph, First, Error));
	TestTrue(TEXT("Second ordering succeeds"), MaterializeSolved(Snapshot, Locals, SecondGraph, Second, Error));
	TestTrue(TEXT("Semantic candidates equivalent"), CandidatesEquivalent(First, Second));
	return true;
}

#undef IMPLEMENT_MATERIALIZER_TEST

#endif
