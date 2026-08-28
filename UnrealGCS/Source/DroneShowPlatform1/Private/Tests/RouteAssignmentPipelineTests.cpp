#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "DroneData.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "FleetManagerSubsystem.h"
#include "GCSDataTypes.h"
#include "HAL/FileManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RouteAssignmentContract.h"
#include "RouteAssignmentGeo.h"
#include "RouteAssignmentPipeline.h"
#include "RouteAssignmentSubsystem.h"
#include "RoutePreviewSubsystem.h"

#include <limits>

namespace
{
	FRouteAssignmentSourceSnapshot MakeSnapshot()
	{
		FRouteAssignmentSourceSnapshot Snapshot;
		uint8 SourceDigestBytes[FRouteArtifactDigest256::ByteCount];
		FMemory::Memset(SourceDigestBytes, 0x5a, sizeof(SourceDigestBytes));
		FString SourceDigestError;
		check(FRouteArtifactDigest256::FromBytes(
			SourceDigestBytes,
			UE_ARRAY_COUNT(SourceDigestBytes),
			Snapshot.SourceIdentity.SourceDatasetDigest,
			SourceDigestError));
		Snapshot.SourceIdentity.NormalizedShowOriginLatitudeE7 = 300000000;
		Snapshot.SourceIdentity.NormalizedShowOriginLongitudeE7 = 1200000000;
		Snapshot.SourceIdentity.FleetSessionID = TEXT("PIPELINE-SESSION");
		Snapshot.SourceIdentity.FleetRevisionAtCapture = 9;
		Snapshot.SourceIdentity.RouteDatasetGeneration = 4;
		Snapshot.SourceIdentity.bShowOriginSet = true;
		Snapshot.SourceIdentity.Latitude = 30.0;
		Snapshot.SourceIdentity.Longitude = 120.0;
		Snapshot.SourceIdentity.Altitude = 50.0;
		Snapshot.SourceIdentity.CapturedAtUtc = FDateTime(2026, 8, 17, 0, 0, 0);
		return Snapshot;
	}

	void AddDrone(
		FRouteAssignmentSourceSnapshot& Snapshot,
		const TCHAR* DroneID,
		const bool bOnline = true,
		const bool bGPSValid = true,
		const int32 GPSFixType = 3,
		const double Latitude = 30.0,
		const double Longitude = 120.0)
	{
		FRouteAssignmentDroneSource Drone;
		Drone.DroneID = DroneID;
		Drone.Latitude = Latitude;
		Drone.Longitude = Longitude;
		Drone.AltitudeAMSL = 50.0f;
		Drone.bGPSValid = bGPSValid;
		Drone.GPSFixType = GPSFixType;
		Drone.bAltitudeValid = true;
		Drone.bOnline = bOnline;
		Snapshot.Drones.Add(Drone);
	}

	void AddRoute(
		FRouteAssignmentSourceSnapshot& Snapshot,
		const double EastMeters,
		const double NorthMeters = 0.0)
	{
		FRouteAssignmentRouteSlotSource Route;
		Route.RouteSlotIndex = Snapshot.RouteSlots.Num();
		Route.EastMeters = EastMeters;
		Route.NorthMeters = NorthMeters;
		Route.UpMeters = 0.0;
		Snapshot.RouteSlots.Add(Route);
	}

	bool Build(
		const FRouteAssignmentSourceSnapshot& Snapshot,
		FRouteAssignmentCandidate& OutCandidate,
		FString& OutError)
	{
		return RouteAssignmentPipeline::BuildCandidateFromSourceSnapshot(
			Snapshot, OutCandidate, OutError);
	}

	URouteAssignmentSubsystem* MakeAuthority()
	{
		UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
		return NewObject<URouteAssignmentSubsystem>(GameInstance);
	}

	FRouteAssignmentCandidate MakeDirectCandidate(
		const double Distance,
		const bool bDistanceValid = true)
	{
		FRouteAssignmentCandidate Candidate;
		Candidate.SourceIdentity = MakeSnapshot().SourceIdentity;
		Candidate.CandidateDroneCount = 1;
		Candidate.RouteSlotCount = 1;
		FRouteAssignmentEntry Entry;
		Entry.DroneID = TEXT("Alpha");
		Entry.RouteSlotIndex = 0;
		Entry.HorizontalDistanceMeters = Distance;
		Entry.bHorizontalDistanceValid = bDistanceValid;
		Candidate.MatchedEntries.Add(Entry);
		return Candidate;
	}

	bool PipelineCandidatesEquivalent(
		const FRouteAssignmentCandidate& Left,
		const FRouteAssignmentCandidate& Right)
	{
		if (Left.SourceIdentity.FleetSessionID != Right.SourceIdentity.FleetSessionID
			|| Left.BindingProvenance != Right.BindingProvenance
			|| Left.SourceIdentity.FleetRevisionAtCapture
				!= Right.SourceIdentity.FleetRevisionAtCapture
			|| Left.SourceIdentity.RouteDatasetGeneration
				!= Right.SourceIdentity.RouteDatasetGeneration
			|| Left.SourceIdentity.bShowOriginSet != Right.SourceIdentity.bShowOriginSet
			|| Left.SourceIdentity.Latitude != Right.SourceIdentity.Latitude
			|| Left.SourceIdentity.Longitude != Right.SourceIdentity.Longitude
			|| Left.SourceIdentity.Altitude != Right.SourceIdentity.Altitude
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

	bool SnapshotsEquivalent(
		const FRouteAssignmentSnapshot& Left,
		const FRouteAssignmentSnapshot& Right)
	{
		if (Left.AssignmentAuthoritySessionID != Right.AssignmentAuthoritySessionID
			|| Left.BindingProvenance != Right.BindingProvenance
			|| Left.AssignmentRevision != Right.AssignmentRevision
			|| Left.SourceIdentity.FleetSessionID != Right.SourceIdentity.FleetSessionID
			|| Left.SourceIdentity.FleetRevisionAtCapture
				!= Right.SourceIdentity.FleetRevisionAtCapture
			|| Left.SourceIdentity.RouteDatasetGeneration
				!= Right.SourceIdentity.RouteDatasetGeneration
			|| Left.CandidateDroneCount != Right.CandidateDroneCount
			|| Left.RouteSlotCount != Right.RouteSlotCount
			|| Left.MatchedEntries.Num() != Right.MatchedEntries.Num()
			|| Left.SpareDrones.Num() != Right.SpareDrones.Num()
			|| Left.ExcludedDrones.Num() != Right.ExcludedDrones.Num()
			|| Left.UnresolvedDrones.Num() != Right.UnresolvedDrones.Num()
			|| Left.VacantRouteSlots != Right.VacantRouteSlots
			|| Left.ReadinessKind != Right.ReadinessKind
			|| Left.CompletionMode != Right.CompletionMode
			|| Left.FreezePolicy != Right.FreezePolicy
			|| Left.FrozenAtUtc != Right.FrozenAtUtc)
		{
			return false;
		}
		for (int32 Index = 0; Index < Left.MatchedEntries.Num(); ++Index)
		{
			if (Left.MatchedEntries[Index].DroneID != Right.MatchedEntries[Index].DroneID
				|| Left.MatchedEntries[Index].RouteSlotIndex
					!= Right.MatchedEntries[Index].RouteSlotIndex
				|| Left.MatchedEntries[Index].HorizontalDistanceMeters
					!= Right.MatchedEntries[Index].HorizontalDistanceMeters)
			{
				return false;
			}
		}
		return true;
	}

	bool ReplaceAndRead(
		FAutomationTestBase& Test,
		const FRouteAssignmentCandidate& Candidate,
		ERouteAssignmentReadinessKind ExpectedReadiness,
		ERouteAssignmentState ExpectedState,
		FRouteAssignmentSnapshot& OutSnapshot)
	{
		URouteAssignmentSubsystem* Authority = MakeAuthority();
		if (!Test.TestNotNull(TEXT("Authority created"), Authority))
		{
			return false;
		}
		FString Error;
		Test.TestTrue(TEXT("Authority accepts Pipeline candidate"),
			Authority->ReplaceWorkingAssignment(Candidate, Error));
		Test.TestEqual(TEXT("Revision issued exactly once"),
			Authority->GetCurrentAssignmentRevision(), static_cast<int64>(1));
		Test.TestEqual(TEXT("Readiness is Authority-derived"),
			Authority->GetReadinessKind(), ExpectedReadiness);
		Test.TestEqual(TEXT("State is Authority-derived"), Authority->GetState(), ExpectedState);
		Test.TestTrue(TEXT("Working snapshot readable"), Authority->GetWorkingSnapshot(OutSnapshot));
		return true;
	}

	FString MakeTemporaryRouteNpz(FAutomationTestBase& Test)
	{
		static const FString Encoded = TEXT(
			"UEsDBC0AAAAAAAAAIQDCWHD8//////////8MABQAcG9zaXRpb24ubnB5AQAQAIwAAAAAAAAAjAAAAAAAAACTTlVNUFkBAHYAeydkZXNjcic6ICc8ZjQnLCAnZm9ydHJhbl9vcmRlcic6IEZhbHNlLCAnc2hhcGUnOiAoMSwgMyksIH0gICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgCgAAAAAAAAAAAAAAAFBLAwQtAAAAAAAAACEAMNa+VP//////////CQAUAGNvbG9yLm5weQEAEACQAAAAAAAAAJAAAAAAAAAAk05VTVBZAQB2AHsnZGVzY3InOiAnPGY0JywgJ2ZvcnRyYW5fb3JkZXInOiBGYWxzZSwgJ3NoYXBlJzogKDEsIDQpLCB9ICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgIAoAAIA/AACAPwAAgD8AAIA/UEsBAi0ALQAAAAAAAAAhAMJYcPyMAAAAjAAAAAwAAAAAAAAAAAAAAIABAAAAAHBvc2l0aW9uLm5weVBLAQItAC0AAAAAAAAAIQAw1r5UkAAAAJAAAAAJAAAAAAAAAAAAAACAAcoAAABjb2xvci5ucHlQSwUGAAAAAAIAAgBxAAAAlQEAAAAA");
		TArray<uint8> Bytes;
		if (!Test.TestTrue(TEXT("Embedded NPZ decodes"), FBase64::Decode(Encoded, Bytes)))
		{
			return FString{};
		}
		const FString Directory = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("Automation/RouteAssignmentPipeline"));
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString FilePath = FPaths::Combine(Directory, TEXT("OneRoute.npz"));
		if (!Test.TestTrue(TEXT("Temporary NPZ written"),
			FFileHelper::SaveArrayToFile(Bytes, *FilePath)))
		{
			return FString{};
		}
		return FilePath;
	}

	FDroneData MakeLiveDrone()
	{
		FDroneData Drone;
		Drone.DroneID = TEXT("LiveAlpha");
		Drone.GPSLat = 30.0;
		Drone.GPSLon = 120.0;
		Drone.AltitudeAMSL = 50.0f;
		Drone.bGPSValid = true;
		Drone.GPSFixType = 3;
		Drone.bAltitudeValid = true;
		Drone.bOnline = true;
		return Drone;
	}
}

#define IMPLEMENT_PIPELINE_TEST(ClassName, LeafName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, \
		"DroneGCS.RouteAssignment.Pipeline." LeafName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_PIPELINE_TEST(FPipeline01FullTest, "01FullOneToOne")
bool FPipeline01FullTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, 0.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("One match"), Candidate.MatchedEntries.Num(), 1);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::FullReady,
		ERouteAssignmentState::Ready, Working);
	TestEqual(TEXT("No spare"), Working.Summary.SpareCount, 0);
	TestEqual(TEXT("No vacant"), Working.Summary.VacantRouteSlotCount, 0);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline02SpareTest, "02FullWithSpare")
bool FPipeline02SpareTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Bravo"));
	AddRoute(Source, 0.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("One spare"), Candidate.SpareDrones.Num(), 1);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::FullReady,
		ERouteAssignmentState::Ready, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline03DegradedTest, "03DegradedWithVacant")
bool FPipeline03DegradedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, 0.0);
	AddRoute(Source, 1.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("One vacant"), Candidate.VacantRouteSlots.Num(), 1);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::DegradedReady,
		ERouteAssignmentState::Ready, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline04ExcludedFullTest, "04ExcludedStillFull")
bool FPipeline04ExcludedFullTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Offline"), false);
	AddDrone(Source, TEXT("InvalidGPS"), true, false);
	AddDrone(Source, TEXT("LowFix"), true, true, 2);
	AddDrone(Source, TEXT("InvalidGeo"), true, true, 3, 100.0, 120.0);
	AddRoute(Source, 0.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("Four excluded"), Candidate.ExcludedDrones.Num(), 4);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::FullReady,
		ERouteAssignmentState::Ready, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline05NoCandidateTest, "05NoCandidateDraft")
bool FPipeline05NoCandidateTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, 1.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("One unresolved"), Candidate.UnresolvedDrones.Num(), 1);
	TestEqual(TEXT("NoCandidate reason"), Candidate.UnresolvedDrones[0].Reason,
		ERouteAssignmentUnresolvedReason::NoCandidate);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::NotReady,
		ERouteAssignmentState::Draft, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline06DeficientTest, "06DeficientConflictDraft")
bool FPipeline06DeficientTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Bravo"));
	AddDrone(Source, TEXT("Charlie"));
	AddRoute(Source, 0.0);
	AddRoute(Source, 0.1);
	AddRoute(Source, 1.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("One unresolved"), Candidate.UnresolvedDrones.Num(), 1);
	TestEqual(TEXT("Conflict reason"), Candidate.UnresolvedDrones[0].Reason,
		ERouteAssignmentUnresolvedReason::Conflict);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::NotReady,
		ERouteAssignmentState::Draft, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline07ZeroEligibleTest, "07ZeroEligibleDraft")
bool FPipeline07ZeroEligibleTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Offline"), false);
	AddRoute(Source, 0.0);
	AddRoute(Source, 1.0);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("No match"), Candidate.MatchedEntries.Num(), 0);
	TestEqual(TEXT("All routes vacant"), Candidate.VacantRouteSlots.Num(), 2);
	FRouteAssignmentSnapshot Working;
	ReplaceAndRead(*this, Candidate, ERouteAssignmentReadinessKind::NotReady,
		ERouteAssignmentState::Draft, Working);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline08BoundaryTest, "08ExactProductionBoundary")
bool FPipeline08BoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, RouteAssignmentContract::MaxHorizontalMatchDistanceMeters);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Exact boundary succeeds"), Build(Source, Candidate, Error));
	TestEqual(TEXT("Boundary produces match"), Candidate.MatchedEntries.Num(), 1);
	TestEqual(TEXT("Exact distance retained"),
		Candidate.MatchedEntries[0].HorizontalDistanceMeters,
		RouteAssignmentContract::MaxHorizontalMatchDistanceMeters);
	return true;
}

IMPLEMENT_PIPELINE_TEST(
	FPipeline08FixedFrameAnchorMatrixTest,
	"08FixedFrameAnchorMatrix")
bool FPipeline08FixedFrameAnchorMatrixTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	RouteAssignmentGeo::FLocalEnuOriginContext Origin;
	FString Error;
	if (!TestTrue(TEXT("Fixed Show Origin ENU context builds"),
		RouteAssignmentGeo::BuildLocalEnuOriginContext(
			Source.SourceIdentity.Latitude,
			Source.SourceIdentity.Longitude,
			Source.SourceIdentity.Altitude,
			Origin,
			Error)))
	{
		return false;
	}

	constexpr int32 AnchorRouteSlot = 14;
	constexpr int32 GridWidth = 10;
	const int32 AnchorColumn = AnchorRouteSlot % GridWidth;
	const int32 AnchorRow = AnchorRouteSlot / GridWidth;
	TMap<FString, int32> ExpectedRouteSlotByDrone;
	for (int32 RouteSlot = 0; RouteSlot < 100; ++RouteSlot)
	{
		const double EastMeters = static_cast<double>(RouteSlot % GridWidth - AnchorColumn);
		const double NorthMeters = static_cast<double>(RouteSlot / GridWidth - AnchorRow);
		FRouteAssignmentRouteSlotSource Route;
		Route.RouteSlotIndex = RouteSlot;
		Route.EastMeters = EastMeters;
		Route.NorthMeters = NorthMeters;
		Source.RouteSlots.Add(Route);

		RouteAssignmentGeo::FGeodeticPosition Geodetic;
		if (!TestTrue(TEXT("Grid point converts to WGS84"),
			RouteAssignmentGeo::LocalEnuToGeodetic(
				Origin, EastMeters, NorthMeters, 0.0, Geodetic, Error)))
		{
			return false;
		}
		// Deliberately permute network DroneID relative to RouteSlot. Matching must
		// use the fixed ENU/Frame-0 position, never DroneID-1.
		const int32 NumericDroneID = (RouteSlot * 37) % 100 + 1;
		const FString DroneID = FString::Printf(TEXT("Drone%03d"), NumericDroneID);
		AddDrone(Source, *DroneID, true, true, 6,
			Geodetic.LatitudeDegrees, Geodetic.LongitudeDegrees);
		ExpectedRouteSlotByDrone.Add(DroneID, RouteSlot);
	}

	FRouteAssignmentCandidate Candidate;
	TestTrue(TEXT("Symmetric matrix matches directly without a rigid-solve ambiguity gate"),
		Build(Source, Candidate, Error));
	TestEqual(TEXT("All 100 drones matched"), Candidate.MatchedEntries.Num(), 100);
	TestEqual(TEXT("No unresolved drones"), Candidate.UnresolvedDrones.Num(), 0);
	TestEqual(TEXT("No matching conflicts"), Candidate.Conflicts.Num(), 0);
	TestEqual(TEXT("No vacant RouteSlots"), Candidate.VacantRouteSlots.Num(), 0);
	TestEqual(TEXT("No spare drones"), Candidate.SpareDrones.Num(), 0);
	for (const FRouteAssignmentEntry& Entry : Candidate.MatchedEntries)
	{
		const int32* ExpectedRouteSlot = ExpectedRouteSlotByDrone.Find(Entry.DroneID);
		TestNotNull(TEXT("Matched DroneID belongs to the captured fleet"), ExpectedRouteSlot);
		if (ExpectedRouteSlot)
		{
			TestEqual(*FString::Printf(TEXT("%s maps by position"), *Entry.DroneID),
				Entry.RouteSlotIndex, *ExpectedRouteSlot);
		}
	}
	const FRouteAssignmentEntry* AnchorEntry = Candidate.MatchedEntries.FindByPredicate(
		[](const FRouteAssignmentEntry& Entry)
		{
			return Entry.RouteSlotIndex == AnchorRouteSlot;
		});
	TestNotNull(TEXT("Boundary pile anchor RouteSlot is present"), AnchorEntry);
	if (AnchorEntry)
	{
		TestEqual(TEXT("The Show Origin pile may be RouteSlot 14, not forced to RouteSlot 0"),
			AnchorEntry->DroneID, FString(TEXT("Drone019")));
		TestTrue(TEXT("Anchor horizontal residual is effectively zero"),
			AnchorEntry->HorizontalDistanceMeters < 0.001);
	}
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline09InvalidSourceTest, "09InvalidSourceFailsWithStage")
bool FPipeline09InvalidSourceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	Source.SourceIdentity.bShowOriginSet = false;
	FRouteAssignmentCandidate Candidate = MakeDirectCandidate(0.0);
	FString Error;
	TestFalse(TEXT("Invalid source fails"), Build(Source, Candidate, Error));
	TestTrue(TEXT("Geo stage retained"), Error.StartsWith(TEXT("Geo:")));
	TestEqual(TEXT("Candidate cleared"), Candidate.CandidateDroneCount, 0);
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline10RepeatTest, "10RepeatDeterministic")
bool FPipeline10RepeatTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Bravo"));
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, 0.0);
	FRouteAssignmentCandidate First;
	FRouteAssignmentCandidate Second;
	FString Error;
	TestTrue(TEXT("First succeeds"), Build(Source, First, Error));
	TestTrue(TEXT("Second succeeds"), Build(Source, Second, Error));
	TestTrue(TEXT("Repeat is equivalent"), PipelineCandidatesEquivalent(First, Second));
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline11CanonicalTest, "11CanonicalEquivalentCandidate")
bool FPipeline11CanonicalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Delta"));
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Charlie"));
	AddRoute(Source, 0.0);
	AddRoute(Source, 0.1);
	FRouteAssignmentCandidate First;
	FRouteAssignmentCandidate Second;
	FString Error;
	TestTrue(TEXT("First succeeds"), Build(Source, First, Error));
	TestTrue(TEXT("Same snapshot succeeds again"), Build(Source, Second, Error));
	TestTrue(TEXT("Canonical Candidates equivalent"), PipelineCandidatesEquivalent(First, Second));
	TestTrue(TEXT("Spare array exists"), !First.SpareDrones.IsEmpty());
	return true;
}

IMPLEMENT_PIPELINE_TEST(FPipeline12ProductionConfigTest, "12ProductionThresholdActuallyUsed")
bool FPipeline12ProductionConfigTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddRoute(Source, RouteAssignmentContract::MaxHorizontalMatchDistanceMeters + 0.000001);
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Pipeline still materializes partition"), Build(Source, Candidate, Error));
	TestEqual(TEXT("Above boundary does not match"), Candidate.MatchedEntries.Num(), 0);
	TestEqual(TEXT("Above boundary is NoCandidate"), Candidate.UnresolvedDrones[0].Reason,
		ERouteAssignmentUnresolvedReason::NoCandidate);
	return true;
}

#undef IMPLEMENT_PIPELINE_TEST

#define IMPLEMENT_EXPLICIT_TEST(ClassName, LeafName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, \
		"DroneGCS.RouteAssignment.Explicit." LeafName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_EXPLICIT_TEST(FExplicit01ValidThreeTest, "01ValidThreeBindings")
bool FExplicit01ValidThreeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Bravo"));
	AddDrone(Source, TEXT("Charlie"));
	AddRoute(Source, 0.0);
	AddRoute(Source, 1.0);
	AddRoute(Source, 2.0);
	TArray<FRouteAssignmentExplicitBinding> Bindings;
	Bindings.Add({TEXT("Charlie"), 2});
	Bindings.Add({TEXT("Alpha"), 0});
	Bindings.Add({TEXT("Bravo"), 1});
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Three explicit bindings materialize"),
		URouteAssignmentSubsystem::BuildExplicitCandidateFromSourceSnapshot(
			Source, Bindings, Candidate, Error));
	TestEqual(TEXT("Explicit provenance retained"), Candidate.BindingProvenance,
		ERouteAssignmentBindingProvenance::Explicit);
	TestEqual(TEXT("All explicit drones matched"), Candidate.MatchedEntries.Num(), 3);
	TestEqual(TEXT("No route slots vacant"), Candidate.VacantRouteSlots.Num(), 0);
	for (const FRouteAssignmentEntry& Entry : Candidate.MatchedEntries)
	{
		TestFalse(TEXT("Explicit entry has no positional distance"),
			Entry.bHorizontalDistanceValid);
		TestEqual(TEXT("Explicit distance storage is canonical zero"),
			Entry.HorizontalDistanceMeters, 0.0);
	}
	return true;
}

IMPLEMENT_EXPLICIT_TEST(FExplicit02PartialDegradedTest, "02PartialDegradedDomain")
bool FExplicit02PartialDegradedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Bravo"));
	AddDrone(Source, TEXT("Charlie"));
	for (int32 Slot = 0; Slot < 1200; ++Slot)
	{
		AddRoute(Source, static_cast<double>(Slot));
	}
	TArray<FRouteAssignmentExplicitBinding> Bindings = {
		{TEXT("Alpha"), 0}, {TEXT("Bravo"), 1}, {TEXT("Charlie"), 2}};
	FRouteAssignmentCandidate Candidate;
	FString Error;
	TestTrue(TEXT("Three-of-1200 explicit assignment materializes"),
		URouteAssignmentSubsystem::BuildExplicitCandidateFromSourceSnapshot(
			Source, Bindings, Candidate, Error));
	TestEqual(TEXT("Three routes matched"), Candidate.MatchedEntries.Num(), 3);
	TestEqual(TEXT("Remaining routes vacant"), Candidate.VacantRouteSlots.Num(), 1197);
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	TestTrue(TEXT("Partial explicit candidate accepted"),
		Authority->ReplaceWorkingAssignment(Candidate, Error));
	TestEqual(TEXT("Partial explicit assignment is DegradedReady"),
		Authority->GetReadinessKind(), ERouteAssignmentReadinessKind::DegradedReady);
	TestTrue(TEXT("Partial explicit assignment freezes with degraded policy"),
		Authority->Freeze(ERouteAssignmentFreezePolicy::AllowDegraded, Error));
	return true;
}

IMPLEMENT_EXPLICIT_TEST(FExplicit03InvalidInputTest, "03InvalidInputs")
bool FExplicit03InvalidInputTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentSourceSnapshot Source = MakeSnapshot();
	AddDrone(Source, TEXT("Alpha"));
	AddDrone(Source, TEXT("Bravo"));
	AddRoute(Source, 0.0);
	AddRoute(Source, 1.0);
	auto Rejects = [&Source](const TArray<FRouteAssignmentExplicitBinding>& Bindings)
	{
		FRouteAssignmentCandidate Candidate;
		FString Error;
		return !URouteAssignmentSubsystem::BuildExplicitCandidateFromSourceSnapshot(
			Source, Bindings, Candidate, Error);
	};
	TestTrue(TEXT("Duplicate Drone rejected"), Rejects({
		{TEXT("Alpha"), 0}, {TEXT("alpha"), 1}}));
	TestTrue(TEXT("Duplicate RouteSlot rejected"), Rejects({
		{TEXT("Alpha"), 0}, {TEXT("Bravo"), 0}}));
	TestTrue(TEXT("Unknown Drone rejected"), Rejects({{TEXT("Unknown"), 0}}));
	TestTrue(TEXT("Out-of-range RouteSlot rejected"),
		Rejects({{TEXT("Alpha"), 2}}));
	return true;
}

IMPLEMENT_EXPLICIT_TEST(FExplicit04DistanceContractTest, "04DistanceContract")
bool FExplicit04DistanceContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteAssignmentCandidate Explicit = MakeDirectCandidate(0.0, false);
	Explicit.BindingProvenance = ERouteAssignmentBindingProvenance::Explicit;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestTrue(TEXT("Explicit candidate does not require positional distance"),
		Authority->ReplaceWorkingAssignment(Explicit, Error));
	Authority->Reset();
	Explicit.MatchedEntries[0].HorizontalDistanceMeters = 0.1;
	Explicit.MatchedEntries[0].bHorizontalDistanceValid = true;
	TestFalse(TEXT("Explicit candidate cannot masquerade as positional"),
		Authority->ReplaceWorkingAssignment(Explicit, Error));
	TestFalse(TEXT("Positional candidate above 0.20m remains rejected"),
		Authority->ReplaceWorkingAssignment(
			MakeDirectCandidate(
				RouteAssignmentContract::MaxHorizontalMatchDistanceMeters + 0.001),
			Error));
	return true;
}

#undef IMPLEMENT_EXPLICIT_TEST

#define IMPLEMENT_DEFENSE_TEST(ClassName, LeafName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, \
		"DroneGCS.RouteAssignment.AuthorityDefense." LeafName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense01InvalidFlagTest, "01DistanceValidityRequired")
bool FAuthorityDefense01InvalidFlagTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestFalse(TEXT("Invalid flag rejected"),
		Authority->ReplaceWorkingAssignment(MakeDirectCandidate(0.1, false), Error));
	TestEqual(TEXT("Revision unchanged"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense02NanTest, "02NaNRejected")
bool FAuthorityDefense02NanTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestFalse(TEXT("NaN rejected"), Authority->ReplaceWorkingAssignment(
		MakeDirectCandidate(std::numeric_limits<double>::quiet_NaN()), Error));
	TestEqual(TEXT("Revision unchanged"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense03NegativeTest, "03NegativeRejected")
bool FAuthorityDefense03NegativeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestFalse(TEXT("Negative rejected"),
		Authority->ReplaceWorkingAssignment(MakeDirectCandidate(-0.001), Error));
	TestEqual(TEXT("Revision unchanged"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense04BoundaryTest, "04ExactBoundaryAccepted")
bool FAuthorityDefense04BoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestTrue(TEXT("Exact boundary accepted"), Authority->ReplaceWorkingAssignment(
		MakeDirectCandidate(RouteAssignmentContract::MaxHorizontalMatchDistanceMeters), Error));
	TestEqual(TEXT("Revision issued once"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(1));
	return true;
}

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense05AboveBoundaryTest, "05AboveBoundaryRejected")
bool FAuthorityDefense05AboveBoundaryTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestFalse(TEXT("Just above boundary rejected"), Authority->ReplaceWorkingAssignment(
		MakeDirectCandidate(RouteAssignmentContract::MaxHorizontalMatchDistanceMeters + 0.000001), Error));
	TestEqual(TEXT("Revision unchanged"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_DEFENSE_TEST(FAuthorityDefense06LargeDistanceTest, "06Point35Rejected")
bool FAuthorityDefense06LargeDistanceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	URouteAssignmentSubsystem* Authority = MakeAuthority();
	FString Error;
	TestFalse(TEXT("0.35m rejected"),
		Authority->ReplaceWorkingAssignment(MakeDirectCandidate(0.35), Error));
	TestEqual(TEXT("Revision unchanged"), Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));
	return true;
}

#undef IMPLEMENT_DEFENSE_TEST

#define IMPLEMENT_INTEGRATION_TEST(ClassName, LeafName) \
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, \
		"DroneGCS.RouteAssignment.Integration." LeafName, \
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

IMPLEMENT_INTEGRATION_TEST(FIntegration01LiveSourceTest, "01LiveSourceEndToEnd")
bool FIntegration01LiveSourceTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString RouteFile = MakeTemporaryRouteNpz(*this);
	if (RouteFile.IsEmpty())
	{
		return false;
	}

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteAssignmentLivePipelineTestWorld"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	URoutePreviewSubsystem* Route = GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	URouteAssignmentSubsystem* Authority = GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	if (!TestNotNull(TEXT("Fleet available"), Fleet)
		|| !TestNotNull(TEXT("Route available"), Route)
		|| !TestNotNull(TEXT("Authority available"), Authority))
	{
		GameInstance->Shutdown();
		IFileManager::Get().Delete(*RouteFile, false, true, true);
		return false;
	}

	FGCSFleetSnapshot FleetSnapshot;
	FleetSnapshot.Header.SessionID = TEXT("LIVE-PIPELINE-SESSION");
	FleetSnapshot.FleetRevision = 21;
	FleetSnapshot.Drones.Add(MakeLiveDrone());
	TestTrue(TEXT("Fleet source installed"), Fleet->ApplySnapshot(FleetSnapshot));
	FString Error;
	TestTrue(TEXT("Route source loaded"), Route->LoadRoute(RouteFile, Error));
	TestTrue(TEXT("ShowOrigin installed"), Route->SetShowOrigin(30.0, 120.0, 50.0, Error));
	TestTrue(TEXT("Live high-level Pipeline succeeds"),
		Authority->BuildAndReplaceWorkingAssignment(Error));
	TestEqual(TEXT("Exactly one revision issued"),
		Authority->GetCurrentAssignmentRevision(), static_cast<int64>(1));
	TestEqual(TEXT("Live result FullReady"), Authority->GetReadinessKind(),
		ERouteAssignmentReadinessKind::FullReady);
	TestEqual(TEXT("Live result Ready"), Authority->GetState(), ERouteAssignmentState::Ready);
	FRouteAssignmentSnapshot Working;
	TestTrue(TEXT("Live Working snapshot available"), Authority->GetWorkingSnapshot(Working));
	TestEqual(TEXT("Capture Fleet session retained"),
		Working.SourceIdentity.FleetSessionID, FString(TEXT("LIVE-PIPELINE-SESSION")));
	TestEqual(TEXT("Capture Fleet revision retained"),
		Working.SourceIdentity.FleetRevisionAtCapture, static_cast<int64>(21));
	TestEqual(TEXT("Capture Route generation retained"),
		Working.SourceIdentity.RouteDatasetGeneration, static_cast<int64>(1));
	TestTrue(TEXT("Capture SourceDatasetDigest retained"),
		Working.SourceIdentity.SourceDatasetDigest ==
			Route->GetRouteData().SourceIdentity.SourceDatasetDigest);
	TestEqual(TEXT("Capture normalized ShowOrigin latitude retained"),
		Working.SourceIdentity.NormalizedShowOriginLatitudeE7, 300000000);
	TestEqual(TEXT("Capture normalized ShowOrigin longitude retained"),
		Working.SourceIdentity.NormalizedShowOriginLongitudeE7, 1200000000);
	TestEqual(TEXT("One live match"), Working.Summary.MatchedCount, 1);
	TestTrue(TEXT("Live Working freezes"),
		Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, Error));
	FRouteAssignmentSnapshot Frozen;
	TestTrue(TEXT("Live Frozen snapshot available"), Authority->GetFrozenSnapshot(Frozen));
	TestTrue(TEXT("Freeze preserves SourceDatasetDigest"),
		Frozen.SourceIdentity.SourceDatasetDigest ==
			Working.SourceIdentity.SourceDatasetDigest);
	TestEqual(TEXT("Freeze preserves normalized latitude"),
		Frozen.SourceIdentity.NormalizedShowOriginLatitudeE7,
		Working.SourceIdentity.NormalizedShowOriginLatitudeE7);
	TestEqual(TEXT("Freeze preserves normalized longitude"),
		Frozen.SourceIdentity.NormalizedShowOriginLongitudeE7,
		Working.SourceIdentity.NormalizedShowOriginLongitudeE7);
	TestEqual(TEXT("Freeze preserves route domain"),
		Frozen.RouteSlotCount, Working.RouteSlotCount);

	GameInstance->Shutdown();
	IFileManager::Get().Delete(*RouteFile, false, true, true);
	return true;
}

IMPLEMENT_INTEGRATION_TEST(FIntegration02FailureAtomicTest, "02CaptureFailurePreservesWorking")
bool FIntegration02FailureAtomicTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteAssignmentFailureAtomicTestWorld"));
	URouteAssignmentSubsystem* Authority = GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	if (!TestNotNull(TEXT("Authority available"), Authority))
	{
		GameInstance->Shutdown();
		return false;
	}
	FString Error;
	TestTrue(TEXT("Seed Working accepted"),
		Authority->ReplaceWorkingAssignment(MakeDirectCandidate(0.1), Error));
	FRouteAssignmentSnapshot Before;
	TestTrue(TEXT("Seed Working readable"), Authority->GetWorkingSnapshot(Before));
	TestFalse(TEXT("Live rebuild fails without source"),
		Authority->BuildAndReplaceWorkingAssignment(Error));
	FRouteAssignmentSnapshot After;
	TestTrue(TEXT("Old Working remains readable"), Authority->GetWorkingSnapshot(After));
	TestTrue(TEXT("Old Working remains identical"), SnapshotsEquivalent(Before, After));
	TestEqual(TEXT("Revision unchanged"),
		Authority->GetCurrentAssignmentRevision(), Before.AssignmentRevision);
	GameInstance->Shutdown();
	return true;
}

IMPLEMENT_INTEGRATION_TEST(FIntegration03FrozenTest, "03FrozenRejectsBeforeCapture")
bool FIntegration03FrozenTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteAssignmentFrozenPipelineTestWorld"));
	URouteAssignmentSubsystem* Authority = GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	if (!TestNotNull(TEXT("Authority available"), Authority))
	{
		GameInstance->Shutdown();
		return false;
	}
	FString Error;
	TestTrue(TEXT("Seed Working accepted"),
		Authority->ReplaceWorkingAssignment(MakeDirectCandidate(0.1), Error));
	TestTrue(TEXT("Seed frozen"),
		Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, Error));
	FRouteAssignmentSnapshot Before;
	TestTrue(TEXT("Frozen snapshot readable"), Authority->GetFrozenSnapshot(Before));
	TestFalse(TEXT("Frozen high-level rebuild rejected"),
		Authority->BuildAndReplaceWorkingAssignment(Error));
	FRouteAssignmentSnapshot After;
	TestTrue(TEXT("Frozen snapshot remains readable"), Authority->GetFrozenSnapshot(After));
	TestTrue(TEXT("Frozen snapshot unchanged"), SnapshotsEquivalent(Before, After));
	TestEqual(TEXT("Revision unchanged"),
		Authority->GetCurrentAssignmentRevision(), Before.AssignmentRevision);
	TestEqual(TEXT("State remains Frozen"), Authority->GetState(), ERouteAssignmentState::Frozen);
	GameInstance->Shutdown();
	return true;
}

IMPLEMENT_INTEGRATION_TEST(FIntegration04ExplicitAuthorityTest, "04ExplicitAuthorityAndStaleSources")
bool FIntegration04ExplicitAuthorityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString RouteFile = MakeTemporaryRouteNpz(*this);
	if (RouteFile.IsEmpty())
	{
		return false;
	}
	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("RouteAssignmentExplicitAuthorityTestWorld"));
	UFleetManagerSubsystem* Fleet = GameInstance->GetSubsystem<UFleetManagerSubsystem>();
	URoutePreviewSubsystem* Route = GameInstance->GetSubsystem<URoutePreviewSubsystem>();
	URouteAssignmentSubsystem* Authority =
		GameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	if (!TestNotNull(TEXT("Fleet available"), Fleet)
		|| !TestNotNull(TEXT("Route available"), Route)
		|| !TestNotNull(TEXT("Authority available"), Authority))
	{
		GameInstance->Shutdown();
		IFileManager::Get().Delete(*RouteFile, false, true, true);
		return false;
	}

	FGCSFleetSnapshot FleetSnapshot;
	FleetSnapshot.Header.SessionID = TEXT("EXPLICIT-FLEET-SESSION");
	FleetSnapshot.FleetRevision = 40;
	FleetSnapshot.Drones.Add(MakeLiveDrone());
	TestTrue(TEXT("Initial Fleet installed"), Fleet->ApplySnapshot(FleetSnapshot));
	FString Error;
	TestTrue(TEXT("Initial Route loaded"), Route->LoadRoute(RouteFile, Error));
	TestTrue(TEXT("Initial ShowOrigin set"),
		Route->SetShowOrigin(30.0, 120.0, 50.0, Error));

	FRouteAssignmentSourceSnapshot InitialSource;
	TestTrue(TEXT("Initial explicit source captured"),
		Authority->CaptureSourceSnapshot(InitialSource, Error));
	FRouteAssignmentExplicitBindingRequest Request;
	Request.ExpectedSourceIdentity = InitialSource.SourceIdentity;
	Request.Bindings.Add({TEXT("LiveAlpha"), 0});

	FleetSnapshot.FleetRevision = 41;
	TestTrue(TEXT("Fleet revision advanced"), Fleet->ApplySnapshot(FleetSnapshot));
	TestFalse(TEXT("Stale Fleet explicit request rejected"),
		Authority->BuildAndReplaceExplicitWorkingAssignment(Request, Error));
	TestEqual(TEXT("Stale Fleet issued no revision"),
		Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));

	FRouteAssignmentSourceSnapshot FleetCurrentSource;
	TestTrue(TEXT("Fleet-current source captured"),
		Authority->CaptureSourceSnapshot(FleetCurrentSource, Error));
	Request.ExpectedSourceIdentity = FleetCurrentSource.SourceIdentity;
	TestTrue(TEXT("Route reloaded"), Route->LoadRoute(RouteFile, Error));
	TestTrue(TEXT("Reloaded ShowOrigin set"),
		Route->SetShowOrigin(30.0, 120.0, 50.0, Error));
	TestFalse(TEXT("Stale Route explicit request rejected"),
		Authority->BuildAndReplaceExplicitWorkingAssignment(Request, Error));
	TestEqual(TEXT("Stale Route issued no revision"),
		Authority->GetCurrentAssignmentRevision(), static_cast<int64>(0));

	FRouteAssignmentSourceSnapshot CurrentSource;
	TestTrue(TEXT("Current explicit source captured"),
		Authority->CaptureSourceSnapshot(CurrentSource, Error));
	Request.ExpectedSourceIdentity = CurrentSource.SourceIdentity;
	TestTrue(TEXT("Current explicit request accepted"),
		Authority->BuildAndReplaceExplicitWorkingAssignment(Request, Error));
	FRouteAssignmentSnapshot Working;
	TestTrue(TEXT("Explicit Working snapshot available"),
		Authority->GetWorkingSnapshot(Working));
	TestEqual(TEXT("Working provenance is Explicit"), Working.BindingProvenance,
		ERouteAssignmentBindingProvenance::Explicit);
	TestFalse(TEXT("Explicit Working carries no positional residual"),
		Working.MatchedEntries[0].bHorizontalDistanceValid);
	TestTrue(TEXT("Explicit Working freezes through the standard authority"),
		Authority->Freeze(ERouteAssignmentFreezePolicy::RequireFull, Error));
	FRouteAssignmentSnapshot Frozen;
	TestTrue(TEXT("Explicit Frozen snapshot available"),
		Authority->GetFrozenSnapshot(Frozen));
	TestEqual(TEXT("Frozen provenance remains local Explicit audit metadata"),
		Frozen.BindingProvenance, ERouteAssignmentBindingProvenance::Explicit);

	GameInstance->Shutdown();
	IFileManager::Get().Delete(*RouteFile, false, true, true);
	return true;
}

#undef IMPLEMENT_INTEGRATION_TEST

#endif
