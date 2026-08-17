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
#include "RouteAssignmentPipeline.h"
#include "RouteAssignmentSubsystem.h"
#include "RoutePreviewSubsystem.h"

#include <limits>

namespace
{
	FRouteAssignmentSourceSnapshot MakeSnapshot()
	{
		FRouteAssignmentSourceSnapshot Snapshot;
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

	bool CandidatesEquivalent(
		const FRouteAssignmentCandidate& Left,
		const FRouteAssignmentCandidate& Right)
	{
		if (Left.SourceIdentity.FleetSessionID != Right.SourceIdentity.FleetSessionID
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
	TestTrue(TEXT("Repeat is equivalent"), CandidatesEquivalent(First, Second));
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
	TestTrue(TEXT("Canonical Candidates equivalent"), CandidatesEquivalent(First, Second));
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
	TestEqual(TEXT("One live match"), Working.Summary.MatchedCount, 1);

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

#undef IMPLEMENT_INTEGRATION_TEST

#endif
