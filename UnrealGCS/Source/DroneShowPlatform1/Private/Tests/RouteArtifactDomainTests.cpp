#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Misc/Paths.h"
#include "RouteArtifactTypes.h"
#include "RouteArtifactWorkspacePaths.h"

namespace
{
	FRouteArtifactDigest256 HashUtf8(
		FAutomationTestBase& Test,
		const ANSICHAR* Text)
	{
		FRouteArtifactDigest256 Digest;
		FString Error;
		const uint64 Length = static_cast<uint64>(FCStringAnsi::Strlen(Text));
		Test.TestTrue(
			TEXT("Synthetic digest computes"),
			FRouteArtifactSha256::HashBytes(Text, Length, Digest, Error));
		return Digest;
	}

	FRouteArtifactBuildIdentity MakeBuildIdentity(FAutomationTestBase& Test)
	{
		FRouteArtifactBuildIdentity Identity;
		Identity.AssignmentBinding.FleetSessionID = TEXT("Fleet-Session-A");
		Identity.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("Authority-Session-A");
		Identity.AssignmentBinding.AssignmentRevision = 7;
		Identity.AssignmentBinding.AssignmentDigest = HashUtf8(Test, "assignment");
		Identity.SourceDatasetDigest = HashUtf8(Test, "source");
		Identity.WaypointGeneratorVersion = TEXT("Generator-Test-v1");
		Identity.GeneratorInputDigest = HashUtf8(Test, "generator-input");
		return Identity;
	}

	bool ComputeSetID(
		FAutomationTestBase& Test,
		const FRouteArtifactBuildIdentity& Identity,
		FString& OutID)
	{
		FString Error;
		return Test.TestTrue(
			TEXT("ArtifactSetID computes"),
			RouteArtifactIdentity::ComputeArtifactSetID(Identity, OutID, Error));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactDigestTest,
	"DroneGCS.RouteArtifact.01Digest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactDigestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	FRouteArtifactDigest256 Abc;
	const ANSICHAR* AbcBytes = "abc";
	TestTrue(TEXT("abc SHA-256 computes"),
		FRouteArtifactSha256::HashBytes(AbcBytes, 3, Abc, Error));
	TestEqual(
		TEXT("abc known vector"),
		Abc.ToLowerHex(),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));

	FRouteArtifactDigest256 Empty;
	TestTrue(TEXT("Empty SHA-256 computes"),
		FRouteArtifactSha256::HashBytes(nullptr, 0, Empty, Error));
	TestEqual(
		TEXT("Empty known vector"),
		Empty.ToLowerHex(),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));

	const TArray<uint8> SameBytes{ 0, 1, 2, 3, 4 };
	TArray<uint8> ChangedBytes = SameBytes;
	ChangedBytes.Last() ^= 1;
	FRouteArtifactDigest256 First;
	FRouteArtifactDigest256 Second;
	FRouteArtifactDigest256 Changed;
	TestTrue(TEXT("First bytes hash"), FRouteArtifactSha256::HashBytes(SameBytes, First, Error));
	TestTrue(TEXT("Same bytes hash"), FRouteArtifactSha256::HashBytes(SameBytes, Second, Error));
	TestTrue(TEXT("Changed bytes hash"), FRouteArtifactSha256::HashBytes(ChangedBytes, Changed, Error));
	TestTrue(TEXT("Same bytes produce same digest"), First == Second);
	TestTrue(TEXT("One changed byte changes digest"), First != Changed);
	TestEqual(TEXT("Digest hex length"), First.ToLowerHex().Len(), 64);

	FRouteArtifactDigest256 Parsed;
	TestTrue(TEXT("Lowercase digest parses"),
		FRouteArtifactDigest256::ParseLowerHex(First.ToLowerHex(), Parsed, Error));
	TestTrue(TEXT("Parsed digest round trips"), Parsed == First);
	TestFalse(TEXT("Uppercase digest rejected"),
		FRouteArtifactDigest256::ParseLowerHex(First.ToLowerHex().ToUpper(), Parsed, Error));
	TestFalse(TEXT("Short digest rejected"),
		FRouteArtifactDigest256::ParseLowerHex(First.ToLowerHex().Left(63), Parsed, Error));
	TestFalse(TEXT("Non-hex digest rejected"),
		FRouteArtifactDigest256::ParseLowerHex(FString::ChrN(64, TEXT('g')), Parsed, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactBuildIdentityTest,
	"DroneGCS.RouteArtifact.02BuildIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactBuildIdentityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteArtifactBuildIdentity Baseline = MakeBuildIdentity(*this);
	FString BaselineID;
	if (!ComputeSetID(*this, Baseline, BaselineID))
	{
		return false;
	}
	FString RepeatedID;
	ComputeSetID(*this, Baseline, RepeatedID);
	TestEqual(TEXT("Same identity is deterministic"), RepeatedID, BaselineID);
	TestEqual(TEXT("ArtifactSetID is 64 lowercase hex"), BaselineID.Len(), 64);
	FString PathError;
	TestTrue(TEXT("ArtifactSetID passes path validator"),
		RouteArtifactWorkspacePaths::ValidateArtifactSetID(BaselineID, PathError));

	TArray<uint8> FirstCanonical;
	TArray<uint8> SecondCanonical;
	FString Error;
	TestTrue(TEXT("Canonical serialization succeeds"),
		RouteArtifactIdentity::SerializeCanonicalBuildIdentity(Baseline, FirstCanonical, Error));
	TestTrue(TEXT("Canonical serialization repeats"),
		RouteArtifactIdentity::SerializeCanonicalBuildIdentity(Baseline, SecondCanonical, Error));
	TestTrue(TEXT("Canonical bytes are stable"), FirstCanonical == SecondCanonical);
	TestEqual(
		TEXT("Canonical schema v1 fixture remains stable"),
		BaselineID,
		FString(TEXT("0bc873098d1694796b13a501efecd9203c6b683330eb268d1dad4dac1b7c0ceb")));

	auto ExpectChanged = [this, &BaselineID](
		const TCHAR* FieldName,
		const FRouteArtifactBuildIdentity& Changed)
	{
		FString ChangedID;
		if (ComputeSetID(*this, Changed, ChangedID))
		{
			TestNotEqual(FieldName, ChangedID, BaselineID);
		}
	};

	FRouteArtifactBuildIdentity Changed = Baseline;
	Changed.ArtifactSchemaVersion = 2;
	ExpectChanged(TEXT("ArtifactSchemaVersion changes ID"), Changed);
	Changed = Baseline;
	Changed.AssignmentBinding.FleetSessionID = TEXT("Fleet-Session-B");
	ExpectChanged(TEXT("FleetSessionID changes ID"), Changed);
	Changed = Baseline;
	Changed.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("Authority-Session-B");
	ExpectChanged(TEXT("AuthoritySessionID changes ID"), Changed);
	Changed = Baseline;
	Changed.AssignmentBinding.AssignmentRevision++;
	ExpectChanged(TEXT("AssignmentRevision changes ID"), Changed);
	Changed = Baseline;
	Changed.AssignmentBinding.AssignmentDigest = HashUtf8(*this, "assignment-changed");
	ExpectChanged(TEXT("AssignmentDigest changes ID"), Changed);
	Changed = Baseline;
	Changed.SourceDatasetDigest = HashUtf8(*this, "source-changed");
	ExpectChanged(TEXT("SourceDatasetDigest changes ID"), Changed);
	Changed = Baseline;
	Changed.WaypointGeneratorVersion = TEXT("Generator-Test-v2");
	ExpectChanged(TEXT("WaypointGeneratorVersion changes ID"), Changed);
	Changed = Baseline;
	Changed.GeneratorInputDigest = HashUtf8(*this, "generator-input-changed");
	ExpectChanged(TEXT("GeneratorInputDigest changes ID"), Changed);

	Changed = Baseline;
	Changed.WaypointGeneratorVersion.Reset();
	FString InvalidID;
	TestFalse(TEXT("Empty generator version rejected"),
		RouteArtifactIdentity::ComputeArtifactSetID(Changed, InvalidID, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactPathSafetyTest,
	"DroneGCS.RouteArtifact.03PathSafety",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactPathSafetyTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	FString DevelopmentRoot;
	FString ShippingRoot;
	TestTrue(TEXT("Development root resolves"),
		RouteArtifactWorkspacePaths::ResolveDefaultRoot(
			ERouteArtifactWorkspaceRootKind::Development, DevelopmentRoot, Error));
	TestTrue(TEXT("Development root is absolute"), !FPaths::IsRelative(DevelopmentRoot));
	TestTrue(TEXT("Development root uses Project Saved"),
		DevelopmentRoot.EndsWith(TEXT("Saved/RouteArtifacts")));
	TestTrue(TEXT("Shipping root resolves"),
		RouteArtifactWorkspacePaths::ResolveDefaultRoot(
			ERouteArtifactWorkspaceRootKind::ShippingLocalUser, ShippingRoot, Error));
	TestTrue(TEXT("Shipping root is absolute"), !FPaths::IsRelative(ShippingRoot));
	TestTrue(TEXT("Shipping root has commercial suffix"),
		ShippingRoot.EndsWith(TEXT("DroneGCS/ArtifactWorkspace")));

	FString OverrideRoot = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RouteArtifactRoot"), TEXT(".."), TEXT("Configured"));
	FString NormalizedOverride;
	TestTrue(TEXT("Configured root resolves"),
		RouteArtifactWorkspacePaths::ResolveConfiguredRoot(OverrideRoot, NormalizedOverride, Error));
	TestTrue(TEXT("Configured root collapses traversal"),
		NormalizedOverride.EndsWith(TEXT("Automation/Configured")));
	TestFalse(TEXT("Empty configured root rejected"),
		RouteArtifactWorkspacePaths::ResolveConfiguredRoot(TEXT(""), NormalizedOverride, Error));
	TestFalse(TEXT("Relative configured root rejected"),
		RouteArtifactWorkspacePaths::ResolveConfiguredRoot(TEXT("relative/root"), NormalizedOverride, Error));

	const FString ValidArtifactSetID = FString::ChrN(64, TEXT('a'));
	TestTrue(TEXT("64 lowercase ArtifactSetID accepted"),
		RouteArtifactWorkspacePaths::ValidateArtifactSetID(ValidArtifactSetID, Error));
	TArray<FString> UnsafeArtifactSetIDs = {
		ValidArtifactSetID.Left(63), ValidArtifactSetID + TEXT("a"),
		ValidArtifactSetID.Left(63) + TEXT("A"), ValidArtifactSetID.Left(63) + TEXT("/"),
		ValidArtifactSetID.Left(63) + TEXT("\\"), ValidArtifactSetID.Left(62) + TEXT(".."),
		ValidArtifactSetID.Left(63) + TEXT(":"), ValidArtifactSetID.Left(63) + TEXT(" "),
		ValidArtifactSetID.Left(63) + TEXT("\u822a")
	};
	TCHAR NulArtifactSetIDCharacters[64];
	for (TCHAR& Character : NulArtifactSetIDCharacters)
	{
		Character = TEXT('a');
	}
	NulArtifactSetIDCharacters[31] = TEXT('\0');
	UnsafeArtifactSetIDs.Emplace(
		UE_ARRAY_COUNT(NulArtifactSetIDCharacters), NulArtifactSetIDCharacters);
	for (const FString& UnsafeArtifactSetID : UnsafeArtifactSetIDs)
	{
		TestFalse(TEXT("Unsafe ArtifactSetID rejected"),
			RouteArtifactWorkspacePaths::ValidateArtifactSetID(UnsafeArtifactSetID, Error));
	}

	struct FRoutePathFixture
	{
		int32 Slot;
		const TCHAR* Expected;
	};
	const FRoutePathFixture Fixtures[] = {
		{ 0, TEXT("routes/00000000/0.waypoints") },
		{ 999, TEXT("routes/00000000/999.waypoints") },
		{ 1000, TEXT("routes/00001000/1000.waypoints") },
		{ 10000, TEXT("routes/00010000/10000.waypoints") }
	};
	for (const FRoutePathFixture& Fixture : Fixtures)
	{
		FString RelativePath;
		TestTrue(TEXT("Route path builds"),
			RouteArtifactWorkspacePaths::BuildRouteRelativePath(Fixture.Slot, RelativePath, Error));
		TestEqual(TEXT("Route path matches schema v1"), RelativePath, FString(Fixture.Expected));
		TestTrue(TEXT("Route path validates"),
			RouteArtifactWorkspacePaths::ValidateArtifactRelativePath(RelativePath, Error));
	}
	FString NegativePath;
	TestFalse(TEXT("Negative RouteSlot rejected"),
		RouteArtifactWorkspacePaths::BuildRouteRelativePath(-1, NegativePath, Error));

	const TArray<FString> UnsafePaths = {
		TEXT(""), TEXT("../x"), TEXT("..\\x"), TEXT("routes/../x"),
		TEXT("/routes/x"), TEXT("C:\\x"), TEXT("C:/x"), TEXT("\\\\server\\share"),
		TEXT("\\\\?\\C:\\x"), TEXT("routes//x"), TEXT("routes/./x"),
		TEXT("routes/x:evil"), TEXT("routes/CON/file"), TEXT("routes/file. ")
	};
	for (const FString& UnsafePath : UnsafePaths)
	{
		TestFalse(
			*FString::Printf(TEXT("Unsafe path rejected: %s"), *UnsafePath),
			RouteArtifactWorkspacePaths::ValidateArtifactRelativePath(UnsafePath, Error));
	}
	const TCHAR NulPathCharacters[] = {
		TEXT('r'), TEXT('o'), TEXT('u'), TEXT('t'), TEXT('e'), TEXT('s'), TEXT('/'),
		TEXT('x'), TEXT('\0'), TEXT('/'), TEXT('y')
	};
	const FString NulPath(UE_ARRAY_COUNT(NulPathCharacters), NulPathCharacters);
	TestFalse(TEXT("NUL path rejected"),
		RouteArtifactWorkspacePaths::ValidateArtifactRelativePath(NulPath, Error));

	FString Resolved;
	TestTrue(TEXT("Safe child resolves beneath root"),
		RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
			DevelopmentRoot, TEXT("routes/00000000/0.waypoints"), Resolved, Error));
	TestTrue(TEXT("Resolved path remains under root segment"),
		Resolved.StartsWith(DevelopmentRoot + TEXT("/"), ESearchCase::IgnoreCase));
	TestFalse(TEXT("Traversal cannot resolve beneath root"),
		RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
			DevelopmentRoot, TEXT("../escape"), Resolved, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactTenThousandPathTest,
	"DroneGCS.RouteArtifact.04TenThousandPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactTenThousandPathTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	TSet<FString> Paths;
	Paths.Reserve(10000);
	FString Error;
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < 10000; ++RouteSlotIndex)
	{
		FString RelativePath;
		if (!RouteArtifactWorkspacePaths::BuildRouteRelativePath(
			RouteSlotIndex, RelativePath, Error))
		{
			AddError(FString::Printf(
				TEXT("Route path %d failed: %s"), RouteSlotIndex, *Error));
			return false;
		}
		if (!RouteArtifactWorkspacePaths::ValidateArtifactRelativePath(RelativePath, Error))
		{
			AddError(FString::Printf(
				TEXT("Route path %d is invalid: %s"), RouteSlotIndex, *Error));
			return false;
		}
		if (FPaths::GetCleanFilename(RelativePath)
			!= FString::Printf(TEXT("%d.waypoints"), RouteSlotIndex))
		{
			AddError(FString::Printf(TEXT("Route basename %d is not authoritative."), RouteSlotIndex));
			return false;
		}
		if (Paths.Contains(RelativePath))
		{
			AddError(FString::Printf(TEXT("Duplicate route path at %d."), RouteSlotIndex));
			return false;
		}
		Paths.Add(RelativePath);
	}
	TestEqual(TEXT("All 10K paths are unique"), Paths.Num(), 10000);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
