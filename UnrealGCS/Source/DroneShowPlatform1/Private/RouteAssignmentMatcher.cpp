#include "RouteAssignmentMatcher.h"

#include "Containers/Queue.h"

#include <cmath>
#include <limits>
#include <queue>
#include <vector>

namespace
{
	int32 CompareDroneIDs(const FString& Left, const FString& Right)
	{
		const int32 IdentityOrder = Left.Compare(Right, ESearchCase::IgnoreCase);
		return IdentityOrder != 0
			? IdentityOrder
			: Left.Compare(Right, ESearchCase::CaseSensitive);
	}

	bool DroneIDLess(const FString& Left, const FString& Right)
	{
		return CompareDroneIDs(Left, Right) < 0;
	}

	bool CandidateEdgeLess(
		const FRouteAssignmentCandidateEdge& Left,
		const FRouteAssignmentCandidateEdge& Right)
	{
		if (Left.HorizontalDistanceMeters != Right.HorizontalDistanceMeters)
		{
			return Left.HorizontalDistanceMeters < Right.HorizontalDistanceMeters;
		}
		return Left.RouteSlotIndex < Right.RouteSlotIndex;
	}

	bool ValidateCandidateGraph(
		const FRouteAssignmentCandidateGraph& Graph,
		FString& OutError)
	{
		if (Graph.EligibleDroneCount < 0
			|| Graph.EligibleDroneCount != Graph.EligibleDroneIDs.Num())
		{
			OutError = FString::Printf(
				TEXT("EligibleDroneCount %d does not match EligibleDroneIDs count %d."),
				Graph.EligibleDroneCount,
				Graph.EligibleDroneIDs.Num());
			return false;
		}

		if (Graph.RouteSlotCount < 0)
		{
			OutError = TEXT("RouteSlotCount must not be negative.");
			return false;
		}

		if (Graph.CandidateEdgeCount < 0
			|| Graph.CandidateEdgeCount != Graph.Edges.Num())
		{
			OutError = FString::Printf(
				TEXT("CandidateEdgeCount %d does not match Edges count %d."),
				Graph.CandidateEdgeCount,
				Graph.Edges.Num());
			return false;
		}

		if (Graph.EdgeOffsets.Num() != Graph.EligibleDroneCount + 1)
		{
			OutError = FString::Printf(
				TEXT("EdgeOffsets count %d must equal EligibleDroneCount + 1 (%d)."),
				Graph.EdgeOffsets.Num(),
				Graph.EligibleDroneCount + 1);
			return false;
		}

		if (Graph.EdgeOffsets[0] != 0
			|| Graph.EdgeOffsets.Last() != Graph.CandidateEdgeCount)
		{
			OutError = TEXT("EdgeOffsets must start at zero and end at CandidateEdgeCount.");
			return false;
		}

		TSet<FString> DroneIdentityKeys;
		for (int32 DroneIndex = 0; DroneIndex < Graph.EligibleDroneCount; ++DroneIndex)
		{
			const FString& DroneID = Graph.EligibleDroneIDs[DroneIndex];
			if (DroneID.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Eligible Drone index %d has an empty DroneID."),
					DroneIndex);
				return false;
			}

			FString IdentityKey = DroneID.ToLower();
			if (DroneIdentityKeys.Contains(IdentityKey))
			{
				OutError = FString::Printf(
					TEXT("EligibleDroneIDs contains duplicate Fleet identity '%s'."),
					*DroneID);
				return false;
			}
			DroneIdentityKeys.Add(MoveTemp(IdentityKey));

			if (DroneIndex > 0
				&& CompareDroneIDs(Graph.EligibleDroneIDs[DroneIndex - 1], DroneID) >= 0)
			{
				OutError = TEXT("EligibleDroneIDs must be in strict canonical order.");
				return false;
			}

			const int32 EdgeBegin = Graph.EdgeOffsets[DroneIndex];
			const int32 EdgeEnd = Graph.EdgeOffsets[DroneIndex + 1];
			if (EdgeBegin < 0
				|| EdgeEnd < EdgeBegin
				|| EdgeEnd > Graph.CandidateEdgeCount)
			{
				OutError = FString::Printf(
					TEXT("Edge range [%d, %d) for DroneID '%s' is invalid."),
					EdgeBegin,
					EdgeEnd,
					*DroneID);
				return false;
			}

			TSet<int32> DroneRouteSlots;
			for (int32 EdgeIndex = EdgeBegin; EdgeIndex < EdgeEnd; ++EdgeIndex)
			{
				const FRouteAssignmentCandidateEdge& Edge = Graph.Edges[EdgeIndex];
				if (Edge.DroneID != DroneID)
				{
					OutError = FString::Printf(
						TEXT("Edge %d DroneID '%s' does not match adjacency DroneID '%s'."),
						EdgeIndex,
						*Edge.DroneID,
						*DroneID);
					return false;
				}

				if (Edge.RouteSlotIndex < 0 || Edge.RouteSlotIndex >= Graph.RouteSlotCount)
				{
					OutError = FString::Printf(
						TEXT("Edge %d RouteSlotIndex %d is outside [0, %d)."),
						EdgeIndex,
						Edge.RouteSlotIndex,
						Graph.RouteSlotCount);
					return false;
				}

				if (DroneRouteSlots.Contains(Edge.RouteSlotIndex))
				{
					OutError = FString::Printf(
						TEXT("DroneID '%s' has duplicate edge to RouteSlotIndex %d."),
						*DroneID,
						Edge.RouteSlotIndex);
					return false;
				}
				DroneRouteSlots.Add(Edge.RouteSlotIndex);

				if (!FMath::IsFinite(Edge.HorizontalDistanceMeters)
					|| Edge.HorizontalDistanceMeters < 0.0
					|| Edge.HorizontalDistanceMeters
						> RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters)
				{
					OutError = FString::Printf(
						TEXT("Edge %d has distance outside the production candidate range."),
						EdgeIndex);
					return false;
				}
			}
		}

		return true;
	}

	bool TryAugment(
		const int32 DroneIndex,
		const TArray<TArray<int32>>& Adjacency,
		TArray<int32>& DroneToRoute,
		TArray<int32>& RouteToDrone,
		TArray<int32>& Layer,
		const int32 ShortestTerminalLayer)
	{
		for (const int32 RouteSlotIndex : Adjacency[DroneIndex])
		{
			const int32 MatchedDroneIndex = RouteToDrone[RouteSlotIndex];
			if ((MatchedDroneIndex == INDEX_NONE
					&& Layer[DroneIndex] == ShortestTerminalLayer)
				|| (MatchedDroneIndex != INDEX_NONE
					&& Layer[MatchedDroneIndex] == Layer[DroneIndex] + 1
					&& TryAugment(
						MatchedDroneIndex,
						Adjacency,
						DroneToRoute,
						RouteToDrone,
						Layer,
						ShortestTerminalLayer)))
			{
				DroneToRoute[DroneIndex] = RouteSlotIndex;
				RouteToDrone[RouteSlotIndex] = DroneIndex;
				return true;
			}
		}

		Layer[DroneIndex] = INDEX_NONE;
		return false;
	}

	bool FindCandidateDistance(
		const FRouteAssignmentCandidateGraph& Graph,
		const FString& DroneID,
		const int32 RouteSlotIndex,
		double& OutDistance)
	{
		const int32 DroneIndex = Graph.EligibleDroneIDs.IndexOfByKey(DroneID);
		if (DroneIndex == INDEX_NONE)
		{
			return false;
		}

		for (int32 EdgeIndex = Graph.EdgeOffsets[DroneIndex];
			EdgeIndex < Graph.EdgeOffsets[DroneIndex + 1];
			++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& Edge = Graph.Edges[EdgeIndex];
			if (Edge.RouteSlotIndex == RouteSlotIndex)
			{
				OutDistance = Edge.HorizontalDistanceMeters;
				return true;
			}
		}
		return false;
	}

	bool ValidateMaximumMatchingResult(
		const FRouteAssignmentCandidateGraph& Graph,
		const FRouteAssignmentMaximumMatchingResult& Result,
		FString& OutError)
	{
		if (Result.EligibleDroneCount != Graph.EligibleDroneCount
			|| Result.RouteSlotCount != Graph.RouteSlotCount)
		{
			OutError = TEXT("Maximum matching counts do not match the candidate graph.");
			return false;
		}

		if (Result.MaximumCardinality < 0
			|| Result.MaximumCardinality
				> FMath::Min(Graph.EligibleDroneCount, Graph.RouteSlotCount)
			|| Result.MaximumCardinality != Result.WitnessPairs.Num())
		{
			OutError = TEXT("MaximumCardinality is invalid or does not match WitnessPairs.");
			return false;
		}

		TSet<FString> DroneIdentityKeys;
		TSet<int32> RouteSlotIndices;
		for (int32 PairIndex = 0; PairIndex < Result.WitnessPairs.Num(); ++PairIndex)
		{
			const FRouteAssignmentMatchingPair& Pair = Result.WitnessPairs[PairIndex];
			if (Pair.DroneID.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("Maximum witness pair %d has an empty DroneID."),
					PairIndex);
				return false;
			}

			if (PairIndex > 0
				&& CompareDroneIDs(
					Result.WitnessPairs[PairIndex - 1].DroneID,
					Pair.DroneID) >= 0)
			{
				OutError = TEXT("Maximum witness pairs must use strict canonical DroneID order.");
				return false;
			}

			FString IdentityKey = Pair.DroneID.ToLower();
			if (DroneIdentityKeys.Contains(IdentityKey))
			{
				OutError = FString::Printf(
					TEXT("Maximum witness repeats DroneID '%s'."),
					*Pair.DroneID);
				return false;
			}
			DroneIdentityKeys.Add(MoveTemp(IdentityKey));

			if (RouteSlotIndices.Contains(Pair.RouteSlotIndex))
			{
				OutError = FString::Printf(
					TEXT("Maximum witness repeats RouteSlotIndex %d."),
					Pair.RouteSlotIndex);
				return false;
			}
			RouteSlotIndices.Add(Pair.RouteSlotIndex);

			double CandidateDistance = 0.0;
			if (!FindCandidateDistance(
				Graph,
				Pair.DroneID,
				Pair.RouteSlotIndex,
				CandidateDistance))
			{
				OutError = FString::Printf(
					TEXT("Maximum witness pair '%s' -> %d is not a candidate edge."),
					*Pair.DroneID,
					Pair.RouteSlotIndex);
				return false;
			}
		}

		return true;
	}

	double CanonicalCompensatedSum(
		const TArray<FRouteAssignmentMinimumCostPair>& Pairs)
	{
		double Sum = 0.0;
		double Compensation = 0.0;
		for (const FRouteAssignmentMinimumCostPair& Pair : Pairs)
		{
			const double Value = Pair.HorizontalDistanceMeters;
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

	bool NormalizeReducedCostInternal(
		const double PhysicalCost,
		const double FromPotential,
		const double ToPotential,
		double& OutReducedCost,
		FString& OutError)
	{
		if (!FMath::IsFinite(PhysicalCost)
			|| !FMath::IsFinite(FromPotential)
			|| !FMath::IsFinite(ToPotential))
		{
			OutError = TEXT("NumericalInvariantFailure: non-finite reduced-cost input.");
			return false;
		}

		OutReducedCost = PhysicalCost + FromPotential - ToPotential;
		if (!FMath::IsFinite(OutReducedCost))
		{
			OutError = TEXT("NumericalInvariantFailure: non-finite reduced cost.");
			return false;
		}

		if (OutReducedCost < 0.0)
		{
			const double Scale = FMath::Max(
				1.0,
				FMath::Max(
					FMath::Abs(PhysicalCost),
					FMath::Max(FMath::Abs(FromPotential), FMath::Abs(ToPotential))));
			const double RoundoffTolerance =
				64.0 * std::numeric_limits<double>::epsilon() * Scale;
			if (OutReducedCost < -RoundoffTolerance)
			{
				OutError = FString::Printf(
					TEXT("NumericalInvariantFailure: reduced cost %.17g is below tolerance %.17g."),
					OutReducedCost,
					RoundoffTolerance);
				return false;
			}
			OutReducedCost = 0.0;
		}

		return true;
	}

	struct FResidualEdge
	{
		int32 ToNode = INDEX_NONE;
		int32 ReverseEdgeIndex = INDEX_NONE;
		int32 ResidualCapacity = 0;
		double PhysicalCost = 0.0;
		int32 StableOrder = INDEX_NONE;
	};

	using FResidualNetwork = TArray<TArray<FResidualEdge>>;

	void AddResidualEdge(
		FResidualNetwork& Network,
		const int32 FromNode,
		const int32 ToNode,
		const double PhysicalCost,
		int32& NextStableOrder)
	{
		FResidualEdge Forward;
		Forward.ToNode = ToNode;
		Forward.ReverseEdgeIndex = Network[ToNode].Num();
		Forward.ResidualCapacity = 1;
		Forward.PhysicalCost = PhysicalCost;
		Forward.StableOrder = NextStableOrder++;

		FResidualEdge Reverse;
		Reverse.ToNode = FromNode;
		Reverse.ReverseEdgeIndex = Network[FromNode].Num();
		Reverse.ResidualCapacity = 0;
		Reverse.PhysicalCost = -PhysicalCost;
		Reverse.StableOrder = NextStableOrder++;

		Network[FromNode].Add(Forward);
		Network[ToNode].Add(Reverse);
	}

	struct FShortestPathQueueEntry
	{
		double Distance = 0.0;
		int32 Node = INDEX_NONE;
	};

	struct FShortestPathQueueGreater
	{
		bool operator()(
			const FShortestPathQueueEntry& Left,
			const FShortestPathQueueEntry& Right) const
		{
			if (Left.Distance != Right.Distance)
			{
				return Left.Distance > Right.Distance;
			}
			return Left.Node > Right.Node;
		}
	};

	bool PreferPredecessor(
		const FResidualNetwork& Network,
		const int32 CandidateFromNode,
		const int32 CandidateEdgeIndex,
		const int32 ExistingFromNode,
		const int32 ExistingEdgeIndex)
	{
		if (ExistingFromNode == INDEX_NONE)
		{
			return true;
		}
		if (CandidateFromNode != ExistingFromNode)
		{
			return CandidateFromNode < ExistingFromNode;
		}
		return Network[CandidateFromNode][CandidateEdgeIndex].StableOrder
			< Network[ExistingFromNode][ExistingEdgeIndex].StableOrder;
	}

	bool FindShortestResidualPath(
		const FResidualNetwork& Network,
		const int32 SourceNode,
		const int32 SinkNode,
		const TArray<double>& Potentials,
		TArray<double>& OutDistances,
		TArray<int32>& OutPreviousNodes,
		TArray<int32>& OutPreviousEdges,
		FString& OutError)
	{
		const double Infinity = std::numeric_limits<double>::infinity();
		OutDistances.Init(Infinity, Network.Num());
		OutPreviousNodes.Init(INDEX_NONE, Network.Num());
		OutPreviousEdges.Init(INDEX_NONE, Network.Num());
		TArray<bool> Settled;
		Settled.Init(false, Network.Num());

		std::priority_queue<
			FShortestPathQueueEntry,
			std::vector<FShortestPathQueueEntry>,
			FShortestPathQueueGreater> Queue;
		OutDistances[SourceNode] = 0.0;
		Queue.push({ 0.0, SourceNode });

		while (!Queue.empty())
		{
			const FShortestPathQueueEntry Current = Queue.top();
			Queue.pop();
			if (Settled[Current.Node]
				|| Current.Distance != OutDistances[Current.Node])
			{
				continue;
			}
			Settled[Current.Node] = true;

			for (int32 EdgeIndex = 0;
				EdgeIndex < Network[Current.Node].Num();
				++EdgeIndex)
			{
				const FResidualEdge& Edge = Network[Current.Node][EdgeIndex];
				if (Edge.ResidualCapacity <= 0 || Settled[Edge.ToNode])
				{
					continue;
				}

				double ReducedCost = 0.0;
				if (!NormalizeReducedCostInternal(
					Edge.PhysicalCost,
					Potentials[Current.Node],
					Potentials[Edge.ToNode],
					ReducedCost,
					OutError))
				{
					return false;
				}

				const double CandidateDistance = Current.Distance + ReducedCost;
				if (!FMath::IsFinite(CandidateDistance))
				{
					OutError = TEXT(
						"NumericalInvariantFailure: non-finite Dijkstra distance.");
					return false;
				}

				if (CandidateDistance < OutDistances[Edge.ToNode]
					|| (CandidateDistance == OutDistances[Edge.ToNode]
						&& PreferPredecessor(
							Network,
							Current.Node,
							EdgeIndex,
							OutPreviousNodes[Edge.ToNode],
							OutPreviousEdges[Edge.ToNode])))
				{
					const bool bDistanceChanged =
						CandidateDistance != OutDistances[Edge.ToNode];
					OutDistances[Edge.ToNode] = CandidateDistance;
					OutPreviousNodes[Edge.ToNode] = Current.Node;
					OutPreviousEdges[Edge.ToNode] = EdgeIndex;
					if (bDistanceChanged)
					{
						Queue.push({ CandidateDistance, Edge.ToNode });
					}
				}
			}
		}

		return FMath::IsFinite(OutDistances[SinkNode]);
	}

	bool ValidateMinimumCostResult(
		const FRouteAssignmentCandidateGraph& Graph,
		const int32 VerifiedMaximumCardinality,
		const FRouteAssignmentMinimumCostMatchingResult& Result,
		FString& OutError)
	{
		if (Result.EligibleDroneCount != Graph.EligibleDroneCount
			|| Result.RouteSlotCount != Graph.RouteSlotCount
			|| Result.MatchedCardinality != VerifiedMaximumCardinality
			|| Result.Pairs.Num() != Result.MatchedCardinality)
		{
			OutError = TEXT("Minimum-cost result counts are inconsistent.");
			return false;
		}

		TSet<FString> DroneIdentityKeys;
		TSet<int32> RouteSlotIndices;
		for (int32 PairIndex = 0; PairIndex < Result.Pairs.Num(); ++PairIndex)
		{
			const FRouteAssignmentMinimumCostPair& Pair = Result.Pairs[PairIndex];
			if (PairIndex > 0
				&& CompareDroneIDs(Result.Pairs[PairIndex - 1].DroneID, Pair.DroneID) >= 0)
			{
				OutError = TEXT("Minimum-cost pairs are not in strict canonical DroneID order.");
				return false;
			}

			FString IdentityKey = Pair.DroneID.ToLower();
			if (DroneIdentityKeys.Contains(IdentityKey)
				|| RouteSlotIndices.Contains(Pair.RouteSlotIndex))
			{
				OutError = TEXT("Minimum-cost result violates one-to-one uniqueness.");
				return false;
			}
			DroneIdentityKeys.Add(MoveTemp(IdentityKey));
			RouteSlotIndices.Add(Pair.RouteSlotIndex);

			double CandidateDistance = 0.0;
			if (!FindCandidateDistance(
				Graph,
				Pair.DroneID,
				Pair.RouteSlotIndex,
				CandidateDistance)
				|| Pair.HorizontalDistanceMeters != CandidateDistance
				|| !FMath::IsFinite(Pair.HorizontalDistanceMeters)
				|| Pair.HorizontalDistanceMeters < 0.0
				|| Pair.HorizontalDistanceMeters
					> RouteAssignmentMatcher::ProductionMaxMatchDistanceMeters)
			{
				OutError = TEXT("Minimum-cost pair is not an exact valid candidate edge.");
				return false;
			}
		}

		const double CanonicalTotal = CanonicalCompensatedSum(Result.Pairs);
		if (!FMath::IsFinite(Result.TotalHorizontalDistanceMeters)
			|| Result.TotalHorizontalDistanceMeters < 0.0
			|| Result.TotalHorizontalDistanceMeters != CanonicalTotal)
		{
			OutError = TEXT("Minimum-cost total does not equal the canonical compensated sum.");
			return false;
		}

		return true;
	}
}

bool RouteAssignmentMatcher::BuildCandidateEdges(
	const FRouteAssignmentSourceSnapshot& SourceSnapshot,
	const TArray<FRouteAssignmentDroneLocalPosition>& DroneLocalPositions,
	const FRouteAssignmentMatcherConfig& Config,
	FRouteAssignmentCandidateGraph& OutGraph,
	FString& OutError)
{
	OutGraph = FRouteAssignmentCandidateGraph{};
	OutError.Reset();

	if (!FMath::IsFinite(Config.MaxMatchDistanceMeters)
		|| Config.MaxMatchDistanceMeters <= 0.0)
	{
		OutError = TEXT("MaxMatchDistanceMeters must be finite and greater than zero.");
		return false;
	}

	if (DroneLocalPositions.Num() != SourceSnapshot.Drones.Num())
	{
		OutError = FString::Printf(
			TEXT("Drone local position count %d does not match source drone count %d."),
			DroneLocalPositions.Num(),
			SourceSnapshot.Drones.Num());
		return false;
	}

	TSet<FString> SourceDroneIdentityKeys;
	TArray<int32> EligibleDroneIndices;
	EligibleDroneIndices.Reserve(DroneLocalPositions.Num());
	for (int32 DroneIndex = 0; DroneIndex < DroneLocalPositions.Num(); ++DroneIndex)
	{
		const FRouteAssignmentDroneSource& SourceDrone = SourceSnapshot.Drones[DroneIndex];
		const FRouteAssignmentDroneLocalPosition& LocalPosition = DroneLocalPositions[DroneIndex];
		if (SourceDrone.DroneID.IsEmpty() || LocalPosition.DroneID.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Drone index %d contains an empty DroneID."), DroneIndex);
			return false;
		}

		if (!SourceDrone.DroneID.Equals(LocalPosition.DroneID, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Drone identity mismatch at index %d: source '%s', local position '%s'."),
				DroneIndex,
				*SourceDrone.DroneID,
				*LocalPosition.DroneID);
			return false;
		}

		FString IdentityKey = SourceDrone.DroneID.ToLower();
		if (SourceDroneIdentityKeys.Contains(IdentityKey))
		{
			OutError = FString::Printf(
				TEXT("Source snapshot contains duplicate DroneID '%s'."),
				*SourceDrone.DroneID);
			return false;
		}
		SourceDroneIdentityKeys.Add(MoveTemp(IdentityKey));

		if (LocalPosition.bAssignmentGeoEligible)
		{
			if (!FMath::IsFinite(LocalPosition.EastMeters)
				|| !FMath::IsFinite(LocalPosition.NorthMeters))
			{
				OutError = FString::Printf(
					TEXT("Eligible DroneID '%s' has non-finite Local EN coordinates."),
					*LocalPosition.DroneID);
				return false;
			}
			EligibleDroneIndices.Add(DroneIndex);
		}
	}

	const int32 RouteSlotCount = SourceSnapshot.RouteSlots.Num();
	TSet<int32> RouteSlotIndices;
	for (const FRouteAssignmentRouteSlotSource& RouteSlot : SourceSnapshot.RouteSlots)
	{
		if (RouteSlot.RouteSlotIndex < 0 || RouteSlot.RouteSlotIndex >= RouteSlotCount)
		{
			OutError = FString::Printf(
				TEXT("RouteSlotIndex %d is outside [0, %d)."),
				RouteSlot.RouteSlotIndex,
				RouteSlotCount);
			return false;
		}

		if (RouteSlotIndices.Contains(RouteSlot.RouteSlotIndex))
		{
			OutError = FString::Printf(
				TEXT("Duplicate RouteSlotIndex %d."),
				RouteSlot.RouteSlotIndex);
			return false;
		}

		if (!FMath::IsFinite(RouteSlot.EastMeters)
			|| !FMath::IsFinite(RouteSlot.NorthMeters))
		{
			OutError = FString::Printf(
				TEXT("RouteSlotIndex %d has non-finite Local EN coordinates."),
				RouteSlot.RouteSlotIndex);
			return false;
		}

		RouteSlotIndices.Add(RouteSlot.RouteSlotIndex);
	}

	EligibleDroneIndices.Sort(
		[&SourceSnapshot](const int32 LeftIndex, const int32 RightIndex)
		{
			return DroneIDLess(
				SourceSnapshot.Drones[LeftIndex].DroneID,
				SourceSnapshot.Drones[RightIndex].DroneID);
		});

	OutGraph.EligibleDroneCount = EligibleDroneIndices.Num();
	OutGraph.RouteSlotCount = RouteSlotCount;
	OutGraph.EligibleDroneIDs.Reserve(EligibleDroneIndices.Num());
	OutGraph.EdgeOffsets.Reserve(EligibleDroneIndices.Num() + 1);

	for (const int32 DroneIndex : EligibleDroneIndices)
	{
		const FRouteAssignmentDroneLocalPosition& Drone = DroneLocalPositions[DroneIndex];
		const FString& AuthoritativeDroneID = SourceSnapshot.Drones[DroneIndex].DroneID;
		OutGraph.EligibleDroneIDs.Add(AuthoritativeDroneID);
		OutGraph.EdgeOffsets.Add(OutGraph.Edges.Num());

		TArray<FRouteAssignmentCandidateEdge> DroneEdges;
		for (const FRouteAssignmentRouteSlotSource& RouteSlot : SourceSnapshot.RouteSlots)
		{
			const double DeltaEast = Drone.EastMeters - RouteSlot.EastMeters;
			const double DeltaNorth = Drone.NorthMeters - RouteSlot.NorthMeters;
			const double HorizontalDistanceMeters = std::hypot(DeltaEast, DeltaNorth);
			if (HorizontalDistanceMeters <= Config.MaxMatchDistanceMeters)
			{
				FRouteAssignmentCandidateEdge Edge;
				Edge.DroneID = AuthoritativeDroneID;
				Edge.RouteSlotIndex = RouteSlot.RouteSlotIndex;
				Edge.HorizontalDistanceMeters = HorizontalDistanceMeters;
				DroneEdges.Add(MoveTemp(Edge));
			}
		}

		DroneEdges.Sort(CandidateEdgeLess);
		OutGraph.Edges.Append(MoveTemp(DroneEdges));
	}

	OutGraph.EdgeOffsets.Add(OutGraph.Edges.Num());
	OutGraph.CandidateEdgeCount = OutGraph.Edges.Num();
	return true;
}

bool RouteAssignmentMatcher::SolveMaximumCardinality(
	const FRouteAssignmentCandidateGraph& CandidateGraph,
	FRouteAssignmentMaximumMatchingResult& OutResult,
	FString& OutError)
{
	OutResult = FRouteAssignmentMaximumMatchingResult{};
	OutError.Reset();

	if (!ValidateCandidateGraph(CandidateGraph, OutError))
	{
		return false;
	}

	OutResult.EligibleDroneCount = CandidateGraph.EligibleDroneCount;
	OutResult.RouteSlotCount = CandidateGraph.RouteSlotCount;

	TArray<TArray<int32>> Adjacency;
	Adjacency.SetNum(CandidateGraph.EligibleDroneCount);
	for (int32 DroneIndex = 0; DroneIndex < CandidateGraph.EligibleDroneCount; ++DroneIndex)
	{
		const int32 EdgeBegin = CandidateGraph.EdgeOffsets[DroneIndex];
		const int32 EdgeEnd = CandidateGraph.EdgeOffsets[DroneIndex + 1];
		Adjacency[DroneIndex].Reserve(EdgeEnd - EdgeBegin);
		for (int32 EdgeIndex = EdgeBegin; EdgeIndex < EdgeEnd; ++EdgeIndex)
		{
			Adjacency[DroneIndex].Add(CandidateGraph.Edges[EdgeIndex].RouteSlotIndex);
		}
		Adjacency[DroneIndex].Sort();
	}

	TArray<int32> DroneToRoute;
	DroneToRoute.Init(INDEX_NONE, CandidateGraph.EligibleDroneCount);
	TArray<int32> RouteToDrone;
	RouteToDrone.Init(INDEX_NONE, CandidateGraph.RouteSlotCount);
	TArray<int32> Layer;
	Layer.Init(INDEX_NONE, CandidateGraph.EligibleDroneCount);

	for (;;)
	{
		TQueue<int32> Queue;
		for (int32 DroneIndex = 0; DroneIndex < CandidateGraph.EligibleDroneCount; ++DroneIndex)
		{
			if (DroneToRoute[DroneIndex] == INDEX_NONE)
			{
				Layer[DroneIndex] = 0;
				Queue.Enqueue(DroneIndex);
			}
			else
			{
				Layer[DroneIndex] = INDEX_NONE;
			}
		}

		int32 ShortestTerminalLayer = MAX_int32;
		int32 DroneIndex = INDEX_NONE;
		while (Queue.Dequeue(DroneIndex))
		{
			if (Layer[DroneIndex] >= ShortestTerminalLayer)
			{
				continue;
			}

			for (const int32 RouteSlotIndex : Adjacency[DroneIndex])
			{
				const int32 MatchedDroneIndex = RouteToDrone[RouteSlotIndex];
				if (MatchedDroneIndex == INDEX_NONE)
				{
					ShortestTerminalLayer = Layer[DroneIndex];
				}
				else if (Layer[MatchedDroneIndex] == INDEX_NONE)
				{
					Layer[MatchedDroneIndex] = Layer[DroneIndex] + 1;
					Queue.Enqueue(MatchedDroneIndex);
				}
			}
		}

		if (ShortestTerminalLayer == MAX_int32)
		{
			break;
		}

		int32 Augmentations = 0;
		for (DroneIndex = 0; DroneIndex < CandidateGraph.EligibleDroneCount; ++DroneIndex)
		{
			if (DroneToRoute[DroneIndex] == INDEX_NONE
				&& TryAugment(
					DroneIndex,
					Adjacency,
					DroneToRoute,
					RouteToDrone,
					Layer,
					ShortestTerminalLayer))
			{
				++Augmentations;
			}
		}

		if (Augmentations == 0)
		{
			OutError = TEXT("Maximum matching solver failed to augment a discovered path.");
			OutResult = FRouteAssignmentMaximumMatchingResult{};
			return false;
		}
	}

	OutResult.WitnessPairs.Reserve(CandidateGraph.EligibleDroneCount);
	for (int32 DroneIndex = 0; DroneIndex < CandidateGraph.EligibleDroneCount; ++DroneIndex)
	{
		if (DroneToRoute[DroneIndex] == INDEX_NONE)
		{
			continue;
		}

		FRouteAssignmentMatchingPair Pair;
		Pair.DroneID = CandidateGraph.EligibleDroneIDs[DroneIndex];
		Pair.RouteSlotIndex = DroneToRoute[DroneIndex];
		OutResult.WitnessPairs.Add(MoveTemp(Pair));
	}
	OutResult.MaximumCardinality = OutResult.WitnessPairs.Num();
	return true;
}

bool RouteAssignmentMatcher::SolveMinimumTotalDistance(
	const FRouteAssignmentCandidateGraph& CandidateGraph,
	const FRouteAssignmentMaximumMatchingResult& MaximumMatchingResult,
	FRouteAssignmentMinimumCostMatchingResult& OutResult,
	FString& OutError)
{
	OutResult = FRouteAssignmentMinimumCostMatchingResult{};
	OutError.Reset();

	FString ValidationError;
	if (!ValidateCandidateGraph(CandidateGraph, ValidationError))
	{
		OutError = FString::Printf(
			TEXT("MalformedCandidateGraph: %s"),
			*ValidationError);
		return false;
	}

	if (!ValidateMaximumMatchingResult(
		CandidateGraph,
		MaximumMatchingResult,
		ValidationError))
	{
		OutError = FString::Printf(
			TEXT("CardinalityContractMismatch: %s"),
			*ValidationError);
		return false;
	}

	FRouteAssignmentMaximumMatchingResult VerifiedMaximumResult;
	FString MaximumError;
	if (!SolveMaximumCardinality(
		CandidateGraph,
		VerifiedMaximumResult,
		MaximumError))
	{
		OutError = FString::Printf(
			TEXT("InternalSolverFailure: maximum-cardinality verification failed: %s"),
			*MaximumError);
		return false;
	}

	if (VerifiedMaximumResult.MaximumCardinality
		!= MaximumMatchingResult.MaximumCardinality)
	{
		OutError = FString::Printf(
			TEXT("CardinalityContractMismatch: supplied cardinality %d, verified cardinality %d."),
			MaximumMatchingResult.MaximumCardinality,
			VerifiedMaximumResult.MaximumCardinality);
		return false;
	}

	const int32 RequiredFlow = VerifiedMaximumResult.MaximumCardinality;
	OutResult.EligibleDroneCount = CandidateGraph.EligibleDroneCount;
	OutResult.RouteSlotCount = CandidateGraph.RouteSlotCount;
	if (RequiredFlow == 0)
	{
		OutResult.MatchedCardinality = 0;
		OutResult.TotalHorizontalDistanceMeters = 0.0;
		if (!ValidateMinimumCostResult(
			CandidateGraph,
			RequiredFlow,
			OutResult,
			ValidationError))
		{
			OutError = FString::Printf(
				TEXT("InternalSolverFailure: zero-flow result validation failed: %s"),
				*ValidationError);
			OutResult = FRouteAssignmentMinimumCostMatchingResult{};
			return false;
		}
		return true;
	}

	const int32 SourceNode = 0;
	const int32 DroneNodeBegin = 1;
	const int32 RouteNodeBegin = DroneNodeBegin + CandidateGraph.EligibleDroneCount;
	const int32 SinkNode = RouteNodeBegin + CandidateGraph.RouteSlotCount;
	const int32 NodeCount = SinkNode + 1;

	FResidualNetwork Network;
	Network.SetNum(NodeCount);
	int32 NextStableOrder = 0;
	for (int32 DroneIndex = 0;
		DroneIndex < CandidateGraph.EligibleDroneCount;
		++DroneIndex)
	{
		AddResidualEdge(
			Network,
			SourceNode,
			DroneNodeBegin + DroneIndex,
			0.0,
			NextStableOrder);
	}

	for (int32 DroneIndex = 0;
		DroneIndex < CandidateGraph.EligibleDroneCount;
		++DroneIndex)
	{
		TArray<int32> CandidateEdgeIndices;
		for (int32 EdgeIndex = CandidateGraph.EdgeOffsets[DroneIndex];
			EdgeIndex < CandidateGraph.EdgeOffsets[DroneIndex + 1];
			++EdgeIndex)
		{
			CandidateEdgeIndices.Add(EdgeIndex);
		}
		CandidateEdgeIndices.Sort(
			[&CandidateGraph](const int32 LeftIndex, const int32 RightIndex)
			{
				return CandidateGraph.Edges[LeftIndex].RouteSlotIndex
					< CandidateGraph.Edges[RightIndex].RouteSlotIndex;
			});

		for (const int32 EdgeIndex : CandidateEdgeIndices)
		{
			const FRouteAssignmentCandidateEdge& CandidateEdge =
				CandidateGraph.Edges[EdgeIndex];
			AddResidualEdge(
				Network,
				DroneNodeBegin + DroneIndex,
				RouteNodeBegin + CandidateEdge.RouteSlotIndex,
				CandidateEdge.HorizontalDistanceMeters,
				NextStableOrder);
		}
	}

	for (int32 RouteSlotIndex = 0;
		RouteSlotIndex < CandidateGraph.RouteSlotCount;
		++RouteSlotIndex)
	{
		AddResidualEdge(
			Network,
			RouteNodeBegin + RouteSlotIndex,
			SinkNode,
			0.0,
			NextStableOrder);
	}

	TArray<double> Potentials;
	Potentials.Init(0.0, NodeCount);
	TArray<double> Distances;
	TArray<int32> PreviousNodes;
	TArray<int32> PreviousEdges;

	for (int32 SentFlow = 0; SentFlow < RequiredFlow; ++SentFlow)
	{
		FString ShortestPathError;
		const bool bPathFound = FindShortestResidualPath(
			Network,
			SourceNode,
			SinkNode,
			Potentials,
			Distances,
			PreviousNodes,
			PreviousEdges,
			ShortestPathError);
		if (!bPathFound)
		{
			OutError = ShortestPathError.IsEmpty()
				? FString::Printf(
					TEXT("InternalSolverFailure: only %d of %d required flow units could be sent."),
					SentFlow,
					RequiredFlow)
				: ShortestPathError;
			OutResult = FRouteAssignmentMinimumCostMatchingResult{};
			return false;
		}

		for (int32 Node = 0; Node < NodeCount; ++Node)
		{
			if (!FMath::IsFinite(Distances[Node]))
			{
				continue;
			}
			Potentials[Node] += Distances[Node];
			if (!FMath::IsFinite(Potentials[Node]))
			{
				OutError = TEXT(
					"NumericalInvariantFailure: non-finite node potential update.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}
		}

		int32 CurrentNode = SinkNode;
		int32 PathLength = 0;
		while (CurrentNode != SourceNode)
		{
			if (++PathLength > NodeCount
				|| !PreviousNodes.IsValidIndex(CurrentNode)
				|| PreviousNodes[CurrentNode] == INDEX_NONE)
			{
				OutError = TEXT("InternalSolverFailure: residual predecessor chain is invalid.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}

			const int32 PreviousNode = PreviousNodes[CurrentNode];
			const int32 EdgeIndex = PreviousEdges[CurrentNode];
			if (!Network.IsValidIndex(PreviousNode)
				|| !Network[PreviousNode].IsValidIndex(EdgeIndex))
			{
				OutError = TEXT("InternalSolverFailure: residual edge index is invalid.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}

			FResidualEdge& Edge = Network[PreviousNode][EdgeIndex];
			if (Edge.ToNode != CurrentNode
				|| Edge.ResidualCapacity != 1
				|| !Network[CurrentNode].IsValidIndex(Edge.ReverseEdgeIndex))
			{
				OutError = TEXT("InternalSolverFailure: residual forward edge invariant failed.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}

			FResidualEdge& Reverse = Network[CurrentNode][Edge.ReverseEdgeIndex];
			if (Reverse.ToNode != PreviousNode
				|| Reverse.ReverseEdgeIndex != EdgeIndex
				|| Reverse.ResidualCapacity != 0
				|| Reverse.PhysicalCost != -Edge.PhysicalCost)
			{
				OutError = TEXT("InternalSolverFailure: residual reverse edge invariant failed.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}

			--Edge.ResidualCapacity;
			++Reverse.ResidualCapacity;
			CurrentNode = PreviousNode;
		}
	}

	OutResult.Pairs.Reserve(RequiredFlow);
	for (int32 DroneIndex = 0;
		DroneIndex < CandidateGraph.EligibleDroneCount;
		++DroneIndex)
	{
		const int32 DroneNode = DroneNodeBegin + DroneIndex;
		int32 MatchedRouteSlotIndex = INDEX_NONE;
		for (const FResidualEdge& Edge : Network[DroneNode])
		{
			if (Edge.ToNode < RouteNodeBegin
				|| Edge.ToNode >= SinkNode
				|| Edge.ResidualCapacity != 0
				|| !Network[Edge.ToNode].IsValidIndex(Edge.ReverseEdgeIndex)
				|| Network[Edge.ToNode][Edge.ReverseEdgeIndex].ResidualCapacity != 1)
			{
				continue;
			}

			if (MatchedRouteSlotIndex != INDEX_NONE)
			{
				OutError = TEXT("InternalSolverFailure: a Drone has multiple flow Routes.");
				OutResult = FRouteAssignmentMinimumCostMatchingResult{};
				return false;
			}
			MatchedRouteSlotIndex = Edge.ToNode - RouteNodeBegin;
		}

		if (MatchedRouteSlotIndex == INDEX_NONE)
		{
			continue;
		}

		double CandidateDistance = 0.0;
		if (!FindCandidateDistance(
			CandidateGraph,
			CandidateGraph.EligibleDroneIDs[DroneIndex],
			MatchedRouteSlotIndex,
			CandidateDistance))
		{
			OutError = TEXT("InternalSolverFailure: flow pair is not a candidate edge.");
			OutResult = FRouteAssignmentMinimumCostMatchingResult{};
			return false;
		}

		FRouteAssignmentMinimumCostPair Pair;
		Pair.DroneID = CandidateGraph.EligibleDroneIDs[DroneIndex];
		Pair.RouteSlotIndex = MatchedRouteSlotIndex;
		Pair.HorizontalDistanceMeters = CandidateDistance;
		OutResult.Pairs.Add(MoveTemp(Pair));
	}

	OutResult.MatchedCardinality = OutResult.Pairs.Num();
	OutResult.TotalHorizontalDistanceMeters = CanonicalCompensatedSum(OutResult.Pairs);
	if (!ValidateMinimumCostResult(
		CandidateGraph,
		RequiredFlow,
		OutResult,
		ValidationError))
	{
		OutError = FString::Printf(
			TEXT("InternalSolverFailure: result validation failed: %s"),
			*ValidationError);
		OutResult = FRouteAssignmentMinimumCostMatchingResult{};
		return false;
	}

	return true;
}

#if WITH_DEV_AUTOMATION_TESTS
bool RouteAssignmentMatcher::Testing::NormalizeReducedCost(
	const double PhysicalCost,
	const double FromPotential,
	const double ToPotential,
	double& OutReducedCost,
	FString& OutError)
{
	OutError.Reset();
	return NormalizeReducedCostInternal(
		PhysicalCost,
		FromPotential,
		ToPotential,
		OutReducedCost,
		OutError);
}
#endif
