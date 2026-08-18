#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Math/Float16Color.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteArtifactWorkspaceManager.h"
#include "RouteWaypointGenerator.h"

#include <limits>

namespace
{
	constexpr TCHAR GoldenRoute0[] =
		TEXT("QGC WPL 110\r\n")
		TEXT("0\t0\t3\t16\t148\t26\t216\t0\t31.932471\t118.638531\t-22.2241935\t1\r\n")
		TEXT("1\t0\t3\t16\t0\t0\t0\t0\t31.9325611\t118.6385838\t0.0\t1\r\n")
		TEXT("2\t0\t3\t16\t229\t2\t191\t0\t31.9324349\t118.6384992\t7.75\t1");
	constexpr TCHAR GoldenRoute1[] =
		TEXT("QGC WPL 110\r\n")
		TEXT("0\t0\t3\t16\t0\t255\t127\t0\t31.9324056\t118.6386631\t0.0\t1\r\n")
		TEXT("1\t0\t3\t16\t255\t255\t255\t0\t31.9325431\t118.638404\t-1.5\t1\r\n")
		TEXT("2\t0\t3\t16\t127\t127\t31\t0\t31.9326964\t118.6385309\t-0.0000001\t1");
	constexpr TCHAR GoldenRoute2[] =
		TEXT("QGC WPL 110\r\n")
		TEXT("0\t0\t3\t16\t255\t0\t63\t0\t31.9326062\t118.6382666\t3.125\t1\r\n")
		TEXT("1\t0\t3\t16\t25\t50\t76\t0\t31.9322906\t118.6390597\t20.0\t1\r\n")
		TEXT("2\t0\t3\t16\t255\t127\t0\t0\t31.9333728\t118.6395885\t0.0\t1");

	TArray<uint8> AsciiBytes(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	FRouteArtifactDigest256 HashText(FAutomationTestBase& Test, const FString& Text)
	{
		const TArray<uint8> Bytes = AsciiBytes(Text);
		FRouteArtifactDigest256 Digest;
		FString Error;
		Test.TestTrue(TEXT("Synthetic SHA-256"),
			FRouteArtifactSha256::HashBytes(Bytes, Digest, Error));
		return Digest;
	}

	FRouteArtifactSourceIdentity MakeSource(FAutomationTestBase& Test)
	{
		FRouteArtifactSourceIdentity Source;
		FString Error;
		Test.TestTrue(TEXT("Synthetic source identity"),
			FRouteArtifactSourceIdentity::Create(
				HashText(Test, TEXT("2d-c1-source")), 98765,
				TEXT("synthetic-golden.npz"), Source, Error));
		return Source;
	}

	FRoutePreviewData MakeGoldenData(FAutomationTestBase& Test)
	{
		FRoutePreviewData Data;
		Data.FrameCount = 3;
		Data.DroneCount = 3;
		Data.SampleRateHz = 50.0;
		Data.OptionalRangeStart = 1;
		Data.OptionalRangeEnd = 1;
		Data.SourceIdentity = MakeSource(Test);
		Data.PositionsENU = {
			FVector3f(0.0f, 0.0f, -22.224193572998047f),
			FVector3f(12.5f, -7.25f, 0.0f),
			FVector3f(-25.0f, 15.0f, 3.125f),
			FVector3f(5.0f, 10.0f, 0.0f),
			FVector3f(-12.0f, 8.0f, -1.5f),
			FVector3f(50.0f, -20.0f, 20.0f),
			FVector3f(-3.0f, -4.0f, 7.75f),
			FVector3f(0.0f, 25.0f, -0.0000001f),
			FVector3f(100.0f, 100.0f, 0.0f)
		};
		Data.Colors = {
			FFloat16Color(FLinearColor(148.5f / 255.0f, 26.5f / 255.0f, 216.5f / 255.0f, 1.0f)),
			FFloat16Color(FLinearColor(0.0f, 1.0f, 0.5f, 1.0f)),
			FFloat16Color(FLinearColor(1.0f, 0.0f, 0.25f, 1.0f)),
			FFloat16Color(FLinearColor(0.0f, 0.0f, 0.0f, 1.0f)),
			FFloat16Color(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)),
			FFloat16Color(FLinearColor(0.1f, 0.2f, 0.3f, 1.0f)),
			FFloat16Color(FLinearColor(0.9f, 0.01f, 0.75f, 1.0f)),
			FFloat16Color(FLinearColor(0.501f, 0.499f, 0.125f, 1.0f)),
			FFloat16Color(FLinearColor(1.0f, 0.5f, 0.0f, 1.0f))
		};
		return Data;
	}

	FRouteWaypointGeneratorContext MakeContext(FAutomationTestBase& Test)
	{
		FRouteWaypointGeneratorContext Context;
		FRouteWaypointGeneratorError Error;
		Test.TestTrue(TEXT("Build normalized generator context"),
			RouteWaypointGenerator::BuildContext(
				31.932471, 118.638531, Context, Error));
		return Context;
	}

	class FMemorySink final : public IRouteWaypointByteSink
	{
	public:
		bool Write(const uint8* Data, const int32 ByteCount, FString& OutError) override
		{
			OutError.Reset();
			++WriteCalls;
			if (FailWriteCall == WriteCalls)
			{
				OutError = TEXT("Injected write failure.");
				return false;
			}
			Bytes.Append(Data, ByteCount);
			return true;
		}

		bool Finalize(FString& OutError) override
		{
			OutError.Reset();
			bFinalizeCalled = true;
			if (bFailFinalize)
			{
				OutError = TEXT("Injected finalize failure.");
				return false;
			}
			return true;
		}

		TArray<uint8> Bytes;
		int32 WriteCalls = 0;
		int32 FailWriteCall = INDEX_NONE;
		bool bFailFinalize = false;
		bool bFinalizeCalled = false;
	};

	FRouteWaypointGenerationResult GenerateToMemory(
		const FRoutePreviewData& Data,
		const FRouteWaypointGeneratorContext& Context,
		const int32 RouteSlot,
		FMemorySink& Sink,
		const TFunction<bool()>& IsCancelled = TFunction<bool()>())
	{
		FRouteWaypointGeneratorInput Input;
		Input.RouteData = &Data;
		Input.RouteSlotIndex = RouteSlot;
		Input.SourceIdentity = Data.SourceIdentity;
		Input.Context = Context;
		return RouteWaypointGenerator::GenerateRoute(Input, Sink, IsCancelled);
	}

	bool TestDigestHex(
		FAutomationTestBase& Test,
		const TArray<uint8>& Bytes,
		const TCHAR* ExpectedHex)
	{
		FRouteArtifactDigest256 Digest;
		FString Error;
		return Test.TestTrue(TEXT("Golden hash computed by Artifact SHA-256"),
			FRouteArtifactSha256::HashBytes(Bytes, Digest, Error))
			&& Test.TestEqual(TEXT("Golden SHA-256"), Digest.ToLowerHex(), FString(ExpectedHex));
	}

	FString MakeTempRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteWaypointGeneratorTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointGeoAndContractTests,
	"DroneGCS.RouteWaypointGenerator.2DC1.GeoAndContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointGeoAndContractTests::RunTest(const FString& Parameters)
{
	FRouteWaypointGeneratorContext Context = MakeContext(*this);
	TestEqual(TEXT("Generator version is frozen v1"), Context.WaypointGeneratorVersion,
		FString(TEXT("qgc-wpl110-wgs84-relative-rgb-v1")));
	TestEqual(TEXT("Normalized latitude E7"), Context.NormalizedShowOriginLatitudeE7, 319324710);
	TestEqual(TEXT("Normalized longitude E7"), Context.NormalizedShowOriginLongitudeE7, 1186385310);
	TestEqual(TEXT("Generator input digest v1"), Context.GeneratorInputDigest.ToLowerHex(),
		FString(TEXT("4c8f8d4f7345123408f022e500d814bda4ecf2a4bf00ce34cc7f2a6f9b32ceec")));

	FRouteWaypointGeneratorError Error;
	FRouteWaypointGeneratorContext SameBucket;
	TestTrue(TEXT("Same E7 bucket context"), RouteWaypointGenerator::BuildContext(
		31.93247101, 118.63853109, SameBucket, Error));
	TestTrue(TEXT("Same E7 bucket digest"),
		SameBucket.GeneratorInputDigest == Context.GeneratorInputDigest);
	TestEqual(TEXT("Same normalized latitude"),
		SameBucket.GeoOrigin.LatitudeDegrees, Context.GeoOrigin.LatitudeDegrees);
	FRouteWaypointGeneratorContext NextBucket;
	TestTrue(TEXT("Next E7 bucket context"), RouteWaypointGenerator::BuildContext(
		31.93247111, 118.638531, NextBucket, Error));
	TestTrue(TEXT("Crossing E7 boundary changes digest"),
		NextBucket.GeneratorInputDigest != Context.GeneratorInputDigest);

	const double InvalidOrigins[][2] = {
		{std::numeric_limits<double>::quiet_NaN(), 0.0},
		{std::numeric_limits<double>::infinity(), 0.0},
		{std::numeric_limits<double>::max(), 0.0},
		{90.0000001, 0.0}, {-90.0000001, 0.0},
		{0.0, 180.0000001}, {0.0, -180.0000001}
	};
	for (const auto& Origin : InvalidOrigins)
	{
		FRouteWaypointGeneratorContext Invalid;
		TestFalse(TEXT("Invalid origin rejected"), RouteWaypointGenerator::BuildContext(
			Origin[0], Origin[1], Invalid, Error));
		TestEqual(TEXT("Invalid origin code"), Error.Code,
			ERouteWaypointGeneratorErrorCode::InvalidOrigin);
	}
	for (const TPair<double, double>& Boundary : {
		TPair<double, double>(0.0, 0.0),
		TPair<double, double>(90.0, 180.0),
		TPair<double, double>(-90.0, -180.0)})
	{
		FRouteWaypointGeneratorContext Valid;
		TestTrue(TEXT("Origin boundary accepted"), RouteWaypointGenerator::BuildContext(
			Boundary.Key, Boundary.Value, Valid, Error));
	}

	TestEqual(TEXT("Decimal zero"), RouteWaypointGenerator::FormatScaledDecimal(0), FString(TEXT("0.0")));
	TestEqual(TEXT("Decimal trailing zeros"), RouteWaypointGenerator::FormatScaledDecimal(281468520), FString(TEXT("28.146852")));
	TestEqual(TEXT("Decimal negative integer"), RouteWaypointGenerator::FormatScaledDecimal(-180000000), FString(TEXT("-18.0")));
	TestEqual(TEXT("Decimal positive quantum"), RouteWaypointGenerator::FormatScaledDecimal(1), FString(TEXT("0.0000001")));
	TestEqual(TEXT("Decimal negative quantum"), RouteWaypointGenerator::FormatScaledDecimal(-1), FString(TEXT("-0.0000001")));
	TestEqual(TEXT("Decimal INT64_MIN"), RouteWaypointGenerator::FormatScaledDecimal(MIN_int64),
		FString(TEXT("-922337203685.4775808")));

	const struct
	{
		double Latitude;
		double Longitude;
		double Height;
	} RoundTrips[] = {
		{0.0, 0.0, 0.0},
		{31.932471, 118.638531, 123.456},
		{-33.8688, 151.2093, -25.0},
		{64.1466, -21.9426, 2500.0},
		{10.0, 180.0, 0.0},
		{-10.0, -180.0, 100.0},
		{89.999999, 45.0, 10.0}
	};
	for (const auto& Point : RoundTrips)
	{
		const RouteAssignmentGeo::FEcefPosition Ecef = RouteAssignmentGeo::GeodeticToEcef(
			Point.Latitude, Point.Longitude, Point.Height);
		RouteAssignmentGeo::FGeodeticPosition Geodetic;
		FString GeoError;
		TestTrue(TEXT("Global WGS84 ECEF roundtrip"),
			RouteAssignmentGeo::EcefToGeodetic(Ecef, Geodetic, GeoError));
		TestTrue(TEXT("Roundtrip latitude tolerance"),
			FMath::Abs(Geodetic.LatitudeDegrees - Point.Latitude) < 1.0e-8);
		const double LongitudeDelta = FMath::Abs(
			FMath::Fmod(Geodetic.LongitudeDegrees - Point.Longitude + 540.0, 360.0) - 180.0);
		TestTrue(TEXT("Roundtrip longitude/wrap tolerance"), LongitudeDelta < 1.0e-8);
		TestTrue(TEXT("Roundtrip height tolerance"),
			FMath::Abs(Geodetic.EllipsoidHeightMeters - Point.Height) < 1.0e-3);
	}
	RouteAssignmentGeo::FGeodeticPosition Degenerate;
	FString GeoError;
	TestFalse(TEXT("Earth-center ECEF rejected"), RouteAssignmentGeo::EcefToGeodetic(
		RouteAssignmentGeo::FEcefPosition{}, Degenerate, GeoError));

	const struct
	{
		double East;
		double North;
		double ExpectedLatitude;
		double ExpectedLongitude;
	} EnuCases[] = {
		{0.0, 0.0, 31.932471, 118.638531},
		{100.0, 0.0, 31.932470995598067, 118.63958850352977},
		{0.0, 100.0, 31.933372830060108, 118.63853099999999},
		{123.456, 789.012, 31.939586537161915, 118.63983665212562},
		{-250.25, -400.5, 31.928859141765532, 118.63588470088249}
	};
	for (const auto& Case : EnuCases)
	{
		RouteAssignmentGeo::FGeodeticPosition Geodetic;
		TestTrue(TEXT("Independent horizontal ENU fixture"),
			RouteAssignmentGeo::LocalEnuToGeodetic(
				Context.GeoOrigin, Case.East, Case.North, 0.0, Geodetic, GeoError));
		TestTrue(TEXT("ENU latitude fixture tolerance"),
			FMath::Abs(Geodetic.LatitudeDegrees - Case.ExpectedLatitude) < 1.0e-10);
		TestTrue(TEXT("ENU longitude fixture tolerance"),
			FMath::Abs(Geodetic.LongitudeDegrees - Case.ExpectedLongitude) < 1.0e-10);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointGoldenTests,
	"DroneGCS.RouteWaypointGenerator.2DC1.GoldenFiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointGoldenTests::RunTest(const FString& Parameters)
{
	const FRoutePreviewData Data = MakeGoldenData(*this);
	const FRouteWaypointGeneratorContext Context = MakeContext(*this);
	const TCHAR* GoldenFiles[] = { GoldenRoute0, GoldenRoute1, GoldenRoute2 };
	const TCHAR* GoldenHashes[] = {
		TEXT("5cc60f87b5a6fbc14d895cf281d0f7ab90c8c453851b5d0858ec21fff611f3e9"),
		TEXT("757da928902f144a239a84a199f7b4f6fd8dac40c3160efb6e283003afaeacec"),
		TEXT("bcecf5863abb62b2031f1d2c77e9a9ede609bfa944b347c2cc6cb3a4bcb052db")
	};
	for (int32 RouteSlot = 0; RouteSlot < 3; ++RouteSlot)
	{
		FMemorySink Sink;
		const FRouteWaypointGenerationResult Result = GenerateToMemory(
			Data, Context, RouteSlot, Sink);
		TestTrue(TEXT("Golden route succeeds"), Result.bSuccess);
		TestEqual(TEXT("Golden row count"), Result.RowCount, 3);
		TestEqual(TEXT("Golden byte count"), Result.BytesWritten, static_cast<int64>(Sink.Bytes.Num()));
		TestEqual(TEXT("Exact WPL110 bytes"), Sink.Bytes, AsciiBytes(GoldenFiles[RouteSlot]));
		TestDigestHex(*this, Sink.Bytes, GoldenHashes[RouteSlot]);
		TestTrue(TEXT("No UTF-8 BOM"), Sink.Bytes.Num() >= 3
			&& !(Sink.Bytes[0] == 0xef && Sink.Bytes[1] == 0xbb && Sink.Bytes[2] == 0xbf));
		TestTrue(TEXT("No final newline"), Sink.Bytes.Num() > 0
			&& Sink.Bytes.Last() != '\r' && Sink.Bytes.Last() != '\n');
	}

	TArray<uint8> Row;
	FRouteWaypointGeneratorError Error;
	TestTrue(TEXT("Standalone golden row format"), RouteWaypointGenerator::FormatWaypointRow(
		0, 148, 26, 216, 319324710, 1186385310, -222241935, Row, Error));
	TestEqual(TEXT("Standalone row exact bytes"), Row, AsciiBytes(
		TEXT("0\t0\t3\t16\t148\t26\t216\t0\t31.932471\t118.638531\t-22.2241935\t1")));

	FRoutePreviewData MetadataVariant = Data;
	MetadataVariant.SampleRateHz = 1234.5;
	MetadataVariant.OptionalRangeStart = INDEX_NONE;
	MetadataVariant.OptionalRangeEnd = INDEX_NONE;
	FMemorySink Original;
	FMemorySink Variant;
	GenerateToMemory(Data, Context, 1, Original);
	GenerateToMemory(MetadataVariant, Context, 1, Variant);
	TestEqual(TEXT("Range and sample rate do not affect bytes"), Original.Bytes, Variant.Bytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointValidationAndFailureTests,
	"DroneGCS.RouteWaypointGenerator.2DC1.ValidationFailureCancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointValidationAndFailureTests::RunTest(const FString& Parameters)
{
	const FRoutePreviewData Golden = MakeGoldenData(*this);
	const FRouteWaypointGeneratorContext Context = MakeContext(*this);

	for (const int32 FailCall : {1, 4})
	{
		FMemorySink Sink;
		Sink.FailWriteCall = FailCall;
		const FRouteWaypointGenerationResult Result = GenerateToMemory(Golden, Context, 0, Sink);
		TestFalse(TEXT("Injected write failure fails route"), Result.bSuccess);
		TestEqual(TEXT("Injected write failure code"), Result.ErrorCode,
			ERouteWaypointGeneratorErrorCode::FileWriteFailed);
	}
	FMemorySink FinalizeFailure;
	FinalizeFailure.bFailFinalize = true;
	const FRouteWaypointGenerationResult FinalizeResult = GenerateToMemory(
		Golden, Context, 0, FinalizeFailure);
	TestFalse(TEXT("Injected final flush failure fails route"), FinalizeResult.bSuccess);
	TestEqual(TEXT("Final flush failure code"), FinalizeResult.ErrorCode,
		ERouteWaypointGeneratorErrorCode::FileWriteFailed);

	for (int32 Component = 0; Component < 3; ++Component)
	{
		FRoutePreviewData Invalid = Golden;
		if (Component == 0) Invalid.PositionsENU[0].X = std::numeric_limits<float>::quiet_NaN();
		if (Component == 1) Invalid.PositionsENU[0].Y = std::numeric_limits<float>::infinity();
		if (Component == 2) Invalid.PositionsENU[0].Z = std::numeric_limits<float>::quiet_NaN();
		FMemorySink Sink;
		const FRouteWaypointGenerationResult Result = GenerateToMemory(Invalid, Context, 0, Sink);
		TestEqual(TEXT("Non-finite ENU rejected"), Result.ErrorCode,
			ERouteWaypointGeneratorErrorCode::NonFinitePosition);
	}

	const FLinearColor InvalidColors[] = {
		FLinearColor(-0.0001f, 0.0f, 0.0f, 1.0f),
		// 1.0001 rounds to exactly 1.0 in the real FFloat16Color storage;
		// 1.001 preserves an above-one half-float for the generator defense.
		FLinearColor(1.001f, 0.0f, 0.0f, 1.0f),
		FLinearColor(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 1.0f),
		FLinearColor(0.0f, std::numeric_limits<float>::infinity(), 0.0f, 1.0f),
		FLinearColor(0.0f, 0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN())
	};
	for (const FLinearColor& InvalidColor : InvalidColors)
	{
		FRoutePreviewData Invalid = Golden;
		Invalid.Colors[0] = FFloat16Color(InvalidColor);
		FMemorySink Sink;
		const FRouteWaypointGenerationResult Result = GenerateToMemory(Invalid, Context, 0, Sink);
		TestEqual(TEXT("Invalid RGB or alpha rejected"), Result.ErrorCode,
			ERouteWaypointGeneratorErrorCode::InvalidColor);
	}

	FRoutePreviewData NearZeroColor = Golden;
	NearZeroColor.Colors[0] = FFloat16Color(FLinearColor(
		-1.1920928955078125e-7f, 0.0f, 0.0f, 1.0f));
	FMemorySink NearZeroSink;
	const FRouteWaypointGenerationResult NearZeroResult = GenerateToMemory(
		NearZeroColor, Context, 0, NearZeroSink);
	TestTrue(TEXT("Float16 zero-boundary RGB noise is accepted"), NearZeroResult.bSuccess);
	FUTF8ToTCHAR NearZeroConverted(
		reinterpret_cast<const ANSICHAR*>(NearZeroSink.Bytes.GetData()),
		NearZeroSink.Bytes.Num());
	const FString NearZeroText(NearZeroConverted.Length(), NearZeroConverted.Get());
	TestTrue(TEXT("Float16 zero-boundary RGB quantizes to uint8 zero"),
		NearZeroText.Contains(TEXT("\t0\t0\t0\t0\t")));

	FMemorySink NegativeAltitude;
	const FRouteWaypointGenerationResult NegativeResult = GenerateToMemory(
		Golden, Context, 0, NegativeAltitude);
	TestTrue(TEXT("Finite negative altitude accepted"), NegativeResult.bSuccess);

	FRoutePreviewData LongData;
	LongData.FrameCount = 130;
	LongData.DroneCount = 1;
	LongData.SourceIdentity = Golden.SourceIdentity;
	LongData.PositionsENU.Init(FVector3f::ZeroVector, LongData.FrameCount);
	LongData.Colors.Init(FFloat16Color(FLinearColor::White), LongData.FrameCount);
	int32 CancelChecks = 0;
	FMemorySink CancelSink;
	const FRouteWaypointGenerationResult CancelResult = GenerateToMemory(
		LongData, Context, 0, CancelSink,
		[&CancelChecks]() { return ++CancelChecks >= 2; });
	TestFalse(TEXT("Mid-route cancellation fails result"), CancelResult.bSuccess);
	TestEqual(TEXT("Cancellation code"), CancelResult.ErrorCode,
		ERouteWaypointGeneratorErrorCode::Cancelled);
	TestEqual(TEXT("Cancellation stops at bounded check"), CancelResult.RowCount, 64);
	TestFalse(TEXT("Cancelled sink is not finalized"), CancelSink.bFinalizeCalled);

	FRouteWaypointGeneratorInput InvalidInput;
	InvalidInput.RouteData = &Golden;
	InvalidInput.RouteSlotIndex = 3;
	InvalidInput.SourceIdentity = Golden.SourceIdentity;
	InvalidInput.Context = Context;
	FMemorySink InvalidSlotSink;
	TestEqual(TEXT("Invalid route slot rejected"),
		RouteWaypointGenerator::GenerateRoute(InvalidInput, InvalidSlotSink).ErrorCode,
		ERouteWaypointGeneratorErrorCode::InvalidRouteSlot);
	InvalidInput.RouteSlotIndex = 0;
	InvalidInput.SourceIdentity.SourceByteSize++;
	FMemorySink SourceMismatchSink;
	TestEqual(TEXT("Source identity mismatch rejected"),
		RouteWaypointGenerator::GenerateRoute(InvalidInput, SourceMismatchSink).ErrorCode,
		ERouteWaypointGeneratorErrorCode::SourceIdentityMismatch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointWorkspaceAdapterTests,
	"DroneGCS.RouteWaypointGenerator.2DC1.WorkspaceStagingAdapter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointWorkspaceAdapterTests::RunTest(const FString& Parameters)
{
	const FRoutePreviewData Data = MakeGoldenData(*this);
	const FRouteWaypointGeneratorContext Context = MakeContext(*this);
	FRouteArtifactBuildIdentity Build;
	Build.AssignmentBinding.FleetSessionID = TEXT("2dc1-fleet");
	Build.AssignmentBinding.AssignmentAuthoritySessionID = TEXT("2dc1-authority");
	Build.AssignmentBinding.AssignmentRevision = 1;
	Build.AssignmentBinding.AssignmentDigest = HashText(*this, TEXT("2dc1-assignment"));
	Build.SourceDatasetDigest = Data.SourceIdentity.SourceDatasetDigest;
	Build.WaypointGeneratorVersion = Context.WaypointGeneratorVersion;
	Build.GeneratorInputDigest = Context.GeneratorInputDigest;

	FRouteArtifactWorkspaceAuthorityGate Gate;
	Gate.bHasFrozenSnapshot = true;
	Gate.FrozenFleetSessionID = Build.AssignmentBinding.FleetSessionID;
	Gate.FrozenAssignmentAuthoritySessionID = Build.AssignmentBinding.AssignmentAuthoritySessionID;
	Gate.FrozenAssignmentRevision = Build.AssignmentBinding.AssignmentRevision;
	Gate.bConsoleAccepted = true;
	Gate.ConsoleAcceptedBinding = Build.AssignmentBinding;
	Gate.bSourceExists = true;
	Gate.ActiveSourceIdentity = Data.SourceIdentity;
	Gate.RouteSlotCount = Data.DroneCount;

	const FString Root = MakeTempRoot();
	FRouteArtifactWorkspaceManager Manager;
	FString Error;
	TestTrue(TEXT("Configure generator test Workspace"),
		Manager.SetConfiguredWorkspaceRoot(Root, Error));
	FRouteArtifactGenerationHandle Handle;
	TestTrue(TEXT("Begin generator test attempt"),
		Manager.BeginGeneration(Build, Gate, Handle, Error));

	FRouteWaypointGeneratorInput Input;
	Input.RouteData = &Data;
	Input.RouteSlotIndex = 1;
	Input.SourceIdentity = Data.SourceIdentity;
	Input.Context = Context;
	const FRouteWaypointGenerationResult Result =
		RouteWaypointGenerator::GenerateRouteToStagingFile(Manager, Input);
	TestTrue(TEXT("Safe staging adapter generates one route"), Result.bSuccess);
	FString RoutePath;
	TestTrue(TEXT("Resolve generated safe path"),
		Manager.ResolveSafeStagingRoutePath(1, RoutePath, Error));
	TArray<uint8> FileBytes;
	TestTrue(TEXT("Read generated staging route"), FFileHelper::LoadFileToArray(FileBytes, *RoutePath));
	TestEqual(TEXT("Staging file exact golden bytes"), FileBytes, AsciiBytes(GoldenRoute1));

	TestTrue(TEXT("Abort test attempt"), Manager.AbortGeneration(Error));
	Manager.Shutdown();
	TestTrue(TEXT("Remove isolated test Workspace"),
		IFileManager::Get().DeleteDirectory(*Root, false, true));
	return true;
}

#endif
