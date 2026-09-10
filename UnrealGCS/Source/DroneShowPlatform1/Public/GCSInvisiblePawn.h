#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "GCSInvisiblePawn.generated.h"

class UCameraComponent;

/** Local viewing state only. Never changes the map, Fleet origin or route data. */
UCLASS()
class DRONESHOWPLATFORM1_API AGCSInvisiblePawn final : public ADefaultPawn
{
 GENERATED_BODY()
public:
 AGCSInvisiblePawn();
 virtual void Tick(float DeltaSeconds) override;

 /** Adopt the current geographic basis (map bounds fallback) and fit the map. */
 UFUNCTION(BlueprintCallable, Category="GCS|View") void ResetOrbitView();
 UFUNCTION(BlueprintCallable, Category="GCS|View") void SetOrbitOriginLocked(bool bLocked);
 UFUNCTION(BlueprintPure, Category="GCS|View") bool IsOrbitOriginLocked() const { return bOriginLocked; }
 UFUNCTION(BlueprintPure, Category="GCS|View") FVector GetOrbitCenter() const { return OrbitCenter; }
 /** Explicit view-only basis. Coordinates are UE world units, not GPS. */
 UFUNCTION(BlueprintCallable, Category="GCS|View") bool SetOrbitBasis(FVector Center, FVector East, FVector Up);
 UFUNCTION(BlueprintCallable, Category="GCS|View") void RotateOrbit(float YawDelta, float PitchDelta);
 UFUNCTION(BlueprintCallable, Category="GCS|View") void ZoomOrbit(float WheelDelta);
 UFUNCTION(BlueprintCallable, Category="GCS|View") void PanOrbit(float RightDelta, float UpDelta);

 void FitMapBounds(const FBox& Bounds, FVector East, FVector Up);
protected:
 virtual void BeginPlay() override;
 virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
private:
 UPROPERTY(VisibleAnywhere, Category="GCS|View") TObjectPtr<UCameraComponent> OrbitCamera;
 FVector OrbitCenter = FVector::ZeroVector;
 FVector LockedCenter = FVector::ZeroVector;
 // Camera-plane framing offset; locked orbit still permits Shift+MMB pan.
 FVector2D FramingOffset = FVector2D::ZeroVector;
 FVector LocalEast = FVector::XAxisVector;
 FVector LocalUp = FVector::ZAxisVector;
 double Distance = 100000.0;
 float Yaw = -45.0f;
 float Elevation = 45.0f;
 bool bOriginLocked = true;
 bool bDragging = false;
 bool bMiddleWasDown = false;
 FVector2D LastMouse = FVector2D::ZeroVector;
 void UpdateOrbitCamera();
 bool IsSceneInputAllowed() const;
};
