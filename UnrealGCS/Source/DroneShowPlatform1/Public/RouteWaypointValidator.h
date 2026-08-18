#pragma once

#include "CoreMinimal.h"
#include "RouteWaypointGeneratorTypes.h"

enum class ERouteWaypointValidationLevel : uint8
{
	WaypointSemantic,
	SourceBound
};

enum class ERouteWaypointValidationErrorCode : uint8
{
	None,
	InvalidInput,
	FileOpenFailed,
	FileSizeInvalid,
	FileReadFailed,
	FileChangedDuringRead,
	Cancelled,
	HashFailed,
	SemanticParseFailed,
	SourcePositionInvalid,
	SourceColorInvalid,
	SourceGeoConversionFailed,
	SourceNumericOverflow,
	SourceRedMismatch,
	SourceGreenMismatch,
	SourceBlueMismatch,
	SourceLatitudeMismatch,
	SourceLongitudeMismatch,
	SourceAltitudeMismatch
};

/** Immutable source Authority captured with the generation snapshot. */
struct DRONESHOWPLATFORM1_API FRouteWaypointSourceValidationContext
{
	TSharedPtr<const FRoutePreviewData, ESPMode::ThreadSafe> RouteData;
	FRouteArtifactSourceIdentity SourceIdentity;
	int64 RouteDatasetGeneration = 0;
	FRouteWaypointGeneratorContext GeneratorContext;
	int32 FrameCount = 0;
	int32 RouteCount = 0;

	bool IsValid() const;
};

struct DRONESHOWPLATFORM1_API FRouteWaypointValidationResult
{
	bool bSuccess = false;
	ERouteWaypointValidationLevel Level = ERouteWaypointValidationLevel::WaypointSemantic;
	ERouteWaypointValidationErrorCode ErrorCode = ERouteWaypointValidationErrorCode::None;
	dronegcs::wpl110::ErrorCode ParserErrorCode = dronegcs::wpl110::ErrorCode::None;
	int32 RouteSlotIndex = INDEX_NONE;
	int32 RowIndex = INDEX_NONE;
	int32 FieldIndex = INDEX_NONE;
	int32 RowCount = 0;
	int64 FileByteSize = 0;
	FRouteArtifactDigest256 FileDigest;
	FString Diagnostic;
};

namespace RouteWaypointValidator
{
	/** Opens and reads the physical file once; the same bytes feed SHA-256 and parsing. */
	DRONESHOWPLATFORM1_API bool ValidateAndHashFileStreaming(
		const FString& FilePath,
		int32 RouteSlotIndex,
		int32 ExpectedFrameCount,
		ERouteWaypointValidationLevel Level,
		const FRouteWaypointSourceValidationContext* SourceContext,
		FRouteWaypointValidationResult& OutResult,
		const TFunction<bool()>& IsCancelled = TFunction<bool()>());
}
