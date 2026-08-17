#include "RouteAssignmentSubsystem.h"

#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "HAL/Platform.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "RouteAssignmentContract.h"
#include "RouteAssignmentPipeline.h"
#include "RoutePreviewSubsystem.h"

namespace RouteAssignmentPrivate
{
int32 CompareDroneIDs(const FString& Left, const FString& Right)
{
	const int32 IdentityOrder = Left.Compare(Right, ESearchCase::IgnoreCase);
	if (IdentityOrder != 0)
	{
		return IdentityOrder;
	}

	return Left.Compare(Right, ESearchCase::CaseSensitive);
}

bool DroneIDLess(const FString& Left, const FString& Right)
{
	return CompareDroneIDs(Left, Right) < 0;
}

FString MakeDroneIdentityKey(const FString& DroneID)
{
	return DroneID.ToLower();
}

int32 GetLowestRouteSlotIndex(const FRouteAssignmentConflict& Conflict)
{
	return Conflict.RouteSlotIndices.IsEmpty() ? TNumericLimits<int32>::Max() : Conflict.RouteSlotIndices[0];
}

const FString& GetLowestDroneID(const FRouteAssignmentConflict& Conflict)
{
	static const FString EmptyDroneID;
	return Conflict.DroneIDs.IsEmpty() ? EmptyDroneID : Conflict.DroneIDs[0];
}

int32 CompareIntArrays(const TArray<int32>& Left, const TArray<int32>& Right)
{
	const int32 CommonCount = FMath::Min(Left.Num(), Right.Num());
	for (int32 Index = 0; Index < CommonCount; ++Index)
	{
		if (Left[Index] != Right[Index])
		{
			return Left[Index] < Right[Index] ? -1 : 1;
		}
	}

	return Left.Num() == Right.Num() ? 0 : (Left.Num() < Right.Num() ? -1 : 1);
}

int32 CompareStringArrays(const TArray<FString>& Left, const TArray<FString>& Right)
{
	const int32 CommonCount = FMath::Min(Left.Num(), Right.Num());
	for (int32 Index = 0; Index < CommonCount; ++Index)
	{
		const int32 Comparison = CompareDroneIDs(Left[Index], Right[Index]);
		if (Comparison != 0)
		{
			return Comparison;
		}
	}

	return Left.Num() == Right.Num() ? 0 : (Left.Num() < Right.Num() ? -1 : 1);
}

bool ConflictLess(const FRouteAssignmentConflict& Left, const FRouteAssignmentConflict& Right)
{
	const int32 LeftLowestSlot = GetLowestRouteSlotIndex(Left);
	const int32 RightLowestSlot = GetLowestRouteSlotIndex(Right);
	if (LeftLowestSlot != RightLowestSlot)
	{
		return LeftLowestSlot < RightLowestSlot;
	}

	const int32 DroneComparison = CompareDroneIDs(GetLowestDroneID(Left), GetLowestDroneID(Right));
	if (DroneComparison != 0)
	{
		return DroneComparison < 0;
	}

	if (Left.ConflictType != Right.ConflictType)
	{
		return static_cast<uint8>(Left.ConflictType) < static_cast<uint8>(Right.ConflictType);
	}

	const int32 SlotArrayComparison = CompareIntArrays(Left.RouteSlotIndices, Right.RouteSlotIndices);
	if (SlotArrayComparison != 0)
	{
		return SlotArrayComparison < 0;
	}

	return CompareStringArrays(Left.DroneIDs, Right.DroneIDs) < 0;
}
}

void URouteAssignmentSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	AssignmentAuthoritySessionID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	check(!AssignmentAuthoritySessionID.IsEmpty());
}

void URouteAssignmentSubsystem::Deinitialize()
{
	State = ERouteAssignmentState::Empty;
	CurrentAssignmentRevision = 0;
	LastIssuedAssignmentRevision = 0;
	WorkingSnapshot = FRouteAssignmentSnapshot{};
	FrozenSnapshot = FRouteAssignmentSnapshot{};
	AssignmentAuthoritySessionID.Reset();
	Super::Deinitialize();
}

bool URouteAssignmentSubsystem::CaptureSourceSnapshot(
	FRouteAssignmentSourceSnapshot& OutSnapshot,
	FString& OutError) const
{
	check(IsInGameThread());
	OutSnapshot = FRouteAssignmentSourceSnapshot{};
	OutError.Reset();

	const UGameInstance* GameInstance = GetGameInstance();
	if (!GameInstance)
	{
		OutError = TEXT("GameInstance is unavailable.");
		return false;
	}

	const UFleetManagerSubsystem* FleetManager =
		GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	if (!FleetManager)
	{
		OutError = TEXT("FleetManagerSubsystem is unavailable.");
		return false;
	}

	FRouteAssignmentSourceSnapshot LocalSnapshot;
	LocalSnapshot.SourceIdentity.FleetSessionID = FleetManager->GetActiveFleetSessionID();
	if (LocalSnapshot.SourceIdentity.FleetSessionID.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("The active FleetSessionID must contain at least one non-whitespace character.");
		return false;
	}

	LocalSnapshot.SourceIdentity.FleetRevisionAtCapture = FleetManager->GetFleetRevision();
	if (LocalSnapshot.SourceIdentity.FleetRevisionAtCapture < 0)
	{
		OutError = TEXT("FleetRevisionAtCapture must not be negative.");
		return false;
	}

	const int32 DroneCount = FleetManager->GetDroneCount();
	const TArray<FDroneData> FleetDrones = FleetManager->GetDronePage(0, DroneCount);
	if (FleetDrones.Num() != DroneCount)
	{
		OutError = FString::Printf(
			TEXT("Fleet capture count mismatch: expected %d drones but copied %d."),
			DroneCount,
			FleetDrones.Num());
		return false;
	}

	LocalSnapshot.Drones.Reserve(FleetDrones.Num());
	TSet<FString> CapturedDroneIDs;
	for (const FDroneData& FleetDrone : FleetDrones)
	{
		if (FleetDrone.DroneID.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("Fleet capture contains a DroneID with no non-whitespace characters.");
			return false;
		}

		if (CapturedDroneIDs.Contains(FleetDrone.DroneID))
		{
			OutError = FString::Printf(
				TEXT("Fleet capture contains duplicate DroneID '%s'."),
				*FleetDrone.DroneID);
			return false;
		}

		CapturedDroneIDs.Add(FleetDrone.DroneID);

		FRouteAssignmentDroneSource DroneSource;
		DroneSource.DroneID = FleetDrone.DroneID;
		DroneSource.Latitude = FleetDrone.GPSLat;
		DroneSource.Longitude = FleetDrone.GPSLon;
		DroneSource.AltitudeAMSL = FleetDrone.AltitudeAMSL;
		DroneSource.bGPSValid = FleetDrone.bGPSValid;
		DroneSource.GPSFixType = FleetDrone.GPSFixType;
		DroneSource.bAltitudeValid = FleetDrone.bAltitudeValid;
		DroneSource.bOnline = FleetDrone.bOnline;
		LocalSnapshot.Drones.Add(MoveTemp(DroneSource));
	}

	LocalSnapshot.Drones.Sort(
		[](const FRouteAssignmentDroneSource& Left, const FRouteAssignmentDroneSource& Right)
		{
			return RouteAssignmentPrivate::DroneIDLess(Left.DroneID, Right.DroneID);
		});

	const URoutePreviewSubsystem* RoutePreview =
		GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	if (!RoutePreview)
	{
		OutError = TEXT("RoutePreviewSubsystem is unavailable.");
		return false;
	}

	if (!RoutePreview->IsRouteLoaded())
	{
		OutError = TEXT("No active route dataset is loaded.");
		return false;
	}

	LocalSnapshot.SourceIdentity.RouteDatasetGeneration =
		RoutePreview->GetRouteDatasetGeneration();
	if (LocalSnapshot.SourceIdentity.RouteDatasetGeneration <= 0)
	{
		OutError = TEXT("RouteDatasetGeneration must be greater than zero.");
		return false;
	}

	const FRoutePreviewData& RouteData = RoutePreview->GetRouteData();
	if (!RouteData.IsLoaded() || RouteData.DroneCount <= 0 || RouteData.FrameCount <= 0)
	{
		OutError = TEXT("The active route data is not structurally loaded.");
		return false;
	}

	if (!RouteData.bShowOriginSet)
	{
		OutError = TEXT("The active route ShowOrigin is not set.");
		return false;
	}

	if (!FMath::IsFinite(RouteData.ShowOriginLatitude)
		|| !FMath::IsFinite(RouteData.ShowOriginLongitude)
		|| !FMath::IsFinite(RouteData.ShowOriginAltitude))
	{
		OutError = TEXT("The active route ShowOrigin must contain finite values.");
		return false;
	}

	if (RouteData.ShowOriginLatitude < -90.0 || RouteData.ShowOriginLatitude > 90.0)
	{
		OutError = TEXT("The active route ShowOrigin latitude must be in [-90, 90].");
		return false;
	}

	if (RouteData.ShowOriginLongitude < -180.0 || RouteData.ShowOriginLongitude > 180.0)
	{
		OutError = TEXT("The active route ShowOrigin longitude must be in [-180, 180].");
		return false;
	}

	LocalSnapshot.SourceIdentity.bShowOriginSet = RouteData.bShowOriginSet;
	LocalSnapshot.SourceIdentity.Latitude = RouteData.ShowOriginLatitude;
	LocalSnapshot.SourceIdentity.Longitude = RouteData.ShowOriginLongitude;
	LocalSnapshot.SourceIdentity.Altitude = RouteData.ShowOriginAltitude;
	LocalSnapshot.SourceIdentity.CapturedAtUtc = FDateTime::UtcNow();

	LocalSnapshot.RouteSlots.Reserve(RouteData.DroneCount);
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < RouteData.DroneCount; ++RouteSlotIndex)
	{
		const FVector3f PositionENU = RouteData.GetPosition(0, RouteSlotIndex);
		if (!FMath::IsFinite(PositionENU.X)
			|| !FMath::IsFinite(PositionENU.Y)
			|| !FMath::IsFinite(PositionENU.Z))
		{
			OutError = FString::Printf(
				TEXT("Frame-0 RouteSlotIndex %d contains a non-finite Local ENU component."),
				RouteSlotIndex);
			return false;
		}

		FRouteAssignmentRouteSlotSource RouteSlotSource;
		RouteSlotSource.RouteSlotIndex = RouteSlotIndex;
		RouteSlotSource.EastMeters = static_cast<double>(PositionENU.X);
		RouteSlotSource.NorthMeters = static_cast<double>(PositionENU.Y);
		RouteSlotSource.UpMeters = static_cast<double>(PositionENU.Z);
		LocalSnapshot.RouteSlots.Add(RouteSlotSource);
	}

	OutSnapshot = MoveTemp(LocalSnapshot);
	return true;
}

bool URouteAssignmentSubsystem::ReplaceWorkingAssignment(
	const FRouteAssignmentCandidate& Candidate,
	FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();

	if (State == ERouteAssignmentState::Frozen)
	{
		OutError = TEXT("The route assignment is frozen. Reset is required before replacement.");
		UE_LOG(LogTemp, Warning, TEXT("[RouteAssignment] Replace rejected: %s"), *OutError);
		return false;
	}

	if (!ValidateCandidate(Candidate, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RouteAssignment] Replace rejected: %s"), *OutError);
		return false;
	}

	if (LastIssuedAssignmentRevision == TNumericLimits<int64>::Max())
	{
		OutError = TEXT("Assignment revision space is exhausted for this GameInstance.");
		UE_LOG(LogTemp, Error, TEXT("[RouteAssignment] Replace rejected: %s"), *OutError);
		return false;
	}

	FRouteAssignmentCandidate CanonicalCandidate = Candidate;
	CanonicalizeCandidate(CanonicalCandidate);

	++LastIssuedAssignmentRevision;
	CurrentAssignmentRevision = LastIssuedAssignmentRevision;

	FRouteAssignmentSnapshot NewWorkingSnapshot;
	NewWorkingSnapshot.AssignmentAuthoritySessionID = AssignmentAuthoritySessionID;
	NewWorkingSnapshot.AssignmentRevision = CurrentAssignmentRevision;
	NewWorkingSnapshot.SourceIdentity = CanonicalCandidate.SourceIdentity;
	NewWorkingSnapshot.CandidateDroneCount = CanonicalCandidate.CandidateDroneCount;
	NewWorkingSnapshot.RouteSlotCount = CanonicalCandidate.RouteSlotCount;
	NewWorkingSnapshot.MatchedEntries = MoveTemp(CanonicalCandidate.MatchedEntries);
	NewWorkingSnapshot.SpareDrones = MoveTemp(CanonicalCandidate.SpareDrones);
	NewWorkingSnapshot.ExcludedDrones = MoveTemp(CanonicalCandidate.ExcludedDrones);
	NewWorkingSnapshot.UnresolvedDrones = MoveTemp(CanonicalCandidate.UnresolvedDrones);
	NewWorkingSnapshot.VacantRouteSlots = MoveTemp(CanonicalCandidate.VacantRouteSlots);
	NewWorkingSnapshot.Conflicts = MoveTemp(CanonicalCandidate.Conflicts);
	NewWorkingSnapshot.Summary = BuildSummary(Candidate);
	NewWorkingSnapshot.ReadinessKind = DetermineReadinessKind(Candidate);

	WorkingSnapshot = MoveTemp(NewWorkingSnapshot);
	FrozenSnapshot = FRouteAssignmentSnapshot{};
	State = WorkingSnapshot.ReadinessKind == ERouteAssignmentReadinessKind::NotReady
		? ERouteAssignmentState::Draft
		: ERouteAssignmentState::Ready;

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteAssignment] Replace accepted Revision=%lld State=%s Readiness=%s Matched=%d Spare=%d Excluded=%d Unresolved=%d Vacant=%d Conflicts=%d"),
		CurrentAssignmentRevision,
		State == ERouteAssignmentState::Ready ? TEXT("Ready") : TEXT("Draft"),
		WorkingSnapshot.ReadinessKind == ERouteAssignmentReadinessKind::FullReady
			? TEXT("FullReady")
			: (WorkingSnapshot.ReadinessKind == ERouteAssignmentReadinessKind::DegradedReady
				? TEXT("DegradedReady")
				: TEXT("NotReady")),
		WorkingSnapshot.Summary.MatchedCount,
		WorkingSnapshot.Summary.SpareCount,
		WorkingSnapshot.Summary.ExcludedCount,
		WorkingSnapshot.Summary.UnresolvedCount,
		WorkingSnapshot.Summary.VacantRouteSlotCount,
		WorkingSnapshot.Summary.ConflictCount);
	return true;
}

bool URouteAssignmentSubsystem::BuildAndReplaceWorkingAssignment(FString& OutError)
{
	check(IsInGameThread());
	OutError.Reset();

	if (State == ERouteAssignmentState::Frozen)
	{
		OutError = TEXT("The route assignment is frozen. Reset is required before rebuilding.");
		return false;
	}

	FRouteAssignmentSourceSnapshot SourceSnapshot;
	FString StageError;
	if (!CaptureSourceSnapshot(SourceSnapshot, StageError))
	{
		OutError = FString::Printf(TEXT("Capture: %s"), *StageError);
		return false;
	}

	FRouteAssignmentCandidate Candidate;
	if (!RouteAssignmentPipeline::BuildCandidateFromSourceSnapshot(
		SourceSnapshot, Candidate, StageError))
	{
		OutError = StageError;
		return false;
	}

	if (!ReplaceWorkingAssignment(Candidate, StageError))
	{
		OutError = FString::Printf(TEXT("Authority: %s"), *StageError);
		return false;
	}

	return true;
}

bool URouteAssignmentSubsystem::CanFreeze(
	const ERouteAssignmentFreezePolicy FreezePolicy,
	FString& OutError) const
{
	OutError.Reset();
	if (State != ERouteAssignmentState::Ready)
	{
		OutError = TEXT("Only a Ready working assignment can be frozen.");
		return false;
	}

	return ValidateWorkingSnapshotForFreeze(FreezePolicy, OutError);
}

bool URouteAssignmentSubsystem::Freeze(
	const ERouteAssignmentFreezePolicy FreezePolicy,
	FString& OutError)
{
	check(IsInGameThread());
	if (!CanFreeze(FreezePolicy, OutError))
	{
		UE_LOG(LogTemp, Warning, TEXT("[RouteAssignment] Freeze rejected: %s"), *OutError);
		return false;
	}

	FrozenSnapshot = WorkingSnapshot;
	FrozenSnapshot.CompletionMode =
		WorkingSnapshot.ReadinessKind == ERouteAssignmentReadinessKind::FullReady
			? ERouteAssignmentCompletionMode::Full
			: ERouteAssignmentCompletionMode::Degraded;
	FrozenSnapshot.FreezePolicy = FreezePolicy;
	FrozenSnapshot.FrozenAtUtc = FDateTime::UtcNow();
	State = ERouteAssignmentState::Frozen;

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteAssignment] Freeze accepted Revision=%lld Completion=%s Policy=%s FrozenAtUtc=%s"),
		CurrentAssignmentRevision,
		FrozenSnapshot.CompletionMode == ERouteAssignmentCompletionMode::Full
			? TEXT("Full")
			: TEXT("Degraded"),
		FreezePolicy == ERouteAssignmentFreezePolicy::RequireFull
			? TEXT("RequireFull")
			: TEXT("AllowDegraded"),
		*FrozenSnapshot.FrozenAtUtc.ToIso8601());
	return true;
}

bool URouteAssignmentSubsystem::GetWorkingSnapshot(FRouteAssignmentSnapshot& OutSnapshot) const
{
	if (State == ERouteAssignmentState::Empty)
	{
		OutSnapshot = FRouteAssignmentSnapshot{};
		return false;
	}

	OutSnapshot = WorkingSnapshot;
	return true;
}

bool URouteAssignmentSubsystem::GetFrozenSnapshot(FRouteAssignmentSnapshot& OutSnapshot) const
{
	if (State != ERouteAssignmentState::Frozen)
	{
		OutSnapshot = FRouteAssignmentSnapshot{};
		return false;
	}

	OutSnapshot = FrozenSnapshot;
	return true;
}

void URouteAssignmentSubsystem::Reset()
{
	check(IsInGameThread());
	const int64 PreviousRevision = CurrentAssignmentRevision;
	State = ERouteAssignmentState::Empty;
	CurrentAssignmentRevision = 0;
	WorkingSnapshot = FRouteAssignmentSnapshot{};
	FrozenSnapshot = FRouteAssignmentSnapshot{};

	UE_LOG(
		LogTemp,
		Display,
		TEXT("[RouteAssignment] Reset PreviousRevision=%lld LastIssuedRevision=%lld"),
		PreviousRevision,
		LastIssuedAssignmentRevision);
}

bool URouteAssignmentSubsystem::ValidateCandidate(
	const FRouteAssignmentCandidate& Candidate,
	FString& OutError)
{
	OutError.Reset();
	if (Candidate.CandidateDroneCount < 0)
	{
		OutError = TEXT("CandidateDroneCount must not be negative.");
		return false;
	}

	if (Candidate.RouteSlotCount < 0)
	{
		OutError = TEXT("RouteSlotCount must not be negative.");
		return false;
	}

	if (Candidate.SourceIdentity.FleetSessionID.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("FleetSessionID must contain at least one non-whitespace character.");
		return false;
	}

	if (Candidate.SourceIdentity.FleetRevisionAtCapture < 0)
	{
		OutError = TEXT("FleetRevisionAtCapture must not be negative.");
		return false;
	}

	if (Candidate.SourceIdentity.RouteDatasetGeneration <= 0)
	{
		OutError = TEXT("RouteDatasetGeneration must be greater than zero.");
		return false;
	}

	if (!Candidate.SourceIdentity.bShowOriginSet)
	{
		OutError = TEXT("ShowOrigin must be set.");
		return false;
	}

	if (!FMath::IsFinite(Candidate.SourceIdentity.Latitude)
		|| !FMath::IsFinite(Candidate.SourceIdentity.Longitude)
		|| !FMath::IsFinite(Candidate.SourceIdentity.Altitude))
	{
		OutError = TEXT("ShowOrigin latitude, longitude, and altitude must be finite.");
		return false;
	}

	if (Candidate.SourceIdentity.Latitude < -90.0 || Candidate.SourceIdentity.Latitude > 90.0)
	{
		OutError = TEXT("ShowOrigin latitude must be in [-90, 90].");
		return false;
	}

	if (Candidate.SourceIdentity.Longitude < -180.0 || Candidate.SourceIdentity.Longitude > 180.0)
	{
		OutError = TEXT("ShowOrigin longitude must be in [-180, 180].");
		return false;
	}

	if (Candidate.SourceIdentity.CapturedAtUtc.GetTicks() <= 0)
	{
		OutError = TEXT("CapturedAtUtc must be set.");
		return false;
	}

	TSet<FString> CandidateDroneIdentityKeys;
	TSet<int32> MatchedRouteSlots;
	for (const FRouteAssignmentEntry& Entry : Candidate.MatchedEntries)
	{
		if (Entry.DroneID.IsEmpty())
		{
			OutError = TEXT("MatchedEntries contains an empty DroneID.");
			return false;
		}

		const FString DroneIdentityKey = RouteAssignmentPrivate::MakeDroneIdentityKey(Entry.DroneID);
		if (CandidateDroneIdentityKeys.Contains(DroneIdentityKey))
		{
			OutError = FString::Printf(TEXT("MatchedEntries contains duplicate DroneID '%s'."), *Entry.DroneID);
			return false;
		}

		if (Entry.RouteSlotIndex < 0 || Entry.RouteSlotIndex >= Candidate.RouteSlotCount)
		{
			OutError = FString::Printf(
				TEXT("Matched RouteSlotIndex %d is outside [0, %d)."),
				Entry.RouteSlotIndex,
				Candidate.RouteSlotCount);
			return false;
		}

		if (MatchedRouteSlots.Contains(Entry.RouteSlotIndex))
		{
			OutError = FString::Printf(
				TEXT("MatchedEntries contains duplicate RouteSlotIndex %d."),
				Entry.RouteSlotIndex);
			return false;
		}

		if (!Entry.bHorizontalDistanceValid
			|| !FMath::IsFinite(Entry.HorizontalDistanceMeters)
			|| Entry.HorizontalDistanceMeters < 0.0
			|| Entry.HorizontalDistanceMeters
				> RouteAssignmentContract::MaxHorizontalMatchDistanceMeters)
		{
			OutError = FString::Printf(
				TEXT("Matched DroneID '%s' must have a valid finite horizontal distance in the production range [0, %.2f] meters."),
				*Entry.DroneID,
				RouteAssignmentContract::MaxHorizontalMatchDistanceMeters);
			return false;
		}

		CandidateDroneIdentityKeys.Add(DroneIdentityKey);
		MatchedRouteSlots.Add(Entry.RouteSlotIndex);
	}

	for (const FRouteAssignmentSpareDrone& Spare : Candidate.SpareDrones)
	{
		if (Spare.DroneID.IsEmpty())
		{
			OutError = TEXT("SpareDrones contains an empty DroneID.");
			return false;
		}

		const FString DroneIdentityKey = RouteAssignmentPrivate::MakeDroneIdentityKey(Spare.DroneID);
		if (CandidateDroneIdentityKeys.Contains(DroneIdentityKey))
		{
			OutError = FString::Printf(
				TEXT("DroneID '%s' appears in more than one candidate disposition bucket."),
				*Spare.DroneID);
			return false;
		}

		CandidateDroneIdentityKeys.Add(DroneIdentityKey);
	}

	for (const FRouteAssignmentExcludedDrone& Excluded : Candidate.ExcludedDrones)
	{
		if (Excluded.DroneID.IsEmpty())
		{
			OutError = TEXT("ExcludedDrones contains an empty DroneID.");
			return false;
		}

		const FString DroneIdentityKey = RouteAssignmentPrivate::MakeDroneIdentityKey(Excluded.DroneID);
		if (CandidateDroneIdentityKeys.Contains(DroneIdentityKey))
		{
			OutError = FString::Printf(
				TEXT("DroneID '%s' appears in more than one candidate disposition bucket."),
				*Excluded.DroneID);
			return false;
		}

		CandidateDroneIdentityKeys.Add(DroneIdentityKey);
	}

	for (const FRouteAssignmentUnresolvedDrone& Unresolved : Candidate.UnresolvedDrones)
	{
		if (Unresolved.DroneID.IsEmpty())
		{
			OutError = TEXT("UnresolvedDrones contains an empty DroneID.");
			return false;
		}

		const FString DroneIdentityKey = RouteAssignmentPrivate::MakeDroneIdentityKey(Unresolved.DroneID);
		if (CandidateDroneIdentityKeys.Contains(DroneIdentityKey))
		{
			OutError = FString::Printf(
				TEXT("DroneID '%s' appears in more than one candidate disposition bucket."),
				*Unresolved.DroneID);
			return false;
		}

		CandidateDroneIdentityKeys.Add(DroneIdentityKey);
	}

	const int64 CoveredDroneCount = static_cast<int64>(Candidate.MatchedEntries.Num())
		+ static_cast<int64>(Candidate.SpareDrones.Num())
		+ static_cast<int64>(Candidate.ExcludedDrones.Num())
		+ static_cast<int64>(Candidate.UnresolvedDrones.Num());
	if (static_cast<int64>(Candidate.CandidateDroneCount) != CoveredDroneCount)
	{
		OutError = FString::Printf(
			TEXT("CandidateDroneCount %d must equal MatchedEntries %d plus SpareDrones %d plus ExcludedDrones %d plus UnresolvedDrones %d."),
			Candidate.CandidateDroneCount,
			Candidate.MatchedEntries.Num(),
			Candidate.SpareDrones.Num(),
			Candidate.ExcludedDrones.Num(),
			Candidate.UnresolvedDrones.Num());
		return false;
	}

	TSet<int32> VacantRouteSlots;
	for (const int32 RouteSlotIndex : Candidate.VacantRouteSlots)
	{
		if (RouteSlotIndex < 0 || RouteSlotIndex >= Candidate.RouteSlotCount)
		{
			OutError = FString::Printf(
				TEXT("Vacant RouteSlotIndex %d is outside [0, %d)."),
				RouteSlotIndex,
				Candidate.RouteSlotCount);
			return false;
		}

		if (MatchedRouteSlots.Contains(RouteSlotIndex))
		{
			OutError = FString::Printf(
				TEXT("RouteSlotIndex %d cannot be both matched and vacant."),
				RouteSlotIndex);
			return false;
		}

		if (VacantRouteSlots.Contains(RouteSlotIndex))
		{
			OutError = FString::Printf(
				TEXT("VacantRouteSlots contains duplicate RouteSlotIndex %d."),
				RouteSlotIndex);
			return false;
		}

		VacantRouteSlots.Add(RouteSlotIndex);
	}

	const int64 CoveredRouteSlotCount = static_cast<int64>(Candidate.MatchedEntries.Num())
		+ static_cast<int64>(Candidate.VacantRouteSlots.Num());
	if (static_cast<int64>(Candidate.RouteSlotCount) != CoveredRouteSlotCount)
	{
		OutError = FString::Printf(
			TEXT("RouteSlotCount %d must equal MatchedEntries %d plus VacantRouteSlots %d."),
			Candidate.RouteSlotCount,
			Candidate.MatchedEntries.Num(),
			Candidate.VacantRouteSlots.Num());
		return false;
	}

	for (const FRouteAssignmentConflict& Conflict : Candidate.Conflicts)
	{
		if (Conflict.DroneIDs.IsEmpty() && Conflict.RouteSlotIndices.IsEmpty())
		{
			OutError = TEXT("A conflict must reference at least one DroneID or RouteSlotIndex.");
			return false;
		}

		TSet<FString> ConflictDroneIDs;
		for (const FString& DroneID : Conflict.DroneIDs)
		{
			if (DroneID.IsEmpty())
			{
				OutError = TEXT("A conflict contains an empty DroneID.");
				return false;
			}

			const FString DroneIdentityKey = RouteAssignmentPrivate::MakeDroneIdentityKey(DroneID);
			if (ConflictDroneIDs.Contains(DroneIdentityKey))
			{
				OutError = FString::Printf(TEXT("A conflict contains duplicate DroneID '%s'."), *DroneID);
				return false;
			}

			if (!CandidateDroneIdentityKeys.Contains(DroneIdentityKey))
			{
				OutError = FString::Printf(
					TEXT("Conflict DroneID '%s' is outside the candidate drone coverage."),
					*DroneID);
				return false;
			}

			ConflictDroneIDs.Add(DroneIdentityKey);
		}

		TSet<int32> ConflictRouteSlots;
		for (const int32 RouteSlotIndex : Conflict.RouteSlotIndices)
		{
			if (RouteSlotIndex < 0 || RouteSlotIndex >= Candidate.RouteSlotCount)
			{
				OutError = FString::Printf(
					TEXT("Conflict RouteSlotIndex %d is outside [0, %d)."),
					RouteSlotIndex,
					Candidate.RouteSlotCount);
				return false;
			}

			if (ConflictRouteSlots.Contains(RouteSlotIndex))
			{
				OutError = FString::Printf(
					TEXT("A conflict contains duplicate RouteSlotIndex %d."),
					RouteSlotIndex);
				return false;
			}

			if (!MatchedRouteSlots.Contains(RouteSlotIndex) && !VacantRouteSlots.Contains(RouteSlotIndex))
			{
				OutError = FString::Printf(
					TEXT("Conflict RouteSlotIndex %d is outside the route slot coverage."),
					RouteSlotIndex);
				return false;
			}

			ConflictRouteSlots.Add(RouteSlotIndex);
		}
	}

	return true;
}

void URouteAssignmentSubsystem::CanonicalizeCandidate(FRouteAssignmentCandidate& Candidate)
{
	Candidate.MatchedEntries.Sort([](const FRouteAssignmentEntry& Left, const FRouteAssignmentEntry& Right)
	{
		return Left.RouteSlotIndex < Right.RouteSlotIndex;
	});

	Candidate.SpareDrones.Sort(
		[](const FRouteAssignmentSpareDrone& Left, const FRouteAssignmentSpareDrone& Right)
		{
			return RouteAssignmentPrivate::DroneIDLess(Left.DroneID, Right.DroneID);
		});

	Candidate.ExcludedDrones.Sort(
		[](const FRouteAssignmentExcludedDrone& Left, const FRouteAssignmentExcludedDrone& Right)
		{
			return RouteAssignmentPrivate::DroneIDLess(Left.DroneID, Right.DroneID);
		});

	Candidate.UnresolvedDrones.Sort(
		[](const FRouteAssignmentUnresolvedDrone& Left, const FRouteAssignmentUnresolvedDrone& Right)
		{
			return RouteAssignmentPrivate::DroneIDLess(Left.DroneID, Right.DroneID);
		});

	Candidate.VacantRouteSlots.Sort();

	for (FRouteAssignmentConflict& Conflict : Candidate.Conflicts)
	{
		Conflict.DroneIDs.Sort(RouteAssignmentPrivate::DroneIDLess);
		Conflict.RouteSlotIndices.Sort();
	}

	Candidate.Conflicts.Sort(RouteAssignmentPrivate::ConflictLess);
}

ERouteAssignmentReadinessKind URouteAssignmentSubsystem::DetermineReadinessKind(
	const FRouteAssignmentCandidate& Candidate)
{
	const int32 MatchedCount = Candidate.MatchedEntries.Num();
	if (Candidate.RouteSlotCount <= 0
		|| MatchedCount <= 0
		|| !Candidate.UnresolvedDrones.IsEmpty()
		|| !Candidate.Conflicts.IsEmpty())
	{
		return ERouteAssignmentReadinessKind::NotReady;
	}

	if (Candidate.VacantRouteSlots.IsEmpty()
		&& MatchedCount == Candidate.RouteSlotCount)
	{
		return ERouteAssignmentReadinessKind::FullReady;
	}

	if (!Candidate.VacantRouteSlots.IsEmpty()
		&& static_cast<int64>(MatchedCount) + static_cast<int64>(Candidate.VacantRouteSlots.Num())
			== static_cast<int64>(Candidate.RouteSlotCount))
	{
		return ERouteAssignmentReadinessKind::DegradedReady;
	}

	return ERouteAssignmentReadinessKind::NotReady;
}

FRouteAssignmentSummary URouteAssignmentSubsystem::BuildSummary(
	const FRouteAssignmentCandidate& Candidate)
{
	FRouteAssignmentSummary Summary;
	Summary.MatchedCount = Candidate.MatchedEntries.Num();
	Summary.SpareCount = Candidate.SpareDrones.Num();
	Summary.ExcludedCount = Candidate.ExcludedDrones.Num();
	Summary.UnresolvedCount = Candidate.UnresolvedDrones.Num();
	Summary.VacantRouteSlotCount = Candidate.VacantRouteSlots.Num();
	Summary.ConflictCount = Candidate.Conflicts.Num();
	return Summary;
}

bool URouteAssignmentSubsystem::ValidateWorkingSnapshotForFreeze(
	const ERouteAssignmentFreezePolicy FreezePolicy,
	FString& OutError) const
{
	if (FreezePolicy != ERouteAssignmentFreezePolicy::RequireFull
		&& FreezePolicy != ERouteAssignmentFreezePolicy::AllowDegraded)
	{
		OutError = TEXT("FreezePolicy must be RequireFull or AllowDegraded.");
		return false;
	}

	if (CurrentAssignmentRevision <= 0
		|| WorkingSnapshot.AssignmentRevision != CurrentAssignmentRevision)
	{
		OutError = TEXT("Working assignment revision is inconsistent with authority state.");
		return false;
	}

	FRouteAssignmentCandidate Candidate;
	Candidate.SourceIdentity = WorkingSnapshot.SourceIdentity;
	Candidate.CandidateDroneCount = WorkingSnapshot.CandidateDroneCount;
	Candidate.RouteSlotCount = WorkingSnapshot.RouteSlotCount;
	Candidate.MatchedEntries = WorkingSnapshot.MatchedEntries;
	Candidate.SpareDrones = WorkingSnapshot.SpareDrones;
	Candidate.ExcludedDrones = WorkingSnapshot.ExcludedDrones;
	Candidate.UnresolvedDrones = WorkingSnapshot.UnresolvedDrones;
	Candidate.VacantRouteSlots = WorkingSnapshot.VacantRouteSlots;
	Candidate.Conflicts = WorkingSnapshot.Conflicts;

	if (!ValidateCandidate(Candidate, OutError))
	{
		return false;
	}

	const ERouteAssignmentReadinessKind ExpectedReadinessKind = DetermineReadinessKind(Candidate);
	if (WorkingSnapshot.ReadinessKind != ExpectedReadinessKind)
	{
		OutError = TEXT("Working assignment readiness is inconsistent with its arrays.");
		return false;
	}

	if (WorkingSnapshot.CompletionMode != ERouteAssignmentCompletionMode::Unset
		|| WorkingSnapshot.FreezePolicy != ERouteAssignmentFreezePolicy::Unset
		|| WorkingSnapshot.FrozenAtUtc.GetTicks() != 0)
	{
		OutError = TEXT("Working assignment contains frozen-only audit values.");
		return false;
	}

	const FRouteAssignmentSummary ExpectedSummary = BuildSummary(Candidate);
	if (WorkingSnapshot.Summary.MatchedCount != ExpectedSummary.MatchedCount
		|| WorkingSnapshot.Summary.SpareCount != ExpectedSummary.SpareCount
		|| WorkingSnapshot.Summary.ExcludedCount != ExpectedSummary.ExcludedCount
		|| WorkingSnapshot.Summary.UnresolvedCount != ExpectedSummary.UnresolvedCount
		|| WorkingSnapshot.Summary.VacantRouteSlotCount != ExpectedSummary.VacantRouteSlotCount
		|| WorkingSnapshot.Summary.ConflictCount != ExpectedSummary.ConflictCount)
	{
		OutError = TEXT("Working assignment summary is inconsistent with its arrays.");
		return false;
	}

	if (ExpectedSummary.UnresolvedCount != 0 || ExpectedSummary.ConflictCount != 0)
	{
		OutError = TEXT("A frozen assignment cannot contain unresolved drones or conflicts.");
		return false;
	}

	if (FreezePolicy == ERouteAssignmentFreezePolicy::RequireFull
		&& ExpectedReadinessKind != ERouteAssignmentReadinessKind::FullReady)
	{
		OutError = TEXT("RequireFull only permits a FullReady assignment.");
		return false;
	}

	if (FreezePolicy == ERouteAssignmentFreezePolicy::AllowDegraded
		&& ExpectedReadinessKind != ERouteAssignmentReadinessKind::FullReady
		&& ExpectedReadinessKind != ERouteAssignmentReadinessKind::DegradedReady)
	{
		OutError = TEXT("AllowDegraded requires a FullReady or DegradedReady assignment.");
		return false;
	}

	return true;
}
