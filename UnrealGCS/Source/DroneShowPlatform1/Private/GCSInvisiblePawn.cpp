#include "GCSInvisiblePawn.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DroneFleetVisualizationActor.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GCSUIBridgeSubsystem.h"
#include "InputCoreTypes.h"
#include "Misc/App.h"

AGCSInvisiblePawn::AGCSInvisiblePawn()
{
 bAddDefaultMovementBindings = false;
 PrimaryActorTick.bCanEverTick = true;
 bUseControllerRotationYaw = bUseControllerRotationPitch = bUseControllerRotationRoll = false;
 SetActorEnableCollision(false);
 if (auto* Mesh = GetMeshComponent())
 {
  Mesh->SetVisibility(false, true);
  Mesh->SetHiddenInGame(true, true);
 }
 OrbitCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("OrbitCamera"));
 OrbitCamera->SetupAttachment(GetRootComponent());
 OrbitCamera->bUsePawnControlRotation = false;
 OrbitCamera->FieldOfView = 60.0f;
}

void AGCSInvisiblePawn::BeginPlay()
{
 Super::BeginPlay();
 if (auto* Movement = GetMovementComponent()) Movement->Deactivate();
 ResetOrbitView();
}

void AGCSInvisiblePawn::SetupPlayerInputComponent(UInputComponent* Input)
{
 Super::SetupPlayerInputComponent(Input);
 // No default WASD/gamepad/mouse-look bindings. Poll only scene gestures below.
}

bool AGCSInvisiblePawn::SetOrbitBasis(FVector Center, FVector East, FVector Up)
{
 if (Center.ContainsNaN() || East.ContainsNaN() || Up.ContainsNaN()) return false;
 Up = Up.GetSafeNormal();
 East = (East - Up * FVector::DotProduct(East, Up)).GetSafeNormal();
 if (Up.IsNearlyZero() || East.IsNearlyZero()) return false;
 OrbitCenter = LockedCenter = Center;
 FramingOffset = FVector2D::ZeroVector;
 LocalEast = East;
 LocalUp = Up;
 UpdateOrbitCamera();
 return true;
}

void AGCSInvisiblePawn::ResetOrbitView()
{
 if (!GetWorld()) return;
 FVector Center = FVector::ZeroVector, East = FVector::XAxisVector, Up = FVector::ZAxisVector;
 FBox MapBounds(ForceInit);
 for (TActorIterator<AActor> It(GetWorld()); It; ++It)
 {
  if (It->ActorHasTag(TEXT("GCS.SceneMap")) || It->GetClass()->GetName().StartsWith(TEXT("BP_GCS_2DMapGround")))
  {
   FVector O, E;
   It->GetActorBounds(false, O, E);
   MapBounds += FBox(O - E, O + E);
   // Map-only fallback, until an explicit geographic basis is available.
   East = It->GetActorForwardVector();
   Up = It->GetActorUpVector();
  }
 }
 if (MapBounds.IsValid) Center = MapBounds.GetCenter();
 for (TActorIterator<ADroneFleetVisualizationActor> It(GetWorld()); It; ++It)
 {
  FVector O, E, N, U;
  if (It->TryGetGeographicTangentBasis(O, E, N, U)) { Center = O; East = E; Up = U; break; }
 }
 Yaw = -45.0f;
 Elevation = 45.0f;
 bOriginLocked = true;
 // Bounding sphere fit handles arbitrary aspect and an off-center geographic origin.
 double Radius = 40000.0;
 if (MapBounds.IsValid)
 {
  Radius = 1.0;
  for (int32 I = 0; I < 8; ++I)
  {
   const FVector Corner((I & 1) ? MapBounds.Max.X : MapBounds.Min.X,
    (I & 2) ? MapBounds.Max.Y : MapBounds.Min.Y, (I & 4) ? MapBounds.Max.Z : MapBounds.Min.Z);
   Radius = FMath::Max(Radius, FVector::Distance(Center, Corner));
  }
 }
 int32 Width = 0, Height = 0;
 if (auto* PC = Cast<APlayerController>(GetController())) PC->GetViewportSize(Width, Height);
 const double Aspect = Height > 0 ? static_cast<double>(Width) / Height : 16.0 / 9.0;
 const double HalfHorizontal = FMath::DegreesToRadians(OrbitCamera->FieldOfView * 0.5);
 const double HalfVertical = FMath::Atan(FMath::Tan(HalfHorizontal) / FMath::Max(Aspect, 0.1));
 Distance = FMath::Max(100.0, Radius * 1.15 / FMath::Sin(FMath::Min(HalfHorizontal, HalfVertical)));
 SetOrbitBasis(Center, East, Up);
}

void AGCSInvisiblePawn::SetOrbitOriginLocked(bool bLocked)
{
 if (bLocked == bOriginLocked) return;
 if (!bLocked)
  OrbitCenter += OrbitCamera->GetRightVector() * FramingOffset.X + OrbitCamera->GetUpVector() * FramingOffset.Y;
 else OrbitCenter = LockedCenter;
 FramingOffset = FVector2D::ZeroVector;
 bOriginLocked = bLocked;
 UpdateOrbitCamera();
}

void AGCSInvisiblePawn::RotateOrbit(float YawDelta, float PitchDelta)
{
 if (!FMath::IsFinite(YawDelta) || !FMath::IsFinite(PitchDelta)) return;
 Yaw = FMath::UnwindDegrees(Yaw + YawDelta);
 Elevation = FMath::UnwindDegrees(Elevation + PitchDelta);
 UpdateOrbitCamera();
}

void AGCSInvisiblePawn::ZoomOrbit(float WheelDelta)
{
 if (!FMath::IsFinite(WheelDelta)) return;
 Distance = FMath::Clamp(Distance * FMath::Pow(0.85, FMath::Clamp(static_cast<double>(WheelDelta), -20.0, 20.0)), 100.0, 1.e9);
 UpdateOrbitCamera();
}

void AGCSInvisiblePawn::PanOrbit(float RightDelta, float UpDelta)
{
 if (!FMath::IsFinite(RightDelta) || !FMath::IsFinite(UpDelta)) return;
 if (bOriginLocked) FramingOffset += FVector2D(RightDelta, UpDelta);
 else OrbitCenter += OrbitCamera->GetRightVector() * RightDelta + OrbitCamera->GetUpVector() * UpDelta;
 UpdateOrbitCamera();
}

void AGCSInvisiblePawn::UpdateOrbitCamera()
{
 OrbitCamera->OrthoWidth=2.f*Distance*FMath::Tan(FMath::DegreesToRadians(OrbitCamera->FieldOfView*.5f));
 if(IsOrthographicView())
 {
  // The orbit component can be far from its pawn root; automatic target-plane
  // correction would clip a Cesium-local fleet. Clip relative to the actual camera.
  OrbitCamera->SetAutoCalculateOrthoPlanes(false);
  OrbitCamera->SetUpdateOrthoPlanes(false);
  OrbitCamera->SetOrthoNearClipPlane(1.f);
  OrbitCamera->SetOrthoFarClipPlane(FMath::Max(100000.f,float(Distance*4.0)));
 }
 const FVector Side = FVector::CrossProduct(LocalUp, LocalEast).GetSafeNormal();
 const double A = FMath::DegreesToRadians(Yaw), B = FMath::DegreesToRadians(Elevation);
 const FVector Outward = (LocalEast * FMath::Cos(A) + Side * FMath::Sin(A)) * FMath::Cos(B) + LocalUp * FMath::Sin(B);
 const FVector CameraUp = -(LocalEast * FMath::Cos(A) + Side * FMath::Sin(A)) * FMath::Sin(B) + LocalUp * FMath::Cos(B);
 const FRotationMatrix ViewRotation(FRotationMatrix::MakeFromXZ(-Outward, CameraUp).Rotator());
 const FVector Offset = ViewRotation.GetUnitAxis(EAxis::Y) * FramingOffset.X
  + ViewRotation.GetUnitAxis(EAxis::Z) * FramingOffset.Y;
 OrbitCamera->SetWorldLocationAndRotation(OrbitCenter + Outward * Distance + Offset, ViewRotation.Rotator());
}

bool AGCSInvisiblePawn::IsSceneInputAllowed() const
{
 const auto* PC = Cast<APlayerController>(GetController());
 if (!PC || PC->GetViewTarget() != this || PC->IsLookInputIgnored()
  || PC->IsInputKeyDown(EKeys::LeftMouseButton) || !FSlateApplication::IsInitialized() || !FApp::HasFocus()) return false;
 if (bDragging) return true; // UI hit testing only decides who owns the gesture at press.
 if (bWorkbenchInputBlocked) return false;
 const auto* GI = GetGameInstance();
 const auto* Bridge = GI ? GI->GetSubsystem<UGCSUIBridgeSubsystem>() : nullptr;
 return Bridge && !Bridge->IsPointerOverBlockingGCSUI(FSlateApplication::Get().GetCursorPos());
}

void AGCSInvisiblePawn::Tick(float DeltaSeconds)
{
 Super::Tick(DeltaSeconds);
 auto* PC = Cast<APlayerController>(GetController());
 if (!PC) return;
 const bool Middle = PC->IsInputKeyDown(EKeys::MiddleMouseButton);
 float X = 0, Y = 0;
 const bool Allowed = IsSceneInputAllowed() && PC->GetMousePosition(X, Y);
 if (!Allowed) bDragging = false;
 else
 {
  const FVector2D Mouse(X, Y);
  if (Middle && !bMiddleWasDown) { bDragging = true; LastMouse = Mouse; }
  if (Middle && bDragging)
  {
   const FVector2D Delta = Mouse - LastMouse;
   if (PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift))
   {
    int32 W, H; PC->GetViewportSize(W, H);
    const double Scale = 2.0 * Distance * FMath::Tan(FMath::DegreesToRadians(OrbitCamera->FieldOfView * 0.5)) / FMath::Max(W, 1);
    PanOrbit(-Delta.X * Scale, Delta.Y * Scale);
   }
   else RotateOrbit(-Delta.X * 0.25f, Delta.Y * 0.25f);
   // Recenter within the viewport after each delta so the desktop edge cannot cap rotation.
   PC->SetMouseLocation(FMath::RoundToInt(LastMouse.X), FMath::RoundToInt(LastMouse.Y));
  }
  ZoomOrbit(PC->GetInputAnalogKeyState(EKeys::MouseWheelAxis));
  if (PC->WasInputKeyJustPressed(EKeys::Home)) ResetOrbitView();
 }
 if (!Middle) bDragging = false;
 bMiddleWasDown = Middle;
}

void AGCSInvisiblePawn::FitMapBounds(const FBox& Bounds, FVector East, FVector Up)
{
 if (!Bounds.IsValid) return;
 int32 W=0,H=0;
 if (auto* PC=Cast<APlayerController>(GetController())) PC->GetViewportSize(W,H);
 const double Aspect=H>0 ? double(W)/H : 16.0/9.0;
 const double Horizontal=FMath::DegreesToRadians(OrbitCamera->FieldOfView*0.5);
 const double Vertical=FMath::Atan(FMath::Tan(Horizontal)/FMath::Max(Aspect,0.1));
 Distance=FMath::Max(100.0,Bounds.GetExtent().Size()*1.15/FMath::Sin(FMath::Min(Horizontal,Vertical)));
 Yaw=-45; Elevation=45; bOriginLocked=false;
 SetOrbitBasis(Bounds.GetCenter(),East,Up);
}

void AGCSInvisiblePawn::SetOrthographicView(bool bOrthographic)
{
 OrbitCamera->SetProjectionMode(bOrthographic?ECameraProjectionMode::Orthographic:ECameraProjectionMode::Perspective);
 UpdateOrbitCamera();
}
bool AGCSInvisiblePawn::IsOrthographicView() const
{
 return OrbitCamera->ProjectionMode==ECameraProjectionMode::Orthographic;
}
