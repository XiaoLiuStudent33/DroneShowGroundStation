#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "HAL/FileManager.h"
#include "Math/Float16Color.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "RouteWaypointGenerator.h"
#include "RouteWaypointValidator.h"
#include "Wpl110Core.h"

namespace
{
	class FValidationMemorySink final : public IRouteWaypointByteSink
	{
	public:
		bool Write(const uint8* Data, const int32 ByteCount, FString& OutError) override
		{
			OutError.Reset();
			Bytes.Append(Data, ByteCount);
			return true;
		}

		bool Finalize(FString& OutError) override
		{
			OutError.Reset();
			return true;
		}

		TArray<uint8> Bytes;
	};

	TArray<uint8> ValidationAscii(const FString& Text)
	{
		FTCHARToUTF8 Utf8(*Text);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	FString ValidationTempRoot()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectIntermediateDir(), TEXT("RouteWaypointValidatorTests"),
			FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	bool SaveValidationFile(const FString& Path, const TArray<uint8>& Bytes)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		return FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> MakeValidationData(
		FAutomationTestBase& Test)
	{
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data =
			MakeShared<FRoutePreviewData, ESPMode::ThreadSafe>();
		Data->FrameCount = 2;
		Data->DroneCount = 1;
		Data->SampleRateHz = 50.0;
		Data->PositionsENU = {
			FVector3f(0.0f, 0.0f, 1.25f),
			FVector3f(1.0f, 2.0f, -3.0f)
		};
		Data->Colors = {
			FFloat16Color(FLinearColor(0.5f, 0.25f, 1.0f, 1.0f)),
			FFloat16Color(FLinearColor(0.0f, 0.75f, 0.125f, 1.0f))
		};
		FRouteArtifactDigest256 Digest;
		FString Error;
		const TArray<uint8> DigestBytes = ValidationAscii(TEXT("2e-b-validator-source"));
		Test.TestTrue(TEXT("Validator source digest"),
			FRouteArtifactSha256::HashBytes(DigestBytes, Digest, Error));
		Test.TestTrue(TEXT("Validator source identity"),
			FRouteArtifactSourceIdentity::Create(
				Digest, 12345, TEXT("validator-source.npz"),
				Data->SourceIdentity, Error));
		return Data;
	}

	bool MakeGeneratedFixture(
		FAutomationTestBase& Test,
		TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe>& OutData,
		FRouteWaypointSourceValidationContext& OutContext,
		TArray<uint8>& OutBytes)
	{
		OutData = MakeValidationData(Test);
		FRouteWaypointGeneratorContext GeneratorContext;
		FRouteWaypointGeneratorError Error;
		if (!RouteWaypointGenerator::BuildContext(
				31.0, 118.0, GeneratorContext, Error))
		{
			Test.AddError(Error.Diagnostic);
			return false;
		}
		FValidationMemorySink Sink;
		FRouteWaypointGeneratorInput Input;
		Input.RouteData = OutData.Get();
		Input.RouteSlotIndex = 0;
		Input.SourceIdentity = OutData->SourceIdentity;
		Input.Context = GeneratorContext;
		const FRouteWaypointGenerationResult Generated =
			RouteWaypointGenerator::GenerateRoute(Input, Sink, [] { return false; });
		if (!Generated.bSuccess)
		{
			Test.AddError(Generated.Diagnostic);
			return false;
		}
		OutContext.RouteData = OutData;
		OutContext.SourceIdentity = OutData->SourceIdentity;
		OutContext.RouteDatasetGeneration = 1;
		OutContext.GeneratorContext = GeneratorContext;
		OutContext.FrameCount = OutData->FrameCount;
		OutContext.RouteCount = OutData->DroneCount;
		OutBytes = MoveTemp(Sink.Bytes);
		return true;
	}

	FString BytesToText(const TArray<uint8>& Bytes)
	{
		FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	TArray<uint8> MutateFirstRowField(
		const TArray<uint8>& Bytes,
		const int32 FieldIndex,
		const FString& Replacement)
	{
		TArray<FString> Lines;
		BytesToText(Bytes).ParseIntoArray(Lines, TEXT("\r\n"), false);
		TArray<FString> Fields;
		Lines[1].ParseIntoArray(Fields, TEXT("\t"), false);
		Fields[FieldIndex] = Replacement;
		Lines[1] = FString::Join(Fields, TEXT("\t"));
		return ValidationAscii(FString::Join(Lines, TEXT("\r\n")));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointStreamingValidationTests,
	"DroneGCS.RouteWaypointValidation.2EB.StreamingSemanticAndSourceBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointStreamingValidationTests::RunTest(const FString& Parameters)
{
	const FString Root = ValidationTempRoot();
	const FString Path = FPaths::Combine(Root, TEXT("route.waypoints"));
	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data;
	FRouteWaypointSourceValidationContext Context;
	TArray<uint8> Bytes;
	TestTrue(TEXT("Generate validator fixture"),
		MakeGeneratedFixture(*this, Data, Context, Bytes));
	TestTrue(TEXT("Write validator fixture"), SaveValidationFile(Path, Bytes));

	FRouteWaypointValidationResult Semantic;
	TestTrue(TEXT("WaypointSemantic accepts canonical generated bytes"),
		RouteWaypointValidator::ValidateAndHashFileStreaming(
			Path, 0, 2, ERouteWaypointValidationLevel::WaypointSemantic,
			nullptr, Semantic));
	TestEqual(TEXT("Semantic row count"), Semantic.RowCount, 2);
	TestEqual(TEXT("Semantic physical byte count"), Semantic.FileByteSize,
		static_cast<int64>(Bytes.Num()));

	FRouteWaypointValidationResult SourceBound;
	TestTrue(TEXT("SourceBound accepts captured source"),
		RouteWaypointValidator::ValidateAndHashFileStreaming(
			Path, 0, 2, ERouteWaypointValidationLevel::SourceBound,
			&Context, SourceBound));
	TestEqual(TEXT("Both levels hash the same physical bytes"),
		SourceBound.FileDigest, Semantic.FileDigest);
	for (const int32 ChunkSize : {1, 2, 3, 7, 11, 17, 64})
	{
		dronegcs::wpl110::StreamingParser Parser(2);
		for (int32 Offset = 0; Offset < Bytes.Num(); Offset += ChunkSize)
		{
			const int32 Count = FMath::Min(ChunkSize, Bytes.Num() - Offset);
			TestTrue(*FString::Printf(TEXT("Chunk boundary consume size=%d"), ChunkSize),
				Parser.Consume(Bytes.GetData() + Offset, static_cast<size_t>(Count)));
		}
		TestTrue(*FString::Printf(TEXT("Chunk boundary finish size=%d"), ChunkSize),
			Parser.Finish());
		TestEqual(TEXT("Chunk boundary row count"), Parser.GetParsedRowCount(), 2);
	}

	TArray<uint8> MultiChunkBytes;
	MultiChunkBytes.Append(
		reinterpret_cast<const uint8*>(dronegcs::wpl110::Header),
		static_cast<int32>(dronegcs::wpl110::HeaderLength));
	MultiChunkBytes.Add('\r');
	MultiChunkBytes.Add('\n');
	constexpr int32 MultiChunkRows = 2000;
	for (int32 RowIndex = 0; RowIndex < MultiChunkRows; ++RowIndex)
	{
		dronegcs::wpl110::Row Row;
		Row.Sequence = RowIndex;
		dronegcs::wpl110::FormattedRow Formatted;
		TestTrue(TEXT("Format multi-chunk cancellation row"),
			dronegcs::wpl110::FormatRow(Row, Formatted));
		MultiChunkBytes.Append(Formatted.Bytes.data(), static_cast<int32>(Formatted.Size));
		if (RowIndex + 1 < MultiChunkRows)
		{
			MultiChunkBytes.Add('\r');
			MultiChunkBytes.Add('\n');
		}
	}
	const FString CancelPath = FPaths::Combine(Root, TEXT("cancel.waypoints"));
	TestTrue(TEXT("Write multi-chunk cancellation fixture"),
		SaveValidationFile(CancelPath, MultiChunkBytes));
	int32 CancellationChecks = 0;
	FRouteWaypointValidationResult Cancelled;
	TestFalse(TEXT("Streaming validation cancels at a chunk boundary"),
		RouteWaypointValidator::ValidateAndHashFileStreaming(
			CancelPath, 0, MultiChunkRows,
			ERouteWaypointValidationLevel::WaypointSemantic,
			nullptr, Cancelled,
			[&CancellationChecks]() { return ++CancellationChecks >= 2; }));
	TestEqual(TEXT("Chunk cancellation classification"), Cancelled.ErrorCode,
		ERouteWaypointValidationErrorCode::Cancelled);
	TestEqual(TEXT("Chunk cancellation checks once per attempted chunk"),
		CancellationChecks, 2);
	TestTrue(TEXT("Chunk cancellation stops before the physical EOF"),
		Cancelled.FileByteSize > 0
			&& Cancelled.FileByteSize < static_cast<int64>(MultiChunkBytes.Num()));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointMalformedCorpusTests,
	"DroneGCS.RouteWaypointValidation.2EB.MalformedCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointMalformedCorpusTests::RunTest(const FString& Parameters)
{
	const FString Root = ValidationTempRoot();
	const FString ValidRow0 =
		TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1");
	const FString ValidRow1 =
		TEXT("1\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1");
	const FString Prefix = TEXT("QGC WPL 110\r\n");
	struct FMalformedCase { FString Name; FString Text; int32 Rows = 1; };
	TArray<FMalformedCase> Cases;
	Cases.Add({ TEXT("bom"), FString(TEXT("\xFEFFQGC WPL 110\r\n")) + ValidRow0 });
	Cases.Add({ TEXT("header"), FString(TEXT("QGC WPL 120\r\n")) + ValidRow0 });
	Cases.Add({ TEXT("header-lf"), FString(TEXT("QGC WPL 110\n")) + ValidRow0 });
	Cases.Add({ TEXT("header-truncated"), TEXT("QGC WPL 110\r") });
	Cases.Add({ TEXT("row-lf"), Prefix + ValidRow0 + TEXT("\n") });
	Cases.Add({ TEXT("bare-cr"), Prefix + ValidRow0 + TEXT("\r") });
	Cases.Add({ TEXT("mixed-newline"), Prefix + ValidRow0 + TEXT("\r\n") + ValidRow1 + TEXT("\n"), 2 });
	Cases.Add({ TEXT("final-newline"), Prefix + ValidRow0 + TEXT("\r\n") });
	Cases.Add({ TEXT("blank-row"), Prefix + TEXT("\r\n") });
	Cases.Add({ TEXT("trailing-data"), Prefix + ValidRow0 + TEXT("\r\nX") });
	Cases.Add({ TEXT("11-fields"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0") });
	Cases.Add({ TEXT("13-fields"), Prefix + ValidRow0 + TEXT("\t0") });
	Cases.Add({ TEXT("empty-field"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("sequence-gap"), Prefix + ValidRow0 + TEXT("\r\n")
		+ TEXT("2\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1"), 2 });
	Cases.Add({ TEXT("sequence-duplicate"), Prefix + ValidRow0 + TEXT("\r\n")
		+ ValidRow0, 2 });
	Cases.Add({ TEXT("fixed-command"), Prefix
		+ TEXT("0\t0\t3\t17\t0\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-negative"), Prefix
		+ TEXT("0\t0\t3\t16\t-1\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-range"), Prefix
		+ TEXT("0\t0\t3\t16\t256\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-decimal"), Prefix
		+ TEXT("0\t0\t3\t16\t1.0\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-leading-zero"), Prefix
		+ TEXT("0\t0\t3\t16\t00\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-plus"), Prefix
		+ TEXT("0\t0\t3\t16\t+1\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("rgb-exponent"), Prefix
		+ TEXT("0\t0\t3\t16\t1e0\t0\t0\t0\t0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("decimal-plus"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t+0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("decimal-exponent"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0e0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("decimal-leading-zero"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t00.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("decimal-trailing-zero"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.10\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("negative-zero"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t-0.0\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("latitude-range"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t90.0000001\t0.0\t0.0\t1") });
	Cases.Add({ TEXT("longitude-range"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t180.0000001\t0.0\t1") });
	Cases.Add({ TEXT("alt-overflow"), Prefix
		+ TEXT("0\t0\t3\t16\t0\t0\t0\t0\t0.0\t0.0\t922337203685.4775808\t1") });
	Cases.Add({ TEXT("row-too-long"), Prefix + FString::ChrN(81, '1') });

	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		const FString Path = FPaths::Combine(
			Root, FString::Printf(TEXT("%02d.waypoints"), Index));
		TestTrue(*FString::Printf(TEXT("Write malformed case %s"), *Cases[Index].Name),
			SaveValidationFile(Path, ValidationAscii(Cases[Index].Text)));
		FRouteWaypointValidationResult Result;
		TestFalse(*FString::Printf(TEXT("Reject malformed case %s"), *Cases[Index].Name),
			RouteWaypointValidator::ValidateAndHashFileStreaming(
				Path, 0, Cases[Index].Rows, ERouteWaypointValidationLevel::WaypointSemantic,
				nullptr, Result));
		TestTrue(*FString::Printf(TEXT("Classify malformed case %s"), *Cases[Index].Name),
			Result.ErrorCode != ERouteWaypointValidationErrorCode::None);
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRouteWaypointSourceCorruptionTests,
	"DroneGCS.RouteWaypointValidation.2EB.SourceCorruption",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FRouteWaypointSourceCorruptionTests::RunTest(const FString& Parameters)
{
	const FString Root = ValidationTempRoot();
	const FString Path = FPaths::Combine(Root, TEXT("corrupt.waypoints"));
	TSharedPtr<FRoutePreviewData, ESPMode::ThreadSafe> Data;
	FRouteWaypointSourceValidationContext Context;
	TArray<uint8> Original;
	TestTrue(TEXT("Generate source corruption fixture"),
		MakeGeneratedFixture(*this, Data, Context, Original));

	struct FCase { int32 Field; FString Value; ERouteWaypointValidationErrorCode Code; };
	const TArray<FCase> Cases = {
		{ 4, TEXT("128"), ERouteWaypointValidationErrorCode::SourceRedMismatch },
		{ 8, TEXT("31.0000001"), ERouteWaypointValidationErrorCode::SourceLatitudeMismatch },
		{ 9, TEXT("118.0000001"), ERouteWaypointValidationErrorCode::SourceLongitudeMismatch },
		{ 10, TEXT("1.2500001"), ERouteWaypointValidationErrorCode::SourceAltitudeMismatch }
	};
	for (const FCase& Case : Cases)
	{
		const TArray<uint8> Corrupt = MutateFirstRowField(Original, Case.Field, Case.Value);
		TestTrue(TEXT("Write canonical source corruption"), SaveValidationFile(Path, Corrupt));
		FRouteWaypointValidationResult Semantic;
		TestTrue(TEXT("Corruption remains WPL semantic-valid"),
			RouteWaypointValidator::ValidateAndHashFileStreaming(
				Path, 0, 2, ERouteWaypointValidationLevel::WaypointSemantic,
				nullptr, Semantic));
		FRouteArtifactDigest256 RecomputedIntegrityDigest;
		int64 RecomputedIntegritySize = 0;
		FString IntegrityError;
		TestTrue(TEXT("Phase2C integrity accepts the self-consistent corrupted bytes"),
			FRouteArtifactSha256::HashFileStreaming(
				Path, RecomputedIntegrityDigest, RecomputedIntegritySize, IntegrityError));
		TestEqual(TEXT("Integrity size covers the corrupted physical file"),
			RecomputedIntegritySize, static_cast<int64>(Corrupt.Num()));
		FRouteWaypointValidationResult SourceBound;
		TestFalse(TEXT("SourceBound rejects source corruption"),
			RouteWaypointValidator::ValidateAndHashFileStreaming(
				Path, 0, 2, ERouteWaypointValidationLevel::SourceBound,
				&Context, SourceBound));
		TestEqual(TEXT("Source mismatch classification"), SourceBound.ErrorCode, Case.Code);
	}
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}

#endif
