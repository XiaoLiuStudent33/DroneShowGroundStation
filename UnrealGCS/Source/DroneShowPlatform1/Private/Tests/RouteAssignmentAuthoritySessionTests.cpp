#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "RouteAssignmentSubsystem.h"

namespace
{
	FRouteAssignmentCandidate MakeAuthoritySessionCandidate()
	{
		FRouteAssignmentCandidate Candidate;
		Candidate.SourceIdentity.FleetSessionID = TEXT("AUTHORITY-SESSION-TEST-FLEET");
		Candidate.SourceIdentity.FleetRevisionAtCapture = 7;
		Candidate.SourceIdentity.RouteDatasetGeneration = 3;
		Candidate.SourceIdentity.bShowOriginSet = true;
		Candidate.SourceIdentity.Latitude = 30.0;
		Candidate.SourceIdentity.Longitude = 120.0;
		Candidate.SourceIdentity.Altitude = 50.0;
		Candidate.SourceIdentity.CapturedAtUtc = FDateTime::UtcNow();
		Candidate.CandidateDroneCount = 1;
		Candidate.RouteSlotCount = 1;

		FRouteAssignmentEntry Entry;
		Entry.DroneID = TEXT("DroneAuthorityEpoch");
		Entry.RouteSlotIndex = 0;
		Entry.HorizontalDistanceMeters = 0.1;
		Entry.bHorizontalDistanceValid = true;
		Candidate.MatchedEntries.Add(MoveTemp(Entry));
		return Candidate;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteAssignmentAuthoritySessionLifetimeTest,
	"DroneGCS.RouteAssignment.AuthoritySession.LifetimeContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteAssignmentAuthoritySessionLifetimeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	UGameInstance* FirstGameInstance = NewObject<UGameInstance>(GEngine);
	UGameInstance* SecondGameInstance = NewObject<UGameInstance>(GEngine);
	FirstGameInstance->InitializeStandalone(TEXT("RouteAssignmentAuthoritySessionFirst"));
	SecondGameInstance->InitializeStandalone(TEXT("RouteAssignmentAuthoritySessionSecond"));

	URouteAssignmentSubsystem* First =
		FirstGameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	URouteAssignmentSubsystem* Second =
		SecondGameInstance->GetSubsystem<URouteAssignmentSubsystem>();
	if (!TestNotNull(TEXT("First RouteAssignment authority exists"), First)
		|| !TestNotNull(TEXT("Second RouteAssignment authority exists"), Second))
	{
		FirstGameInstance->Shutdown();
		SecondGameInstance->Shutdown();
		return false;
	}

	const FString FirstEpoch = First->GetAssignmentAuthoritySessionID();
	const FString SecondEpoch = Second->GetAssignmentAuthoritySessionID();
	TestFalse(TEXT("Authority epoch is generated"), FirstEpoch.IsEmpty());
	TestEqual(TEXT("Authority epoch is stable within subsystem lifetime"),
		First->GetAssignmentAuthoritySessionID(), FirstEpoch);
	TestNotEqual(TEXT("New GameInstance receives a new authority epoch"),
		SecondEpoch, FirstEpoch);

	FString Error;
	TestTrue(TEXT("First replacement succeeds"),
		First->ReplaceWorkingAssignment(MakeAuthoritySessionCandidate(), Error));
	FRouteAssignmentSnapshot FirstWorking;
	TestTrue(TEXT("First working snapshot is readable"),
		First->GetWorkingSnapshot(FirstWorking));
	TestEqual(TEXT("Working snapshot captures authority epoch"),
		FirstWorking.AssignmentAuthoritySessionID, FirstEpoch);
	TestEqual(TEXT("First assignment revision"),
		FirstWorking.AssignmentRevision, static_cast<int64>(1));

	First->Reset();
	TestEqual(TEXT("Reset preserves authority epoch"),
		First->GetAssignmentAuthoritySessionID(), FirstEpoch);
	TestTrue(TEXT("Replacement after Reset succeeds"),
		First->ReplaceWorkingAssignment(MakeAuthoritySessionCandidate(), Error));
	FRouteAssignmentSnapshot SecondWorking;
	TestTrue(TEXT("Second working snapshot is readable"),
		First->GetWorkingSnapshot(SecondWorking));
	TestEqual(TEXT("Revision changes without changing authority epoch"),
		SecondWorking.AssignmentRevision, static_cast<int64>(2));
	TestEqual(TEXT("Replacement preserves authority epoch"),
		SecondWorking.AssignmentAuthoritySessionID, FirstEpoch);

	TestTrue(TEXT("Freeze succeeds"),
		First->Freeze(ERouteAssignmentFreezePolicy::RequireFull, Error));
	FRouteAssignmentSnapshot Frozen;
	TestTrue(TEXT("Frozen snapshot is readable"), First->GetFrozenSnapshot(Frozen));
	TestEqual(TEXT("Freeze preserves authority epoch"),
		Frozen.AssignmentAuthoritySessionID, FirstEpoch);
	TestEqual(TEXT("Freeze does not issue another revision"),
		Frozen.AssignmentRevision, SecondWorking.AssignmentRevision);

	FirstGameInstance->Shutdown();
	SecondGameInstance->Shutdown();
	return true;
}

#endif
