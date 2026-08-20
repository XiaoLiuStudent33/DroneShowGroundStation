#pragma once

#include "CoreMinimal.h"
#include "RouteArtifactTypes.h"
#include "RouteAssignmentGeo.h"
#include "RoutePreviewData.h"
#include "Wpl110Core.h"

namespace RouteWaypointGeneratorContract
{
	inline constexpr uint16 GeneratorInputSchemaVersion = 1;
	// If NPZ position/color precision, channel interpretation, or decoding changes
	// can alter output bytes, bump this generator version or the input schema.
	inline constexpr const TCHAR* WaypointGeneratorVersion =
		TEXT("qgc-wpl110-wgs84-relative-rgb-saturate-v2");
	inline constexpr int64 HeaderBytes = dronegcs::wpl110::HeaderLength;
	inline constexpr int64 RowCrLfBytes = 2;
	inline constexpr int64 MaxSequenceCharacters = 10;
	inline constexpr int64 FixedCurrentCharacters = 1;
	inline constexpr int64 FixedFrameCharacters = 1;
	inline constexpr int64 FixedCommandCharacters = 2;
	inline constexpr int64 MaxRgbCharacters = 3 * 3;
	inline constexpr int64 FixedParam4Characters = 1;
	inline constexpr int64 MaxLatitudeCharacters = 11;
	inline constexpr int64 MaxLongitudeCharacters = 12;
	inline constexpr int64 MaxAltitudeCharacters = 21;
	inline constexpr int64 FixedAutoContinueCharacters = 1;
	inline constexpr int64 TabCharacters = 11;
	inline constexpr int64 MaxWaypointRowBytesV1 = dronegcs::wpl110::MaxRowBytes;
	inline constexpr int64 MaxWaypointRowBytesIncludingCrLfV1 =
		RowCrLfBytes + MaxWaypointRowBytesV1;
	inline constexpr ANSICHAR GeneratorInputDomainTag[] =
		"DRONEGCS_WAYPOINT_GENERATOR_INPUT";
	inline constexpr int32 FractionDigits = dronegcs::wpl110::FractionDigits;
	inline constexpr int64 DecimalScale = dronegcs::wpl110::DecimalScale;
	inline constexpr int32 FileBufferBytes = 64 * 1024;
	inline constexpr int32 CancellationCheckIntervalFrames = 64;
}

enum class ERouteWaypointGeneratorErrorCode : uint8
{
	None,
	InvalidRouteData,
	InvalidRouteSlot,
	InvalidOrigin,
	SourceIdentityMismatch,
	NonFinitePosition,
	InvalidColor,
	GeoConversionFailed,
	NumericOverflow,
	PathRejected,
	FileOpenFailed,
	FileWriteFailed,
	Cancelled,
	InternalError
};

struct DRONESHOWPLATFORM1_API FRouteWaypointGeneratorError
{
	ERouteWaypointGeneratorErrorCode Code = ERouteWaypointGeneratorErrorCode::None;
	FString Diagnostic;

	void Reset()
	{
		Code = ERouteWaypointGeneratorErrorCode::None;
		Diagnostic.Reset();
	}

	void Set(ERouteWaypointGeneratorErrorCode InCode, const TCHAR* InDiagnostic)
	{
		Code = InCode;
		Diagnostic = InDiagnostic;
	}
};

/** Immutable generator inputs shared by all routes from one source/origin. */
struct DRONESHOWPLATFORM1_API FRouteWaypointGeneratorContext
{
	uint16 GeneratorInputSchemaVersion = RouteWaypointGeneratorContract::GeneratorInputSchemaVersion;
	FString WaypointGeneratorVersion = RouteWaypointGeneratorContract::WaypointGeneratorVersion;
	int32 NormalizedShowOriginLatitudeE7 = 0;
	int32 NormalizedShowOriginLongitudeE7 = 0;
	FRouteArtifactDigest256 GeneratorInputDigest;
	RouteAssignmentGeo::FLocalEnuOriginContext GeoOrigin;

	bool IsValid() const
	{
		return GeneratorInputSchemaVersion == RouteWaypointGeneratorContract::GeneratorInputSchemaVersion
			&& WaypointGeneratorVersion == RouteWaypointGeneratorContract::WaypointGeneratorVersion
			&& GeneratorInputDigest.IsValid()
			&& GeoOrigin.bIsValid;
	}
};

/** One route request. RouteData remains a caller-owned immutable snapshot. */
struct DRONESHOWPLATFORM1_API FRouteWaypointGeneratorInput
{
	const FRoutePreviewData* RouteData = nullptr;
	int32 RouteSlotIndex = INDEX_NONE;
	FRouteArtifactSourceIdentity SourceIdentity;
	FRouteWaypointGeneratorContext Context;
};

struct DRONESHOWPLATFORM1_API FRouteWaypointGenerationResult
{
	bool bSuccess = false;
	int32 RouteSlotIndex = INDEX_NONE;
	int32 RowCount = 0;
	int64 BytesWritten = 0;
	int32 ClampedSampleCount = 0;
	int64 ClampedChannelCount = 0;
	ERouteWaypointGeneratorErrorCode ErrorCode = ERouteWaypointGeneratorErrorCode::None;
	FString Diagnostic;
};

/** Testable streaming boundary. Production filesystem access is adapter-only. */
class DRONESHOWPLATFORM1_API IRouteWaypointByteSink
{
public:
	virtual ~IRouteWaypointByteSink() = default;
	virtual bool Write(const uint8* Data, int32 ByteCount, FString& OutError) = 0;
	virtual bool Finalize(FString& OutError) = 0;
};

/** Per-batch file-handle instrumentation; updated only on route open/close. */
class DRONESHOWPLATFORM1_API FRouteWaypointFileActivityTracker final
{
public:
	void NotifyRouteFileOpened();
	void NotifyRouteFileClosed();
	int32 GetActiveRouteFileCount() const;
	int32 GetPeakActiveRouteFileCount() const;

private:
	mutable FCriticalSection Mutex;
	int32 ActiveRouteFileCount = 0;
	int32 PeakActiveRouteFileCount = 0;
};
