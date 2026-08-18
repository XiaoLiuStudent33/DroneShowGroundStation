#include "RouteWaypointGenerator.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/ScopeLock.h"
#include "RouteArtifactWorkspaceManager.h"
#include "Wpl110Core.h"

namespace
{
	constexpr uint8 CrLf[] = { '\r', '\n' };

	void AppendBigEndianU16(TArray<uint8>& OutBytes, const uint16 Value)
	{
		OutBytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		OutBytes.Add(static_cast<uint8>(Value & 0xff));
	}

	void AppendBigEndianI32(TArray<uint8>& OutBytes, const int32 Value)
	{
		const uint32 Bits = static_cast<uint32>(Value);
		OutBytes.Add(static_cast<uint8>((Bits >> 24) & 0xff));
		OutBytes.Add(static_cast<uint8>((Bits >> 16) & 0xff));
		OutBytes.Add(static_cast<uint8>((Bits >> 8) & 0xff));
		OutBytes.Add(static_cast<uint8>(Bits & 0xff));
	}

	bool NormalizeOriginCoordinate(
		const double Value,
		const double Minimum,
		const double Maximum,
		int32& OutE7)
	{
		OutE7 = 0;
		if (!FMath::IsFinite(Value) || Value < Minimum || Value > Maximum)
		{
			return false;
		}
		return dronegcs::wpl110::QuantizeGeodeticE7(
			Value, Minimum, Maximum, OutE7);
	}

	bool QuantizeGeodeticE7(
		const double Degrees,
		const double Minimum,
		const double Maximum,
		int32& OutE7)
	{
		return dronegcs::wpl110::QuantizeGeodeticE7(
			Degrees, Minimum, Maximum, OutE7);
	}

	bool QuantizeAltitudeE7(const double Meters, int64& OutE7)
	{
		return dronegcs::wpl110::QuantizeAltitudeE7(Meters, OutE7);
	}

	bool SameSourceIdentity(
		const FRouteArtifactSourceIdentity& Left,
		const FRouteArtifactSourceIdentity& Right)
	{
		return Left.IsValid()
			&& Right.IsValid()
			&& Left.SourceDatasetDigest == Right.SourceDatasetDigest
			&& Left.SourceByteSize == Right.SourceByteSize
			&& Left.SourceDisplayFileName == Right.SourceDisplayFileName;
	}

	FRouteWaypointGenerationResult MakeFailure(
		const int32 RouteSlotIndex,
		const int32 RowCount,
		const int64 BytesWritten,
		const ERouteWaypointGeneratorErrorCode Code,
		const TCHAR* Diagnostic)
	{
		FRouteWaypointGenerationResult Result;
		Result.RouteSlotIndex = RouteSlotIndex;
		Result.RowCount = RowCount;
		Result.BytesWritten = BytesWritten;
		Result.ErrorCode = Code;
		Result.Diagnostic = Diagnostic;
		return Result;
	}

	class FBufferedRouteWaypointFileSink final : public IRouteWaypointByteSink
	{
	public:
		explicit FBufferedRouteWaypointFileSink(TUniquePtr<IFileHandle>&& InHandle)
			: Handle(MoveTemp(InHandle))
		{
			Buffer.Reserve(RouteWaypointGeneratorContract::FileBufferBytes);
		}

		bool Write(const uint8* Data, const int32 ByteCount, FString& OutError) override
		{
			OutError.Reset();
			if (bFinalized || !Handle || ByteCount < 0 || (ByteCount > 0 && Data == nullptr))
			{
				OutError = TEXT("Waypoint file sink is not writable.");
				return false;
			}
			int32 Remaining = ByteCount;
			while (Remaining > 0)
			{
				const int32 Available = RouteWaypointGeneratorContract::FileBufferBytes - Buffer.Num();
				const int32 CopyCount = FMath::Min(Available, Remaining);
				Buffer.Append(Data, CopyCount);
				Data += CopyCount;
				Remaining -= CopyCount;
				if (Buffer.Num() == RouteWaypointGeneratorContract::FileBufferBytes
					&& !FlushBuffer(OutError))
				{
					return false;
				}
			}
			return true;
		}

		bool Finalize(FString& OutError) override
		{
			OutError.Reset();
			if (bFinalized || !Handle || !FlushBuffer(OutError) || !Handle->Flush(true))
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("Waypoint file finalization failed.");
				}
				return false;
			}
			bFinalized = true;
			Handle.Reset();
			return true;
		}

	private:
		bool FlushBuffer(FString& OutError)
		{
			if (Buffer.IsEmpty())
			{
				return true;
			}
			if (!Handle || !Handle->Write(Buffer.GetData(), Buffer.Num()))
			{
				OutError = TEXT("Waypoint file write failed.");
				return false;
			}
			Buffer.Reset();
			return true;
		}

		TUniquePtr<IFileHandle> Handle;
		TArray<uint8> Buffer;
		bool bFinalized = false;
	};
}

void FRouteWaypointFileActivityTracker::NotifyRouteFileOpened()
{
	FScopeLock Lock(&Mutex);
	++ActiveRouteFileCount;
	PeakActiveRouteFileCount = FMath::Max(PeakActiveRouteFileCount, ActiveRouteFileCount);
}

void FRouteWaypointFileActivityTracker::NotifyRouteFileClosed()
{
	FScopeLock Lock(&Mutex);
	check(ActiveRouteFileCount > 0);
	--ActiveRouteFileCount;
}

int32 FRouteWaypointFileActivityTracker::GetActiveRouteFileCount() const
{
	FScopeLock Lock(&Mutex);
	return ActiveRouteFileCount;
}

int32 FRouteWaypointFileActivityTracker::GetPeakActiveRouteFileCount() const
{
	FScopeLock Lock(&Mutex);
	return PeakActiveRouteFileCount;
}

bool RouteWaypointGenerator::BuildContext(
	const double ShowOriginLatitude,
	const double ShowOriginLongitude,
	FRouteWaypointGeneratorContext& OutContext,
	FRouteWaypointGeneratorError& OutError)
{
	OutContext = FRouteWaypointGeneratorContext{};
	OutError.Reset();
	if (!NormalizeOriginCoordinate(ShowOriginLatitude, -90.0, 90.0,
			OutContext.NormalizedShowOriginLatitudeE7)
		|| !NormalizeOriginCoordinate(ShowOriginLongitude, -180.0, 180.0,
			OutContext.NormalizedShowOriginLongitudeE7))
	{
		OutContext = FRouteWaypointGeneratorContext{};
		OutError.Set(ERouteWaypointGeneratorErrorCode::InvalidOrigin,
			TEXT("Show origin must be finite, in range, and representable as signed E7."));
		return false;
	}

	TArray<uint8> CanonicalBytes;
	CanonicalBytes.Reserve(
		static_cast<int32>(sizeof(RouteWaypointGeneratorContract::GeneratorInputDomainTag) - 1)
		+ 2 + 4 + 4);
	CanonicalBytes.Append(
		reinterpret_cast<const uint8*>(RouteWaypointGeneratorContract::GeneratorInputDomainTag),
		static_cast<int32>(sizeof(RouteWaypointGeneratorContract::GeneratorInputDomainTag) - 1));
	AppendBigEndianU16(CanonicalBytes, RouteWaypointGeneratorContract::GeneratorInputSchemaVersion);
	AppendBigEndianI32(CanonicalBytes, OutContext.NormalizedShowOriginLatitudeE7);
	AppendBigEndianI32(CanonicalBytes, OutContext.NormalizedShowOriginLongitudeE7);

	FString DigestError;
	if (!FRouteArtifactSha256::HashBytes(
			CanonicalBytes, OutContext.GeneratorInputDigest, DigestError))
	{
		OutContext = FRouteWaypointGeneratorContext{};
		OutError.Set(ERouteWaypointGeneratorErrorCode::InternalError,
			TEXT("Generator input digest computation failed."));
		return false;
	}

	const double NormalizedLatitude =
		static_cast<double>(OutContext.NormalizedShowOriginLatitudeE7)
		/ RouteWaypointGeneratorContract::DecimalScale;
	const double NormalizedLongitude =
		static_cast<double>(OutContext.NormalizedShowOriginLongitudeE7)
		/ RouteWaypointGeneratorContract::DecimalScale;
	FString GeoError;
	if (!RouteAssignmentGeo::BuildLocalEnuOriginContext(
			NormalizedLatitude, NormalizedLongitude, 0.0, OutContext.GeoOrigin, GeoError))
	{
		OutContext = FRouteWaypointGeneratorContext{};
		OutError.Set(ERouteWaypointGeneratorErrorCode::InvalidOrigin,
			TEXT("Normalized show origin could not create a WGS84 ENU context."));
		return false;
	}
	return true;
}

FString RouteWaypointGenerator::FormatScaledDecimal(const int64 ScaledValue)
{
	std::array<uint8, 32> Bytes{};
	size_t ByteCount = 0;
	if (!dronegcs::wpl110::FormatScaledDecimal(ScaledValue, Bytes, ByteCount))
	{
		return FString();
	}
	FString Result;
	Result.Reserve(static_cast<int32>(ByteCount));
	for (size_t Index = 0; Index < ByteCount; ++Index)
	{
		Result.AppendChar(static_cast<TCHAR>(Bytes[Index]));
	}
	return Result;
}

bool RouteWaypointGenerator::FormatWaypointRow(
	const int32 Sequence,
	const uint8 Red,
	const uint8 Green,
	const uint8 Blue,
	const int32 LatitudeE7,
	const int32 LongitudeE7,
	const int64 AltitudeE7,
	TArray<uint8>& OutRowBytes,
	FRouteWaypointGeneratorError& OutError)
{
	OutRowBytes.Reset();
	OutError.Reset();
	if (Sequence < 0)
	{
		OutError.Set(ERouteWaypointGeneratorErrorCode::InternalError,
			TEXT("Waypoint sequence must be non-negative."));
		return false;
	}
	dronegcs::wpl110::Row Row;
	Row.Sequence = Sequence;
	Row.Red = Red;
	Row.Green = Green;
	Row.Blue = Blue;
	Row.LatitudeE7 = LatitudeE7;
	Row.LongitudeE7 = LongitudeE7;
	Row.AltitudeE7 = AltitudeE7;
	dronegcs::wpl110::FormattedRow Formatted;
	if (!dronegcs::wpl110::FormatRow(Row, Formatted))
	{
		OutError.Set(ERouteWaypointGeneratorErrorCode::InternalError,
			TEXT("Waypoint row exceeds the frozen v1 byte bound."));
		return false;
	}
	OutRowBytes.Append(Formatted.Bytes.data(), static_cast<int32>(Formatted.Size));
	return true;
}

FRouteWaypointGenerationResult RouteWaypointGenerator::GenerateRoute(
	const FRouteWaypointGeneratorInput& Input,
	IRouteWaypointByteSink& Sink,
	const TFunction<bool()>& IsCancelled)
{
	int32 RowCount = 0;
	int64 BytesWritten = 0;
	const FRoutePreviewData* const RouteData = Input.RouteData;
	if (RouteData == nullptr || !RouteData->IsLoaded() || !RouteData->SourceIdentity.IsValid())
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::InvalidRouteData,
			TEXT("Route data is not a valid loaded immutable snapshot."));
	}
	const int64 ExpectedSamples = static_cast<int64>(RouteData->FrameCount) * RouteData->DroneCount;
	if (ExpectedSamples <= 0 || ExpectedSamples > MAX_int32
		|| RouteData->PositionsENU.Num() != ExpectedSamples
		|| RouteData->Colors.Num() != ExpectedSamples)
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::InvalidRouteData,
			TEXT("Route sample storage is invalid or exceeds supported container indexing."));
	}
	if (Input.RouteSlotIndex < 0 || Input.RouteSlotIndex >= RouteData->DroneCount)
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::InvalidRouteSlot,
			TEXT("Route slot is outside the loaded source domain."));
	}
	if (!SameSourceIdentity(Input.SourceIdentity, RouteData->SourceIdentity))
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::SourceIdentityMismatch,
			TEXT("Generator source identity does not match the route snapshot."));
	}
	if (!Input.Context.IsValid())
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::InvalidOrigin,
			TEXT("Generator context or normalized origin is invalid."));
	}

	FString SinkError;
	if (!Sink.Write(reinterpret_cast<const uint8*>(dronegcs::wpl110::Header),
			static_cast<int32>(dronegcs::wpl110::HeaderLength), SinkError))
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::FileWriteFailed,
			TEXT("Waypoint header write failed."));
	}
	BytesWritten += dronegcs::wpl110::HeaderLength;

	TArray<uint8> RowBytes;
	RowBytes.Reserve(128);
	FString GeoError;
	for (int32 FrameIndex = 0; FrameIndex < RouteData->FrameCount; ++FrameIndex)
	{
		if (IsCancelled
			&& FrameIndex % RouteWaypointGeneratorContract::CancellationCheckIntervalFrames == 0
			&& IsCancelled())
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::Cancelled,
				TEXT("Route generation was cancelled."));
		}

		const int64 FlatIndex64 = static_cast<int64>(FrameIndex) * RouteData->DroneCount
			+ Input.RouteSlotIndex;
		if (FlatIndex64 < 0 || FlatIndex64 >= ExpectedSamples)
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::InvalidRouteData,
				TEXT("Route sample index overflowed its validated storage."));
		}
		const int32 FlatIndex = static_cast<int32>(FlatIndex64);
		const FVector3f& Position = RouteData->PositionsENU[FlatIndex];
		const double EastMeters = static_cast<double>(Position.X);
		const double NorthMeters = static_cast<double>(Position.Y);
		const double UpMeters = static_cast<double>(Position.Z);
		if (!FMath::IsFinite(EastMeters)
			|| !FMath::IsFinite(NorthMeters)
			|| !FMath::IsFinite(UpMeters))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::NonFinitePosition,
				TEXT("Route position contains a non-finite ENU component."));
		}

		const FLinearColor Color = RouteData->Colors[FlatIndex].GetFloats();
		uint8 Red = 0;
		uint8 Green = 0;
		uint8 Blue = 0;
		if (!dronegcs::wpl110::QuantizeRgb(
				Color.R, Color.G, Color.B, Color.A, Red, Green, Blue))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::InvalidColor,
				TEXT("Route color contains invalid linear RGBA values."));
		}

		RouteAssignmentGeo::FGeodeticPosition Geodetic;
		if (!RouteAssignmentGeo::LocalEnuToGeodetic(
				Input.Context.GeoOrigin, EastMeters, NorthMeters, 0.0,
				Geodetic, GeoError))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::GeoConversionFailed,
				TEXT("WGS84 Local ENU conversion failed."));
		}

		int32 LatitudeE7 = 0;
		int32 LongitudeE7 = 0;
		int64 AltitudeE7 = 0;
		if (!QuantizeGeodeticE7(Geodetic.LatitudeDegrees, -90.0, 90.0, LatitudeE7)
			|| !QuantizeGeodeticE7(Geodetic.LongitudeDegrees, -180.0, 180.0, LongitudeE7)
			|| !QuantizeAltitudeE7(UpMeters, AltitudeE7))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::NumericOverflow,
				TEXT("Waypoint coordinate cannot be represented by the v1 E7 contract."));
		}

		FRouteWaypointGeneratorError RowError;
		if (!FormatWaypointRow(
				FrameIndex, Red, Green, Blue, LatitudeE7, LongitudeE7, AltitudeE7,
				RowBytes, RowError))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				RowError.Code, TEXT("Waypoint row formatting failed."));
		}
		if (!Sink.Write(CrLf, UE_ARRAY_COUNT(CrLf), SinkError))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::FileWriteFailed,
				TEXT("Waypoint row separator write failed."));
		}
		BytesWritten += UE_ARRAY_COUNT(CrLf);
		if (!Sink.Write(RowBytes.GetData(), RowBytes.Num(), SinkError))
		{
			return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
				ERouteWaypointGeneratorErrorCode::FileWriteFailed,
				TEXT("Waypoint row write failed."));
		}
		BytesWritten += RowBytes.Num();
		++RowCount;
	}

	if (!Sink.Finalize(SinkError))
	{
		return MakeFailure(Input.RouteSlotIndex, RowCount, BytesWritten,
			ERouteWaypointGeneratorErrorCode::FileWriteFailed,
			TEXT("Waypoint file finalization failed."));
	}

	FRouteWaypointGenerationResult Result;
	Result.bSuccess = true;
	Result.RouteSlotIndex = Input.RouteSlotIndex;
	Result.RowCount = RowCount;
	Result.BytesWritten = BytesWritten;
	return Result;
}

FRouteWaypointGenerationResult RouteWaypointGenerator::GenerateRouteToStagingFile(
	FRouteArtifactWorkspaceManager& WorkspaceManager,
	const FRouteWaypointGeneratorInput& Input,
	const TFunction<bool()>& IsCancelled,
	FRouteWaypointFileActivityTracker* const FileActivityTracker)
{
	FString SafePath;
	FString PathError;
	if (!WorkspaceManager.ResolveSafeStagingRoutePath(
			Input.RouteSlotIndex, SafePath, PathError))
	{
		return MakeFailure(Input.RouteSlotIndex, 0, 0,
			ERouteWaypointGeneratorErrorCode::PathRejected,
			TEXT("Workspace rejected the staging route path."));
	}

	TUniquePtr<IFileHandle> Handle(
		FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*SafePath, false, false));
	if (!Handle)
	{
		return MakeFailure(Input.RouteSlotIndex, 0, 0,
			ERouteWaypointGeneratorErrorCode::FileOpenFailed,
			TEXT("Staging route file could not be opened."));
	}
	if (FileActivityTracker)
	{
		FileActivityTracker->NotifyRouteFileOpened();
	}
	FRouteWaypointGenerationResult Result;
	{
		FBufferedRouteWaypointFileSink Sink(MoveTemp(Handle));
		Result = GenerateRoute(Input, Sink, IsCancelled);
	}
	if (FileActivityTracker)
	{
		FileActivityTracker->NotifyRouteFileClosed();
	}
	return Result;
}
