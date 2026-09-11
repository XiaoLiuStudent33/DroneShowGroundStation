#pragma once
#include "CoreMinimal.h"

// Scale control sizes uniformly, but distribute surplus space to the center
// viewport and stretchable panels, never to an outer letterbox.
struct FGCSMockWorkbenchLayout
{
    static constexpr float ControlScale = 0.85f;
    float Scale = 1.f;
    float Width = 1672.f;
    float Height = 940.f;
    float ExtraWidth = 0.f;
    float ExtraHeight = 0.f;

    static FGCSMockWorkbenchLayout FromViewport(FVector2D Size)
    {
        FGCSMockWorkbenchLayout Result;
        Size.X = FMath::Max(1.0, Size.X);
        Size.Y = FMath::Max(1.0, Size.Y);
        Result.Scale = FMath::Min(Size.X / 1672.0, Size.Y / 940.0) * ControlScale;
        Result.Width = Size.X / Result.Scale;
        Result.Height = Size.Y / Result.Scale;
        Result.ExtraWidth = FMath::Max(0.f, Result.Width - 1672.f);
        Result.ExtraHeight = FMath::Max(0.f, Result.Height - 940.f);
        return Result;
    }

    FVector2D ToDesign(FVector2D LocalPoint) const { return LocalPoint / Scale; }
};
