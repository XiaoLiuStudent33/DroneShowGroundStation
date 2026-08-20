#include "RouteAssignmentMaterializer.h"

#include "RouteAssignmentContract.h"

namespace
{
	int32 CompareMaterializerDroneIDs(const FString& Left, const FString& Right)
	{
		const int32 IdentityOrder = Left.Compare(Right, ESearchCase::IgnoreCase);
		return IdentityOrder != 0
			? IdentityOrder
			: Left.Compare(Right, ESearchCase::CaseSensitive);
	}

	bool MaterializerDroneIDLess(const FString& Left, const FString& Right)
	{
		return CompareMaterializerDroneIDs(Left, Right) < 0;
	}

	FString MakeDroneIdentityKey(const FString& DroneID)
	{
		return DroneID.ToLower();
	}

	bool IsMaterializerLatitudeValid(const double Latitude)
	{
		return FMath::IsFinite(Latitude) && Latitude >= -90.0 && Latitude <= 90.0;
	}

	bool IsMaterializerLongitudeValid(const double Longitude)
	{
		return FMath::IsFinite(Longitude) && Longitude >= -180.0 && Longitude <= 180.0;
	}

	bool IsRawDroneGeoEligible(const FRouteAssignmentDroneSource& Drone)
	{
		return Drone.bOnline
			&& Drone.bGPSValid
			&& Drone.GPSFixType >= 3
			&& !(Drone.Latitude == 0.0 && Drone.Longitude == 0.0)
			&& IsMaterializerLatitudeValid(Drone.Latitude)
			&& IsMaterializerLongitudeValid(Drone.Longitude);
	}

	ERouteAssignmentExcludedReason DetermineExcludedReason(
		const FRouteAssignmentDroneSource& Drone)
	{
		if (!Drone.bOnline)
		{
			return ERouteAssignmentExcludedReason::Offline;
		}
		if (!Drone.bGPSValid)
		{
			return ERouteAssignmentExcludedReason::InvalidGPS;
		}
		if (Drone.GPSFixType < 3)
		{
			return ERouteAssignmentExcludedReason::InsufficientGPSFix;
		}
		if ((Drone.Latitude == 0.0 && Drone.Longitude == 0.0)
			|| !IsMaterializerLatitudeValid(Drone.Latitude)
			|| !IsMaterializerLongitudeValid(Drone.Longitude))
		{
			return ERouteAssignmentExcludedReason::InvalidGeo;
		}

		// Raw conditions can be valid while the derived EN projection is not.
		return ERouteAssignmentExcludedReason::InvalidGeo;
	}

	bool ValidateSourceIdentity(
		const FRouteAssignmentSourceIdentity& Identity,
		FString& OutError)
	{
		if (Identity.FleetSessionID.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("SourceIdentity FleetSessionID must not be empty.");
			return false;
		}
		if (Identity.FleetRevisionAtCapture < 0)
		{
			OutError = TEXT("SourceIdentity FleetRevisionAtCapture must not be negative.");
			return false;
		}
		if (Identity.RouteDatasetGeneration <= 0)
		{
			OutError = TEXT("SourceIdentity RouteDatasetGeneration must be positive.");
			return false;
		}
		if (!Identity.bShowOriginSet
			|| !IsMaterializerLatitudeValid(Identity.Latitude)
			|| !IsMaterializerLongitudeValid(Identity.Longitude)
			|| !FMath::IsFinite(Identity.Altitude))
		{
			OutError = TEXT("SourceIdentity ShowOrigin is missing or invalid.");
			return false;
		}
		if (Identity.CapturedAtUtc.GetTicks() <= 0)
		{
			OutError = TEXT("SourceIdentity CapturedAtUtc must be set.");
			return false;
		}
		return true;
	}

	double CanonicalCompensatedPairSum(
		TArray<FRouteAssignmentMinimumCostPair> Pairs)
	{
		Pairs.Sort(
			[](const FRouteAssignmentMinimumCostPair& Left,
				const FRouteAssignmentMinimumCostPair& Right)
			{
				const int32 DroneOrder = CompareMaterializerDroneIDs(Left.DroneID, Right.DroneID);
				return DroneOrder != 0
					? DroneOrder < 0
					: Left.RouteSlotIndex < Right.RouteSlotIndex;
			});

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
}

bool RouteAssignmentMaterializer::MaterializeAssignmentCandidate(
	const FRouteAssignmentSourceSnapshot& SourceSnapshot,
	const TArray<FRouteAssignmentDroneLocalPosition>& DroneLocalPositions,
	const FRouteAssignmentCandidateGraph& CandidateGraph,
	const FRouteAssignmentMinimumCostMatchingResult& OptimalMatching,
	FRouteAssignmentCandidate& OutCandidate,
	FString& OutError)
{
	OutCandidate = FRouteAssignmentCandidate{};
	OutError.Reset();

	if (!ValidateSourceIdentity(SourceSnapshot.SourceIdentity, OutError))
	{
		return false;
	}
	if (DroneLocalPositions.Num() != SourceSnapshot.Drones.Num())
	{
		OutError = TEXT("DroneLocalPositions count does not match SourceSnapshot Drones.");
		return false;
	}

	TSet<FString> SourceDroneKeys;
	TSet<FString> EligibleDroneKeys;
	TMap<FString, FString> AuthoritativeDroneIDs;
	int32 EligibleDroneCount = 0;
	for (int32 DroneIndex = 0; DroneIndex < SourceSnapshot.Drones.Num(); ++DroneIndex)
	{
		const FRouteAssignmentDroneSource& SourceDrone = SourceSnapshot.Drones[DroneIndex];
		const FRouteAssignmentDroneLocalPosition& LocalPosition =
			DroneLocalPositions[DroneIndex];
		if (SourceDrone.DroneID.IsEmpty() || LocalPosition.DroneID.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Drone index %d has an empty DroneID."), DroneIndex);
			return false;
		}
		if (!SourceDrone.DroneID.Equals(LocalPosition.DroneID, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Local/Source DroneID mismatch at index %d."),
				DroneIndex);
			return false;
		}

		const FString IdentityKey = MakeDroneIdentityKey(SourceDrone.DroneID);
		if (SourceDroneKeys.Contains(IdentityKey))
		{
			OutError = FString::Printf(
				TEXT("SourceSnapshot contains duplicate DroneID '%s'."),
				*SourceDrone.DroneID);
			return false;
		}
		SourceDroneKeys.Add(IdentityKey);
		AuthoritativeDroneIDs.Add(IdentityKey, SourceDrone.DroneID);

		if (LocalPosition.bAssignmentGeoEligible)
		{
			if (!IsRawDroneGeoEligible(SourceDrone)
				|| !FMath::IsFinite(LocalPosition.EastMeters)
				|| !FMath::IsFinite(LocalPosition.NorthMeters))
			{
				OutError = FString::Printf(
					TEXT("Eligible DroneID '%s' has inconsistent source or Local EN data."),
					*SourceDrone.DroneID);
				return false;
			}
			EligibleDroneKeys.Add(IdentityKey);
			++EligibleDroneCount;
		}
	}

	const int32 SourceRouteCount = SourceSnapshot.RouteSlots.Num();
	TSet<int32> SourceRouteSlots;
	for (const FRouteAssignmentRouteSlotSource& RouteSlot : SourceSnapshot.RouteSlots)
	{
		if (RouteSlot.RouteSlotIndex < 0 || RouteSlot.RouteSlotIndex >= SourceRouteCount
			|| SourceRouteSlots.Contains(RouteSlot.RouteSlotIndex))
		{
			OutError = TEXT("SourceSnapshot RouteSlotIndex domain is invalid or duplicated.");
			return false;
		}
		if (!FMath::IsFinite(RouteSlot.EastMeters)
			|| !FMath::IsFinite(RouteSlot.NorthMeters)
			|| !FMath::IsFinite(RouteSlot.UpMeters))
		{
			OutError = TEXT("SourceSnapshot contains non-finite Route ENU data.");
			return false;
		}
		SourceRouteSlots.Add(RouteSlot.RouteSlotIndex);
	}

	if (CandidateGraph.EligibleDroneCount != EligibleDroneCount
		|| CandidateGraph.EligibleDroneCount != CandidateGraph.EligibleDroneIDs.Num())
	{
		OutError = TEXT("CandidateGraph eligible count does not match LocalPositions.");
		return false;
	}
	if (CandidateGraph.RouteSlotCount != SourceRouteCount)
	{
		OutError = TEXT("CandidateGraph RouteSlotCount does not match SourceSnapshot.");
		return false;
	}
	if (CandidateGraph.CandidateEdgeCount < 0
		|| CandidateGraph.CandidateEdgeCount != CandidateGraph.Edges.Num()
		|| CandidateGraph.EdgeOffsets.Num() != CandidateGraph.EligibleDroneCount + 1
		|| CandidateGraph.EdgeOffsets[0] != 0
		|| CandidateGraph.EdgeOffsets.Last() != CandidateGraph.CandidateEdgeCount)
	{
		OutError = TEXT("CandidateGraph counts or EdgeOffsets are malformed.");
		return false;
	}

	TSet<FString> GraphEligibleKeys;
	TMap<FString, int32> GraphDroneIndices;
	for (int32 GraphDroneIndex = 0;
		GraphDroneIndex < CandidateGraph.EligibleDroneCount;
		++GraphDroneIndex)
	{
		const FString& GraphDroneID = CandidateGraph.EligibleDroneIDs[GraphDroneIndex];
		const FString IdentityKey = MakeDroneIdentityKey(GraphDroneID);
		const FString* AuthoritativeDroneID = AuthoritativeDroneIDs.Find(IdentityKey);
		if (GraphDroneID.IsEmpty()
			|| !EligibleDroneKeys.Contains(IdentityKey)
			|| GraphEligibleKeys.Contains(IdentityKey)
			|| !AuthoritativeDroneID
			|| *AuthoritativeDroneID != GraphDroneID)
		{
			OutError = TEXT("CandidateGraph eligible Drone identity coverage is invalid.");
			return false;
		}
		if (GraphDroneIndex > 0
			&& CompareMaterializerDroneIDs(
				CandidateGraph.EligibleDroneIDs[GraphDroneIndex - 1],
				GraphDroneID) >= 0)
		{
			OutError = TEXT("CandidateGraph EligibleDroneIDs are not strictly canonical.");
			return false;
		}

		GraphEligibleKeys.Add(IdentityKey);
		GraphDroneIndices.Add(IdentityKey, GraphDroneIndex);
		const int32 EdgeBegin = CandidateGraph.EdgeOffsets[GraphDroneIndex];
		const int32 EdgeEnd = CandidateGraph.EdgeOffsets[GraphDroneIndex + 1];
		if (EdgeBegin < 0
			|| EdgeEnd < EdgeBegin
			|| EdgeEnd > CandidateGraph.CandidateEdgeCount)
		{
			OutError = TEXT("CandidateGraph contains an invalid adjacency range.");
			return false;
		}

		TSet<int32> DroneRouteSlots;
		for (int32 EdgeIndex = EdgeBegin; EdgeIndex < EdgeEnd; ++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& Edge = CandidateGraph.Edges[EdgeIndex];
			if (Edge.DroneID != GraphDroneID
				|| !SourceRouteSlots.Contains(Edge.RouteSlotIndex)
				|| DroneRouteSlots.Contains(Edge.RouteSlotIndex)
				|| !FMath::IsFinite(Edge.HorizontalDistanceMeters)
				|| Edge.HorizontalDistanceMeters < 0.0
				|| Edge.HorizontalDistanceMeters
					> RouteAssignmentContract::MaxHorizontalMatchDistanceMeters)
			{
				OutError = TEXT("CandidateGraph contains a malformed candidate edge.");
				return false;
			}
			DroneRouteSlots.Add(Edge.RouteSlotIndex);
		}
	}
	if (GraphEligibleKeys.Num() != EligibleDroneKeys.Num())
	{
		OutError = TEXT("CandidateGraph eligible identity set is incomplete.");
		return false;
	}

	if (OptimalMatching.EligibleDroneCount != CandidateGraph.EligibleDroneCount
		|| OptimalMatching.RouteSlotCount != CandidateGraph.RouteSlotCount
		|| OptimalMatching.MatchedCardinality < 0
		|| OptimalMatching.MatchedCardinality
			> FMath::Min(CandidateGraph.EligibleDroneCount, CandidateGraph.RouteSlotCount)
		|| OptimalMatching.MatchedCardinality != OptimalMatching.Pairs.Num()
		|| !FMath::IsFinite(OptimalMatching.TotalHorizontalDistanceMeters)
		|| OptimalMatching.TotalHorizontalDistanceMeters < 0.0)
	{
		OutError = TEXT("OptimalMatching counts or total are malformed.");
		return false;
	}

	TSet<FString> MatchedDroneKeys;
	TSet<int32> MatchedRouteSlots;
	FRouteAssignmentCandidate LocalCandidate;
	LocalCandidate.BindingProvenance =
		ERouteAssignmentBindingProvenance::MatchedByPosition;
	LocalCandidate.SourceIdentity = SourceSnapshot.SourceIdentity;
	LocalCandidate.CandidateDroneCount = SourceSnapshot.Drones.Num();
	LocalCandidate.RouteSlotCount = SourceRouteCount;
	LocalCandidate.MatchedEntries.Reserve(OptimalMatching.Pairs.Num());
	for (const FRouteAssignmentMinimumCostPair& Pair : OptimalMatching.Pairs)
	{
		const FString IdentityKey = MakeDroneIdentityKey(Pair.DroneID);
		const int32* GraphDroneIndex = GraphDroneIndices.Find(IdentityKey);
		if (!GraphDroneIndex
			|| CandidateGraph.EligibleDroneIDs[*GraphDroneIndex] != Pair.DroneID
			|| MatchedDroneKeys.Contains(IdentityKey)
			|| MatchedRouteSlots.Contains(Pair.RouteSlotIndex))
		{
			OutError = TEXT("OptimalMatching contains a duplicate or unknown Drone/Route pair.");
			return false;
		}

		const FRouteAssignmentCandidateEdge* MatchingEdge = nullptr;
		for (int32 EdgeIndex = CandidateGraph.EdgeOffsets[*GraphDroneIndex];
			EdgeIndex < CandidateGraph.EdgeOffsets[*GraphDroneIndex + 1];
			++EdgeIndex)
		{
			const FRouteAssignmentCandidateEdge& Edge = CandidateGraph.Edges[EdgeIndex];
			if (Edge.RouteSlotIndex == Pair.RouteSlotIndex)
			{
				MatchingEdge = &Edge;
				break;
			}
		}
		if (!MatchingEdge
			|| Pair.HorizontalDistanceMeters != MatchingEdge->HorizontalDistanceMeters
			|| !FMath::IsFinite(Pair.HorizontalDistanceMeters)
			|| Pair.HorizontalDistanceMeters < 0.0
			|| Pair.HorizontalDistanceMeters
				> RouteAssignmentContract::MaxHorizontalMatchDistanceMeters)
		{
			OutError = TEXT("OptimalMatching pair is not an exact production candidate edge.");
			return false;
		}

		MatchedDroneKeys.Add(IdentityKey);
		MatchedRouteSlots.Add(Pair.RouteSlotIndex);
		FRouteAssignmentEntry Entry;
		Entry.DroneID = Pair.DroneID;
		Entry.RouteSlotIndex = Pair.RouteSlotIndex;
		Entry.HorizontalDistanceMeters = Pair.HorizontalDistanceMeters;
		Entry.bHorizontalDistanceValid = true;
		LocalCandidate.MatchedEntries.Add(MoveTemp(Entry));
	}

	if (OptimalMatching.TotalHorizontalDistanceMeters
		!= CanonicalCompensatedPairSum(OptimalMatching.Pairs))
	{
		OutError = TEXT("OptimalMatching total does not match its canonical pair sum.");
		return false;
	}

	for (int32 DroneIndex = 0; DroneIndex < SourceSnapshot.Drones.Num(); ++DroneIndex)
	{
		const FRouteAssignmentDroneSource& SourceDrone = SourceSnapshot.Drones[DroneIndex];
		const FRouteAssignmentDroneLocalPosition& LocalPosition =
			DroneLocalPositions[DroneIndex];
		const FString IdentityKey = MakeDroneIdentityKey(SourceDrone.DroneID);
		if (!LocalPosition.bAssignmentGeoEligible)
		{
			FRouteAssignmentExcludedDrone Excluded;
			Excluded.DroneID = SourceDrone.DroneID;
			Excluded.Reason = DetermineExcludedReason(SourceDrone);
			LocalCandidate.ExcludedDrones.Add(MoveTemp(Excluded));
			continue;
		}
		if (MatchedDroneKeys.Contains(IdentityKey))
		{
			continue;
		}

		if (OptimalMatching.MatchedCardinality == SourceRouteCount)
		{
			FRouteAssignmentSpareDrone Spare;
			Spare.DroneID = SourceDrone.DroneID;
			LocalCandidate.SpareDrones.Add(MoveTemp(Spare));
			continue;
		}

		const int32* GraphDroneIndex = GraphDroneIndices.Find(IdentityKey);
		if (!GraphDroneIndex)
		{
			OutError = TEXT("Eligible unmatched Drone is absent from CandidateGraph.");
			return false;
		}
		FRouteAssignmentUnresolvedDrone Unresolved;
		Unresolved.DroneID = SourceDrone.DroneID;
		Unresolved.Reason =
			CandidateGraph.EdgeOffsets[*GraphDroneIndex]
				== CandidateGraph.EdgeOffsets[*GraphDroneIndex + 1]
			? ERouteAssignmentUnresolvedReason::NoCandidate
			: ERouteAssignmentUnresolvedReason::Conflict;
		LocalCandidate.UnresolvedDrones.Add(MoveTemp(Unresolved));
	}

	for (const FRouteAssignmentRouteSlotSource& RouteSlot : SourceSnapshot.RouteSlots)
	{
		if (!MatchedRouteSlots.Contains(RouteSlot.RouteSlotIndex))
		{
			LocalCandidate.VacantRouteSlots.Add(RouteSlot.RouteSlotIndex);
		}
	}

	if (!LocalCandidate.VacantRouteSlots.IsEmpty()
		&& !LocalCandidate.SpareDrones.IsEmpty())
	{
		OutError = TEXT("Materialized partition cannot contain both Vacant and Spare entries.");
		return false;
	}
	const int64 CoveredDroneCount =
		static_cast<int64>(LocalCandidate.MatchedEntries.Num())
		+ static_cast<int64>(LocalCandidate.SpareDrones.Num())
		+ static_cast<int64>(LocalCandidate.ExcludedDrones.Num())
		+ static_cast<int64>(LocalCandidate.UnresolvedDrones.Num());
	if (CoveredDroneCount != LocalCandidate.CandidateDroneCount
		|| LocalCandidate.MatchedEntries.Num() + LocalCandidate.VacantRouteSlots.Num()
			!= LocalCandidate.RouteSlotCount)
	{
		OutError = TEXT("Materialized Drone or Route partition is incomplete.");
		return false;
	}

	LocalCandidate.MatchedEntries.Sort(
		[](const FRouteAssignmentEntry& Left, const FRouteAssignmentEntry& Right)
		{
			return Left.RouteSlotIndex < Right.RouteSlotIndex;
		});
	LocalCandidate.SpareDrones.Sort(
		[](const FRouteAssignmentSpareDrone& Left, const FRouteAssignmentSpareDrone& Right)
		{
			return MaterializerDroneIDLess(Left.DroneID, Right.DroneID);
		});
	LocalCandidate.ExcludedDrones.Sort(
		[](const FRouteAssignmentExcludedDrone& Left,
			const FRouteAssignmentExcludedDrone& Right)
		{
			return MaterializerDroneIDLess(Left.DroneID, Right.DroneID);
		});
	LocalCandidate.UnresolvedDrones.Sort(
		[](const FRouteAssignmentUnresolvedDrone& Left,
			const FRouteAssignmentUnresolvedDrone& Right)
		{
			return MaterializerDroneIDLess(Left.DroneID, Right.DroneID);
		});
	LocalCandidate.VacantRouteSlots.Sort();
	LocalCandidate.Conflicts.Reset();

	OutCandidate = MoveTemp(LocalCandidate);
	return true;
}
