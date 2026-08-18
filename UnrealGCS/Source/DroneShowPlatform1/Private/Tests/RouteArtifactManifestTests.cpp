#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/PlatformTime.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RouteArtifactManifest.h"
#include "RouteArtifactWorkspacePaths.h"

namespace
{
	FRouteArtifactDigest256 ManifestHashText(
		FAutomationTestBase& Test,
		const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Synthetic Manifest digest computes"),
			FRouteArtifactSha256::HashBytes(
				Utf8.Get(), static_cast<uint64>(Utf8.Length()), Digest, Error));
		return Digest;
	}

	bool FinalizeManifest(
		FAutomationTestBase& Test,
		FRouteArtifactManifest& Manifest)
	{
		FString Error;
		FRouteArtifactBuildIdentity Identity;
		if (!Test.TestTrue(TEXT("Build Identity derives from Manifest"),
			RouteArtifactManifest::BuildIdentityFromManifest(Manifest, Identity, Error))
			|| !Test.TestTrue(TEXT("ArtifactSetID computes from Manifest"),
				RouteArtifactIdentity::ComputeArtifactSetID(
					Identity, Manifest.ArtifactSetID, Error))
			|| !Test.TestTrue(TEXT("ArtifactSetDigest computes"),
				RouteArtifactManifest::ComputeArtifactSetDigest(
					Manifest, Manifest.ArtifactSetDigest, Error)))
		{
			Test.AddError(Error);
			return false;
		}
		return Test.TestTrue(TEXT("Finalized Manifest validates"),
			RouteArtifactManifest::ValidateManifestStructure(Manifest, Error));
	}

	FRouteArtifactManifest MakeManifest(
		FAutomationTestBase& Test,
		const int32 RouteCount = 3,
		const int32 MatchedCount = 3)
	{
		FRouteArtifactManifest Manifest;
		Manifest.CreatedAtUtc = 1700000000123;
		Manifest.AssignmentBinding.FleetSessionID = TEXT("Fleet-Session-Test");
		Manifest.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("Authority-Session-Test");
		Manifest.AssignmentBinding.AssignmentRevision = 9;
		Manifest.AssignmentBinding.AssignmentDigest = ManifestHashText(Test, TEXT("assignment"));
		Manifest.FleetRevisionAtCapture = 12;
		Manifest.RouteDatasetGeneration = 4;
		Manifest.CompletionMode = MatchedCount == RouteCount
			? ERouteAssignmentCompletionMode::Full
			: ERouteAssignmentCompletionMode::Degraded;
		Manifest.FreezePolicy = MatchedCount == RouteCount
			? ERouteAssignmentFreezePolicy::RequireFull
			: ERouteAssignmentFreezePolicy::AllowDegraded;
		Manifest.MatchedCount = MatchedCount;
		Manifest.SpareCount = 1;
		Manifest.ExcludedCount = 2;
		Manifest.VacantCount = RouteCount - MatchedCount;
		Manifest.SourceIdentity.SourceDatasetDigest = ManifestHashText(Test, TEXT("source-npz"));
		Manifest.SourceIdentity.SourceByteSize = 4567;
		Manifest.SourceIdentity.SourceDisplayFileName = TEXT("show-source.npz");
		Manifest.FrameCount = 120;
		Manifest.RouteSlotCount = RouteCount;
		Manifest.WaypointGeneratorVersion = TEXT("test-generator-v1");
		Manifest.GeneratorInputDigest = ManifestHashText(Test, TEXT("generator-input"));

		const FRouteArtifactDigest256 SharedRouteDigest =
			ManifestHashText(Test, TEXT("synthetic-route-bytes"));
		for (int32 RouteSlotIndex = 0; RouteSlotIndex < RouteCount; ++RouteSlotIndex)
		{
			FString PathError;
			FRouteArtifactRecord& Route = Manifest.Routes.AddDefaulted_GetRef();
			Route.RouteSlotIndex = RouteSlotIndex;
			Test.TestTrue(TEXT("Synthetic route path builds"),
				RouteArtifactWorkspacePaths::BuildRouteRelativePath(
					RouteSlotIndex, Route.RelativePath, PathError));
			Route.FileByteSize = 100 + RouteSlotIndex;
			Route.RouteArtifactDigest = SharedRouteDigest;
			if (RouteSlotIndex < MatchedCount)
			{
				FRouteArtifactMatchedMapping& Mapping =
					Manifest.MatchedMapping.AddDefaulted_GetRef();
				Mapping.RouteSlotIndex = RouteSlotIndex;
				Mapping.DroneID = RouteSlotIndex == 0
					? TEXT("\u65e0\u4eba\u673a\"\\\nA")
					: FString::Printf(TEXT("Drone%05d"), RouteSlotIndex);
			}
		}
		FinalizeManifest(Test, Manifest);
		return Manifest;
	}

	FRouteArtifactExpectedInputs MakeExpected(const FRouteArtifactManifest& Manifest)
	{
		FRouteArtifactExpectedInputs Expected;
		FString Error;
		RouteArtifactManifest::BuildIdentityFromManifest(
			Manifest, Expected.BuildIdentity, Error);
		Expected.SourceIdentity = Manifest.SourceIdentity;
		return Expected;
	}

	bool RefreshSetDigest(FRouteArtifactManifest& Manifest)
	{
		FString Error;
		return RouteArtifactManifest::ComputeArtifactSetDigest(
			Manifest, Manifest.ArtifactSetDigest, Error);
	}

	FString Utf8BytesToString(const TArray<uint8>& Bytes)
	{
		FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	TArray<uint8> StringToUtf8Bytes(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactManifestCanonicalTest,
	"DroneGCS.RouteArtifact2C2.01CanonicalManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactManifestCanonicalTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteArtifactManifest Manifest = MakeManifest(*this);
	FString Error;
	TArray<uint8> First;
	TArray<uint8> Second;
	TestTrue(TEXT("First canonical serialization"),
		RouteArtifactManifest::SerializeCanonicalManifest(Manifest, First, Error));
	TestTrue(TEXT("Repeated canonical serialization"),
		RouteArtifactManifest::SerializeCanonicalManifest(Manifest, Second, Error));
	TestTrue(TEXT("Repeated bytes are identical"), First == Second);
	TestTrue(TEXT("Canonical Manifest is non-empty"), !First.IsEmpty());
	TestFalse(TEXT("Canonical Manifest has no UTF-8 BOM"),
		First.Num() >= 3 && First[0] == 0xef && First[1] == 0xbb && First[2] == 0xbf);
	TestFalse(TEXT("Compact Manifest has no raw CR"), First.Contains(static_cast<uint8>('\r')));
	TestFalse(TEXT("Compact Manifest has no raw LF"), First.Contains(static_cast<uint8>('\n')));

	const FString Json = Utf8BytesToString(First);
	TestTrue(TEXT("Field order fixture starts with schema then set ID"),
		Json.StartsWith(TEXT("{\"artifactSchemaVersion\":1,\"artifactSetId\":")));
	TestTrue(TEXT("Assignment precedes audit"),
		Json.Find(TEXT("\"assignment\":")) < Json.Find(TEXT("\"assignmentAudit\":")));
	TestTrue(TEXT("Mapping precedes routes"),
		Json.Find(TEXT("\"mapping\":")) < Json.Find(TEXT("\"routes\":")));
	TestTrue(TEXT("ArtifactSetDigest is final protected output field"),
		Json.Find(TEXT("\"artifactSetDigest\":")) > Json.Find(TEXT("\"routes\":")));
	TestTrue(TEXT("Absent range has explicit boolean and null values"),
		Json.Contains(TEXT("\"range\":{\"hasRange\":false,\"start\":null,\"end\":null}")));
	TestTrue(TEXT("UTF-8 DroneID retained"), Json.Contains(TEXT("\u65e0\u4eba\u673a")));
	TestTrue(TEXT("Quote escape is deterministic"), Json.Contains(TEXT("\\\"")));
	TestTrue(TEXT("Backslash escape is deterministic"), Json.Contains(TEXT("\\\\")));
	TestTrue(TEXT("Newline escape is deterministic"), Json.Contains(TEXT("\\n")));

	FRouteArtifactManifest Parsed;
	TestTrue(TEXT("Canonical parse succeeds"),
		RouteArtifactManifest::ParseCanonicalManifest(First, Parsed, Error));
	TArray<uint8> RoundTrip;
	TestTrue(TEXT("Parsed Manifest reserializes"),
		RouteArtifactManifest::SerializeCanonicalManifest(Parsed, RoundTrip, Error));
	TestTrue(TEXT("Parse serialize is byte-identical"), RoundTrip == First);
	TestEqual(TEXT("UTF-8/escaped DroneID round trips"),
		Parsed.MatchedMapping[0].DroneID, Manifest.MatchedMapping[0].DroneID);

	FString NonCanonical = Json;
	NonCanonical.InsertAt(1, TEXT(" "));
	TestFalse(TEXT("Noncanonical whitespace rejected"),
		RouteArtifactManifest::ParseCanonicalManifest(
			StringToUtf8Bytes(NonCanonical), Parsed, Error));
	FString Unknown = Json;
	Unknown.InsertAt(1, TEXT("\"unknown\":1,"));
	TestFalse(TEXT("Unknown fields rejected"),
		RouteArtifactManifest::ParseCanonicalManifest(StringToUtf8Bytes(Unknown), Parsed, Error));
	FString Duplicate = Json;
	Duplicate.InsertAt(1, TEXT("\"artifactSchemaVersion\":1,"));
	TestFalse(TEXT("Duplicate logical fields rejected"),
		RouteArtifactManifest::ParseCanonicalManifest(StringToUtf8Bytes(Duplicate), Parsed, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactManifestDigestTest,
	"DroneGCS.RouteArtifact2C2.02ArtifactSetDigest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactManifestDigestTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteArtifactManifest Baseline = MakeManifest(*this);
	FString Error;
	FRouteArtifactDigest256 Repeated;
	TestTrue(TEXT("Digest recomputes"),
		RouteArtifactManifest::ComputeArtifactSetDigest(Baseline, Repeated, Error));
	TestTrue(TEXT("Baseline digest deterministic"), Repeated == Baseline.ArtifactSetDigest);

	auto ExpectDigestChanged = [this, &Baseline](
		const TCHAR* Name, FRouteArtifactManifest Changed)
	{
		FString LocalError;
		FRouteArtifactDigest256 ChangedDigest;
		if (TestTrue(Name, RouteArtifactManifest::ComputeArtifactSetDigest(
			Changed, ChangedDigest, LocalError)))
		{
			TestTrue(*FString::Printf(TEXT("%s changes digest"), Name),
				ChangedDigest != Baseline.ArtifactSetDigest);
		}
	};

	FRouteArtifactManifest Changed = Baseline;
	Changed.CreatedAtUtc++;
	ExpectDigestChanged(TEXT("CreatedAtUtc"), Changed);
	Changed = Baseline;
	Changed.AssignmentBinding.AssignmentDigest = ManifestHashText(*this, TEXT("assignment-2"));
	ExpectDigestChanged(TEXT("AssignmentDigest"), Changed);
	Changed = Baseline;
	Changed.SourceIdentity.SourceDatasetDigest = ManifestHashText(*this, TEXT("source-2"));
	ExpectDigestChanged(TEXT("SourceDatasetDigest"), Changed);
	Changed = Baseline;
	Changed.WaypointGeneratorVersion = TEXT("test-generator-v2");
	ExpectDigestChanged(TEXT("GeneratorVersion"), Changed);
	Changed = Baseline;
	Changed.GeneratorInputDigest = ManifestHashText(*this, TEXT("generator-input-2"));
	ExpectDigestChanged(TEXT("GeneratorInputDigest"), Changed);
	Changed = Baseline;
	Changed.MatchedMapping[0].DroneID += TEXT("-changed");
	ExpectDigestChanged(TEXT("Mapping"), Changed);
	Changed = Baseline;
	Changed.Routes[0].RelativePath = TEXT("routes/00000000/different.waypoints");
	ExpectDigestChanged(TEXT("RoutePath"), Changed);
	Changed = Baseline;
	Changed.Routes[0].FileByteSize++;
	ExpectDigestChanged(TEXT("FileSize"), Changed);
	Changed = Baseline;
	Changed.Routes[0].RouteArtifactDigest = ManifestHashText(*this, TEXT("route-2"));
	ExpectDigestChanged(TEXT("RouteArtifactDigest"), Changed);

	Changed = Baseline;
	Changed.ArtifactSetDigest = ManifestHashText(*this, TEXT("wrong-stored-digest"));
	FRouteArtifactDigest256 SelfExcluded;
	TestTrue(TEXT("Stored digest is excluded from its preimage"),
		RouteArtifactManifest::ComputeArtifactSetDigest(Changed, SelfExcluded, Error));
	TestTrue(TEXT("Changing stored digest does not change recomputation"),
		SelfExcluded == Baseline.ArtifactSetDigest);
	TestFalse(TEXT("Wrong stored digest rejected"),
		RouteArtifactManifest::ValidateManifestStructure(Changed, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactManifestValidationTest,
	"DroneGCS.RouteArtifact2C2.03MappingRoutesBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactManifestValidationTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString Error;
	const FRouteArtifactManifest Full = MakeManifest(*this, 3, 3);
	const FRouteArtifactManifest Degraded = MakeManifest(*this, 3, 2);
	TestTrue(TEXT("Full 3-route Manifest validates"),
		RouteArtifactManifest::ValidateManifestStructure(Full, Error));
	TestTrue(TEXT("Degraded 3-route Manifest validates"),
		RouteArtifactManifest::ValidateManifestStructure(Degraded, Error));
	TestEqual(TEXT("Degraded still covers every route"), Degraded.Routes.Num(), 3);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestEqual(TEXT("Route domain is continuous"), Degraded.Routes[Index].RouteSlotIndex, Index);
	}

	auto ExpectInvalid = [this](const TCHAR* Name, FRouteArtifactManifest Changed)
	{
		RefreshSetDigest(Changed);
		FString LocalError;
		TestFalse(Name, RouteArtifactManifest::ValidateManifestStructure(Changed, LocalError));
	};

	FRouteArtifactManifest Changed = Full;
	Changed.MatchedMapping[1].DroneID = Changed.MatchedMapping[0].DroneID;
	ExpectInvalid(TEXT("Duplicate DroneID rejected"), Changed);
	Changed = Full;
	Changed.MatchedMapping[1].RouteSlotIndex = Changed.MatchedMapping[0].RouteSlotIndex;
	ExpectInvalid(TEXT("Duplicate matched RouteSlot rejected"), Changed);
	Changed = Full;
	Changed.MatchedMapping[2].RouteSlotIndex = 3;
	ExpectInvalid(TEXT("Out-of-range mapping rejected"), Changed);
	Changed = Full;
	Changed.MatchedMapping[0].DroneID.Reset();
	ExpectInvalid(TEXT("Empty DroneID rejected"), Changed);
	Changed = Full;
	Changed.MatchedCount = 2;
	ExpectInvalid(TEXT("Matched count mismatch rejected"), Changed);
	Changed = Full;
	Swap(Changed.MatchedMapping[0], Changed.MatchedMapping[1]);
	ExpectInvalid(TEXT("Noncanonical mapping order rejected"), Changed);

	Changed = Full;
	Changed.Routes.RemoveAt(1);
	ExpectInvalid(TEXT("Missing route slot rejected"), Changed);
	Changed = Full;
	Changed.Routes[1].RouteSlotIndex = 0;
	ExpectInvalid(TEXT("Duplicate route slot rejected"), Changed);
	Changed = Full;
	Changed.Routes[1].RouteSlotIndex = 2;
	ExpectInvalid(TEXT("Skipped route slot rejected"), Changed);
	Changed = Full;
	Changed.Routes[1].RelativePath = Changed.Routes[0].RelativePath;
	ExpectInvalid(TEXT("Mismatched route relative path rejected"), Changed);
	Changed = Full;
	Changed.Routes[1].FileByteSize = -1;
	ExpectInvalid(TEXT("Negative route size rejected"), Changed);
	Changed = Full;
	Changed.Routes[1].RouteArtifactDigest.Reset();
	FString InvalidDigestError;
	TestFalse(TEXT("Invalid route digest rejected"),
		RouteArtifactManifest::ValidateManifestStructure(Changed, InvalidDigestError));

	const FRouteArtifactExpectedInputs Expected = MakeExpected(Full);
	TestTrue(TEXT("Expected Assignment/Source/Build binding passes"),
		RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(Full, Expected, Error));
	FRouteArtifactExpectedInputs WrongExpected = Expected;
	WrongExpected.BuildIdentity.AssignmentBinding.AssignmentRevision++;
	TestFalse(TEXT("Wrong expected AssignmentBinding rejected"),
		RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(Full, WrongExpected, Error));
	WrongExpected = Expected;
	WrongExpected.SourceIdentity.SourceDatasetDigest = ManifestHashText(*this, TEXT("wrong-source"));
	TestFalse(TEXT("Wrong expected SourceDatasetDigest rejected"),
		RouteArtifactManifest::ValidateManifestAgainstExpectedInputs(Full, WrongExpected, Error));
	Changed = Full;
	Changed.ArtifactSetID = FString::ChrN(64, TEXT('f'));
	TestFalse(TEXT("Valid-shaped but mismatched ArtifactSetID rejected"),
		RouteArtifactManifest::ValidateManifestStructure(Changed, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactManifestMalformedJsonTest,
	"DroneGCS.RouteArtifact2C2.04MalformedJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactManifestMalformedJsonTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FRouteArtifactManifest Manifest = MakeManifest(*this);
	FString Error;
	TArray<uint8> Canonical;
	TestTrue(TEXT("Malformed baseline serializes"),
		RouteArtifactManifest::SerializeCanonicalManifest(Manifest, Canonical, Error));
	const FString Json = Utf8BytesToString(Canonical);
	FRouteArtifactManifest Parsed;
	auto RejectText = [this, &Parsed](const TCHAR* Name, const FString& Text)
	{
		FString LocalError;
		TestFalse(Name, RouteArtifactManifest::ParseCanonicalManifest(
			StringToUtf8Bytes(Text), Parsed, LocalError));
	};

	TArray<uint8> Truncated = Canonical;
	Truncated.Pop();
	TestFalse(TEXT("Truncated JSON rejected"),
		RouteArtifactManifest::ParseCanonicalManifest(Truncated, Parsed, Error));
	FString Changed = Json;
	Changed.ReplaceInline(TEXT("\"matchedCount\":3"), TEXT("\"matchedCount\":\"3\""));
	RejectText(TEXT("Wrong field type rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(TEXT("\"spareCount\":1,"), TEXT(""));
	RejectText(TEXT("Missing required field rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(*Manifest.AssignmentBinding.AssignmentDigest.ToLowerHex(), TEXT("abc"));
	RejectText(TEXT("Invalid digest length rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(*Manifest.ArtifactSetID, TEXT("not-an-id"));
	RejectText(TEXT("Invalid ArtifactSetID rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(TEXT("1700000000123"), TEXT("9223372036854775808"));
	RejectText(TEXT("Integer overflow rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(TEXT("\"spareCount\":1"), TEXT("\"spareCount\":-1"));
	RejectText(TEXT("Negative count rejected"), Changed);
	Changed = Json;
	Changed.ReplaceInline(TEXT("\"artifactSchemaVersion\":1"),
		TEXT("\"artifactSchemaVersion\":2"));
	RejectText(TEXT("Unsupported schema rejected"), Changed);
	Changed = Json + TEXT("x");
	RejectText(TEXT("Trailing semantic garbage rejected"), Changed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactManifestTenThousandTest,
	"DroneGCS.RouteArtifact2C2.05TenThousandManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactManifestTenThousandTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FRouteArtifactManifest Manifest = MakeManifest(*this, 10000, 10000);
	FString Error;
	TArray<uint8> Bytes;
	const double SerializeStart = FPlatformTime::Seconds();
	const bool bSerialized = RouteArtifactManifest::SerializeCanonicalManifest(Manifest, Bytes, Error);
	const double SerializeMs = (FPlatformTime::Seconds() - SerializeStart) * 1000.0;
	TestTrue(TEXT("10K Manifest serializes"), bSerialized);

	FRouteArtifactManifest Parsed;
	const double ParseStart = FPlatformTime::Seconds();
	const bool bParsed = RouteArtifactManifest::ParseCanonicalManifest(Bytes, Parsed, Error);
	const double ParseMs = (FPlatformTime::Seconds() - ParseStart) * 1000.0;
	TestTrue(TEXT("10K Manifest parses"), bParsed);

	const double ValidateStart = FPlatformTime::Seconds();
	const bool bValidated = RouteArtifactManifest::ValidateManifestStructure(Parsed, Error);
	const double ValidateMs = (FPlatformTime::Seconds() - ValidateStart) * 1000.0;
	TestTrue(TEXT("10K Manifest validates"), bValidated);
	TestEqual(TEXT("10K mapping retained"), Parsed.MatchedMapping.Num(), 10000);
	TestEqual(TEXT("10K route domain retained"), Parsed.Routes.Num(), 10000);
	AddInfo(FString::Printf(
		TEXT("MANIFEST10K bytes=%d serializeMs=%.3f parseMs=%.3f validateMs=%.3f"),
		Bytes.Num(), SerializeMs, ParseMs, ValidateMs));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactStreamingHashTest,
	"DroneGCS.RouteArtifact2C2.06StreamingFileHash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactStreamingHashTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Directory = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RouteArtifact2C2Streaming"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString FilePath = FPaths::Combine(Directory, TEXT("streaming.waypoints"));
	FString Error;
	auto HashFile = [this, &FilePath, &Error](
		const TCHAR* Name,
		const TArray<uint8>& Bytes,
		FRouteArtifactDigest256& OutDigest,
		int64& OutSize)
	{
		if (!TestTrue(TEXT("Synthetic streaming file writes"),
			FFileHelper::SaveArrayToFile(Bytes, *FilePath))) return false;
		return TestTrue(Name, FRouteArtifactSha256::HashFileStreaming(
			FilePath, OutDigest, OutSize, Error));
	};

	TArray<uint8> Empty;
	FRouteArtifactDigest256 EmptyDigest;
	int64 EmptySize = -1;
	HashFile(TEXT("Empty file hashes"), Empty, EmptyDigest, EmptySize);
	TestEqual(TEXT("Empty file size is zero"), EmptySize, static_cast<int64>(0));
	TestEqual(TEXT("Empty file known digest"), EmptyDigest.ToLowerHex(),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));

	const TArray<uint8> Small{ 'Q', 'G', 'C', ' ', 'W', 'P', 'L', ' ', '1', '1', '0', '\n' };
	FRouteArtifactDigest256 SmallDigest;
	int64 SmallSize = 0;
	HashFile(TEXT("Small file hashes"), Small, SmallDigest, SmallSize);
	FRouteArtifactDigest256 SmallMemoryDigest;
	TestTrue(TEXT("Small memory hash computes"),
		FRouteArtifactSha256::HashBytes(Small, SmallMemoryDigest, Error));
	TestTrue(TEXT("Streaming equals memory digest"), SmallDigest == SmallMemoryDigest);
	TestEqual(TEXT("Small file byte size"), SmallSize, static_cast<int64>(Small.Num()));

	TArray<uint8> MultiBuffer;
	MultiBuffer.SetNumUninitialized(FRouteArtifactSha256::StreamingBufferBytes * 3 + 17);
	for (int32 Index = 0; Index < MultiBuffer.Num(); ++Index)
	{
		MultiBuffer[Index] = static_cast<uint8>((Index * 31) & 0xff);
	}
	FRouteArtifactDigest256 MultiDigest;
	int64 MultiSize = 0;
	HashFile(TEXT("Multi-buffer file hashes"), MultiBuffer, MultiDigest, MultiSize);
	TestEqual(TEXT("Multi-buffer actual size"), MultiSize, static_cast<int64>(MultiBuffer.Num()));

	TArray<uint8> Changed = MultiBuffer;
	Changed[Changed.Num() / 2] ^= 1;
	FRouteArtifactDigest256 ChangedDigest;
	int64 ChangedSize = 0;
	HashFile(TEXT("One-byte modified file hashes"), Changed, ChangedDigest, ChangedSize);
	TestTrue(TEXT("One-byte modification changes digest"), ChangedDigest != MultiDigest);
	TestEqual(TEXT("One-byte modification keeps size"), ChangedSize, MultiSize);

	Changed = MultiBuffer;
	Changed.Pop();
	FRouteArtifactDigest256 TruncatedDigest;
	int64 TruncatedSize = 0;
	HashFile(TEXT("Truncated file hashes"), Changed, TruncatedDigest, TruncatedSize);
	TestEqual(TEXT("Truncate changes size"), TruncatedSize, MultiSize - 1);
	TestTrue(TEXT("Truncate changes digest"), TruncatedDigest != MultiDigest);

	Changed = MultiBuffer;
	Changed.Add(0x42);
	FRouteArtifactDigest256 AppendedDigest;
	int64 AppendedSize = 0;
	HashFile(TEXT("Appended file hashes"), Changed, AppendedDigest, AppendedSize);
	TestEqual(TEXT("Append changes size"), AppendedSize, MultiSize + 1);
	TestTrue(TEXT("Append changes digest"), AppendedDigest != MultiDigest);

	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteArtifactFileIntegrityTest,
	"DroneGCS.RouteArtifact2C2.07FileIntegrity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteArtifactFileIntegrityTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	const FString Root = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("RouteArtifact2C2Integrity"));
	IFileManager::Get().MakeDirectory(*Root, true);
	FRouteArtifactManifest Manifest = MakeManifest(*this, 3, 3);
	TArray<TArray<uint8>> RouteBytes;
	RouteBytes.SetNum(3);
	FString Error;
	for (int32 RouteSlotIndex = 0; RouteSlotIndex < 3; ++RouteSlotIndex)
	{
		const FString Text = FString::Printf(
			TEXT("QGC WPL synthetic route %d\n"), RouteSlotIndex);
		FTCHARToUTF8 Utf8(*Text);
		RouteBytes[RouteSlotIndex].Append(
			reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		FString Resolved;
		TestTrue(TEXT("Integrity route resolves"),
			RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
				Root, Manifest.Routes[RouteSlotIndex].RelativePath, Resolved, Error));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Resolved), true);
		TestTrue(TEXT("Integrity route writes"),
			FFileHelper::SaveArrayToFile(RouteBytes[RouteSlotIndex], *Resolved));
		TestTrue(TEXT("Integrity route hashes"),
			FRouteArtifactSha256::HashFileStreaming(
				Resolved,
				Manifest.Routes[RouteSlotIndex].RouteArtifactDigest,
				Manifest.Routes[RouteSlotIndex].FileByteSize,
				Error));
	}
	TestTrue(TEXT("Integrity Manifest digest refreshes"), RefreshSetDigest(Manifest));
	TestTrue(TEXT("Correct Artifact files pass"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Manifest, Error));

	FString RouteOnePath;
	RouteArtifactWorkspacePaths::ResolvePathUnderRoot(
		Root, Manifest.Routes[1].RelativePath, RouteOnePath, Error);
	IFileManager::Get().Delete(*RouteOnePath, false, true, true);
	TestFalse(TEXT("Missing route file rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Manifest, Error));
	FFileHelper::SaveArrayToFile(RouteBytes[1], *RouteOnePath);

	TArray<uint8> Corrupt = RouteBytes[1];
	Corrupt[0] ^= 1;
	FFileHelper::SaveArrayToFile(Corrupt, *RouteOnePath);
	TestFalse(TEXT("Byte corruption rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Manifest, Error));
	FFileHelper::SaveArrayToFile(RouteBytes[1], *RouteOnePath);

	TArray<uint8> Truncated = RouteBytes[1];
	Truncated.Pop();
	FFileHelper::SaveArrayToFile(Truncated, *RouteOnePath);
	TestFalse(TEXT("Truncated route rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Manifest, Error));
	FFileHelper::SaveArrayToFile(RouteBytes[1], *RouteOnePath);

	TArray<uint8> Appended = RouteBytes[1];
	Appended.Add('\n');
	FFileHelper::SaveArrayToFile(Appended, *RouteOnePath);
	TestFalse(TEXT("Appended route rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Manifest, Error));
	FFileHelper::SaveArrayToFile(RouteBytes[1], *RouteOnePath);

	FRouteArtifactManifest Wrong = Manifest;
	Wrong.Routes[1].RouteArtifactDigest = ManifestHashText(*this, TEXT("wrong-file"));
	RefreshSetDigest(Wrong);
	TestFalse(TEXT("Wrong Manifest route digest rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Wrong, Error));
	Wrong = Manifest;
	Wrong.Routes[1].FileByteSize++;
	RefreshSetDigest(Wrong);
	TestFalse(TEXT("Wrong Manifest route size rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Wrong, Error));
	Wrong = Manifest;
	Wrong.Routes[1].RelativePath = TEXT("../escape.waypoints");
	RefreshSetDigest(Wrong);
	TestFalse(TEXT("Unsafe Manifest relative path rejected"),
		RouteArtifactManifest::ValidateArtifactFiles(Root, Wrong, Error));

	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
