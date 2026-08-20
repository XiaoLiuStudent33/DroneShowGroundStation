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
		uint8 SourceDigestBytes[FRouteArtifactDigest256::ByteCount];
		FMemory::Memset(SourceDigestBytes, 0x5a, sizeof(SourceDigestBytes));
		FString SourceDigestError;
		check(FRouteArtifactDigest256::FromBytes(
			SourceDigestBytes,
			UE_ARRAY_COUNT(SourceDigestBytes),
			Candidate.SourceIdentity.SourceDatasetDigest,
			SourceDigestError));
		Candidate.SourceIdentity.NormalizedShowOriginLatitudeE7 = 300000000;
		Candidate.SourceIdentity.NormalizedShowOriginLongitudeE7 = 1200000000;
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
	TestTrue(TEXT("Working preserves SourceDatasetDigest"),
		FirstWorking.SourceIdentity.SourceDatasetDigest ==
			MakeAuthoritySessionCandidate().SourceIdentity.SourceDatasetDigest);
	TestEqual(TEXT("Working preserves normalized latitude"),
		FirstWorking.SourceIdentity.NormalizedShowOriginLatitudeE7, 300000000);
	TestEqual(TEXT("Working preserves normalized longitude"),
		FirstWorking.SourceIdentity.NormalizedShowOriginLongitudeE7, 1200000000);

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
	TestTrue(TEXT("Freeze preserves stable source digest"),
		Frozen.SourceIdentity.SourceDatasetDigest ==
			SecondWorking.SourceIdentity.SourceDatasetDigest);
	TestEqual(TEXT("Freeze preserves stable source latitude"),
		Frozen.SourceIdentity.NormalizedShowOriginLatitudeE7,
		SecondWorking.SourceIdentity.NormalizedShowOriginLatitudeE7);
	TestEqual(TEXT("Freeze preserves stable source longitude"),
		Frozen.SourceIdentity.NormalizedShowOriginLongitudeE7,
		SecondWorking.SourceIdentity.NormalizedShowOriginLongitudeE7);

	URouteAssignmentSubsystem* InvalidAuthority = Second;
	for (int32 InvalidKind = 0; InvalidKind < 6; ++InvalidKind)
	{
		FRouteAssignmentCandidate Invalid = MakeAuthoritySessionCandidate();
		switch (InvalidKind)
		{
		case 0:
			Invalid.SourceIdentity.SourceDatasetDigest.Reset();
			break;
		case 1:
		{
			uint8 ZeroDigestBytes[FRouteArtifactDigest256::ByteCount]{};
			FString ZeroDigestError;
			check(FRouteArtifactDigest256::FromBytes(
				ZeroDigestBytes,
				UE_ARRAY_COUNT(ZeroDigestBytes),
				Invalid.SourceIdentity.SourceDatasetDigest,
				ZeroDigestError));
			break;
		}
		case 2:
			Invalid.SourceIdentity.NormalizedShowOriginLatitudeE7 = -900000001;
			break;
		case 3:
			Invalid.SourceIdentity.NormalizedShowOriginLatitudeE7 = 900000001;
			break;
		case 4:
			Invalid.SourceIdentity.NormalizedShowOriginLongitudeE7 = -1800000001;
			break;
		default:
			Invalid.SourceIdentity.NormalizedShowOriginLongitudeE7 = 1800000001;
			break;
		}
		TestFalse(TEXT("Authority rejects invalid stable source identity"),
			InvalidAuthority->ReplaceWorkingAssignment(Invalid, Error));
	}

	FirstGameInstance->Shutdown();
	SecondGameInstance->Shutdown();
	return true;
}

#endif
