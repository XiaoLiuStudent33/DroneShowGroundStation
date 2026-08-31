#include "RouteWaypointValidator.h"

#include "HAL/PlatformFileManager.h"
#include "RouteAssignmentGeo.h"
#include "Wpl110Core.h"

namespace
{
	constexpr int32 StreamingBufferBytes = 64 * 1024;

	bool ValidatorSameSourceIdentity(
		const FRouteArtifactSourceIdentity& Left,
		const FRouteArtifactSourceIdentity& Right)
	{
		return Left.IsValid() && Right.IsValid()
			&& Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.SourceByteSize == Right.SourceByteSize
			&& Left.SourceDisplayFileName == Right.SourceDisplayFileName;
	}

	struct FSourceConsumerContext
	{
		const FRouteWaypointSourceValidationContext* Source = nullptr;
		FRouteWaypointValidationResult* Result = nullptr;
		int32 RouteSlotIndex = INDEX_NONE;
	};

	bool RejectSource(
		FSourceConsumerContext& Context,
		const ERouteWaypointValidationErrorCode Code,
		const int32 RowIndex,
		const int32 FieldIndex,
		const TCHAR* Diagnostic)
	{
		Context.Result->ErrorCode = Code;
		Context.Result->RowIndex = RowIndex;
		Context.Result->FieldIndex = FieldIndex;
		Context.Result->Diagnostic = Diagnostic;
		return false;
	}

	bool CompareSourceRow(const dronegcs::wpl110::Row& Row, void* const Opaque)
	{
		FSourceConsumerContext& Context = *static_cast<FSourceConsumerContext*>(Opaque);
		const FRouteWaypointSourceValidationContext& Source = *Context.Source;
		const int32 FrameIndex = Row.Sequence;
		const int64 FlatIndex64 = static_cast<int64>(FrameIndex) * Source.RouteCount
			+ Context.RouteSlotIndex;
		const int64 ExpectedSamples = static_cast<int64>(Source.FrameCount) * Source.RouteCount;
		if (FlatIndex64 < 0 || FlatIndex64 >= ExpectedSamples || FlatIndex64 > MAX_int32)
		{
			return RejectSource(Context,
				ERouteWaypointValidationErrorCode::InvalidInput,
				FrameIndex, INDEX_NONE, TEXT("Source sample index is outside the captured domain."));
		}
		const int32 FlatIndex = static_cast<int32>(FlatIndex64);
		const FVector3f& Position = Source.RouteData->PositionsENU[FlatIndex];
		const double EastMeters = static_cast<double>(Position.X);
		const double NorthMeters = static_cast<double>(Position.Y);
		const double UpMeters = static_cast<double>(Position.Z);
		if (!FMath::IsFinite(EastMeters) || !FMath::IsFinite(NorthMeters)
			|| !FMath::IsFinite(UpMeters))
		{
			return RejectSource(Context,
				ERouteWaypointValidationErrorCode::SourcePositionInvalid,
				FrameIndex, INDEX_NONE, TEXT("Captured source position is non-finite."));
		}

		const FLinearColor Color = Source.RouteData->Colors[FlatIndex].GetFloats();
		uint8 ExpectedRed = 0;
		uint8 ExpectedGreen = 0;
		uint8 ExpectedBlue = 0;
		if (!dronegcs::wpl110::QuantizeRgb(
				Color.R, Color.G, Color.B, Color.A,
				ExpectedRed, ExpectedGreen, ExpectedBlue))
		{
			return RejectSource(Context,
				ERouteWaypointValidationErrorCode::SourceColorInvalid,
				FrameIndex, INDEX_NONE, TEXT("Captured source color is outside the v1 domain."));
		}

		RouteAssignmentGeo::FGeodeticPosition Geodetic;
		FString GeoError;
		if (!RouteAssignmentGeo::LocalEnuToGeodetic(
				Source.GeneratorContext.GeoOrigin,
				EastMeters, NorthMeters, 0.0, Geodetic, GeoError))
		{
			return RejectSource(Context,
				ERouteWaypointValidationErrorCode::SourceGeoConversionFailed,
				FrameIndex, INDEX_NONE, TEXT("Captured source ENU conversion failed."));
		}
		int32 ExpectedLatitudeE7 = 0;
		int32 ExpectedLongitudeE7 = 0;
		int64 ExpectedAltitudeE7 = 0;
		if (!dronegcs::wpl110::QuantizeGeodeticE7(
				Geodetic.LatitudeDegrees, -90.0, 90.0, ExpectedLatitudeE7)
			|| !dronegcs::wpl110::QuantizeGeodeticE7(
				Geodetic.LongitudeDegrees, -180.0, 180.0, ExpectedLongitudeE7)
			|| !dronegcs::wpl110::QuantizeAltitudeE7(UpMeters, ExpectedAltitudeE7))
		{
			return RejectSource(Context,
				ERouteWaypointValidationErrorCode::SourceNumericOverflow,
				FrameIndex, INDEX_NONE, TEXT("Captured source coordinate exceeds the v1 E7 domain."));
		}

		if (Row.Red != ExpectedRed) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceRedMismatch,
			FrameIndex, 4, TEXT("Waypoint red channel does not match the captured source."));
		if (Row.Green != ExpectedGreen) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceGreenMismatch,
			FrameIndex, 5, TEXT("Waypoint green channel does not match the captured source."));
		if (Row.Blue != ExpectedBlue) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceBlueMismatch,
			FrameIndex, 6, TEXT("Waypoint blue channel does not match the captured source."));
		if (Row.LatitudeE7 != ExpectedLatitudeE7) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceLatitudeMismatch,
			FrameIndex, 8, TEXT("Waypoint latitude does not match the captured source."));
		if (Row.LongitudeE7 != ExpectedLongitudeE7) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceLongitudeMismatch,
			FrameIndex, 9, TEXT("Waypoint longitude does not match the captured source."));
		if (Row.AltitudeE7 != ExpectedAltitudeE7) return RejectSource(Context,
			ERouteWaypointValidationErrorCode::SourceAltitudeMismatch,
			FrameIndex, 10, TEXT("Waypoint altitude does not match the captured source."));
		return true;
	}
}

bool FRouteWaypointSourceValidationContext::IsValid() const
{
	if (!RouteData.IsValid() || !RouteData->IsLoaded()
		|| !ValidatorSameSourceIdentity(SourceIdentity, RouteData->SourceIdentity)
		|| RouteDatasetGeneration <= 0 || !GeneratorContext.IsValid()
		|| FrameCount <= 0 || RouteCount <= 0
		|| FrameCount != RouteData->FrameCount || RouteCount != RouteData->DroneCount)
	{
		return false;
	}
	const int64 ExpectedSamples = static_cast<int64>(FrameCount) * RouteCount;
	return ExpectedSamples > 0 && ExpectedSamples <= MAX_int32
		&& RouteData->PositionsENU.Num() == ExpectedSamples
		&& RouteData->Colors.Num() == ExpectedSamples;
}

bool RouteWaypointValidator::ValidateAndHashFileStreaming(
	const FString& FilePath,
	const int32 RouteSlotIndex,
	const int32 ExpectedFrameCount,
	const ERouteWaypointValidationLevel Level,
	const FRouteWaypointSourceValidationContext* const SourceContext,
	FRouteWaypointValidationResult& OutResult,
	const TFunction<bool()>& IsCancelled)
{
	OutResult = FRouteWaypointValidationResult{};
	OutResult.Level = Level;
	OutResult.RouteSlotIndex = RouteSlotIndex;
	if (ExpectedFrameCount <= 0 || RouteSlotIndex < 0
		|| (Level == ERouteWaypointValidationLevel::SourceBound
			&& (!SourceContext || !SourceContext->IsValid()
				|| SourceContext->FrameCount != ExpectedFrameCount
				|| RouteSlotIndex >= SourceContext->RouteCount)))
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::InvalidInput;
		OutResult.Diagnostic = TEXT("Waypoint validation inputs are invalid.");
		return false;
	}

	FFileOpenResult OpenResult = FPlatformFileManager::Get().GetPlatformFile().OpenRead(
		*FilePath, IPlatformFile::EOpenReadFlags::AllowDelete);
	TUniquePtr<IFileHandle> Handle;
	if (OpenResult.IsValid()) Handle = OpenResult.StealValue();
	if (!Handle)
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::FileOpenFailed;
		OutResult.Diagnostic = TEXT("Waypoint file could not be opened for validation.");
		return false;
	}
	const int64 SizeAtOpen = Handle->Size();
	const int64 MaximumSize = static_cast<int64>(dronegcs::wpl110::HeaderLength)
		+ static_cast<int64>(ExpectedFrameCount)
			* static_cast<int64>(dronegcs::wpl110::MaxRowBytes + 2);
	if (SizeAtOpen <= 0 || SizeAtOpen > MaximumSize)
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::FileSizeInvalid;
		OutResult.Diagnostic = TEXT("Waypoint file size is outside the bounded v1 domain.");
		return false;
	}

	FRouteArtifactSha256Builder HashBuilder;
	FString HashError;
	if (!HashBuilder.Initialize(HashError))
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::HashFailed;
		OutResult.Diagnostic = TEXT("Waypoint SHA-256 initialization failed.");
		return false;
	}
	FSourceConsumerContext ConsumerContext{ SourceContext, &OutResult, RouteSlotIndex };
	dronegcs::wpl110::StreamingParser Parser(
		ExpectedFrameCount,
		Level == ERouteWaypointValidationLevel::SourceBound ? &CompareSourceRow : nullptr,
		Level == ERouteWaypointValidationLevel::SourceBound ? &ConsumerContext : nullptr);
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(StreamingBufferBytes);
	while (OutResult.FileByteSize < SizeAtOpen)
	{
		if (IsCancelled && IsCancelled())
		{
			OutResult.ErrorCode = ERouteWaypointValidationErrorCode::Cancelled;
			OutResult.Diagnostic = TEXT("Waypoint validation was cancelled.");
			return false;
		}
		const int64 ReadBytes = FMath::Min<int64>(
			SizeAtOpen - OutResult.FileByteSize, Buffer.Num());
		if (!Handle->Read(Buffer.GetData(), ReadBytes))
		{
			OutResult.ErrorCode = ERouteWaypointValidationErrorCode::FileReadFailed;
			OutResult.Diagnostic = TEXT("Waypoint file read ended before the opened size.");
			return false;
		}
		if (!HashBuilder.Update(Buffer.GetData(), static_cast<uint64>(ReadBytes), HashError))
		{
			OutResult.ErrorCode = ERouteWaypointValidationErrorCode::HashFailed;
			OutResult.Diagnostic = TEXT("Waypoint SHA-256 update failed.");
			return false;
		}
		if (!Parser.Consume(Buffer.GetData(), static_cast<size_t>(ReadBytes)))
		{
			break;
		}
		OutResult.FileByteSize += ReadBytes;
	}
	OutResult.RowCount = Parser.GetParsedRowCount();
	if (OutResult.ErrorCode != ERouteWaypointValidationErrorCode::None
		|| !Parser.Finish())
	{
		if (OutResult.ErrorCode == ERouteWaypointValidationErrorCode::None)
		{
			const dronegcs::wpl110::Error& Error = Parser.GetError();
			OutResult.ErrorCode = ERouteWaypointValidationErrorCode::SemanticParseFailed;
			OutResult.ParserErrorCode = Error.Code;
			OutResult.RowIndex = Error.RowIndex;
			OutResult.FieldIndex = Error.FieldIndex;
			OutResult.Diagnostic = TEXT("Waypoint file violates the canonical WPL110 contract.");
		}
		return false;
	}
	OutResult.RowCount = Parser.GetParsedRowCount();
	if (Handle->Size() != SizeAtOpen || Handle->Tell() != SizeAtOpen)
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::FileChangedDuringRead;
		OutResult.Diagnostic = TEXT("Waypoint file changed while it was validated.");
		return false;
	}
	if (!HashBuilder.Finalize(OutResult.FileDigest, HashError))
	{
		OutResult.ErrorCode = ERouteWaypointValidationErrorCode::HashFailed;
		OutResult.Diagnostic = TEXT("Waypoint SHA-256 finalization failed.");
		return false;
	}
	OutResult.bSuccess = true;
	return true;
}
