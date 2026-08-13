#pragma once

#include "CoreMinimal.h"

struct FRoutePreviewData;

/** Minimal runtime NPZ/NPY reader for legacy position/color/range arrays. */
class FRoutePreviewNpzLoader final
{
public:
	static bool Load(const FString& FilePath, FRoutePreviewData& OutData, FString& OutError);
};
