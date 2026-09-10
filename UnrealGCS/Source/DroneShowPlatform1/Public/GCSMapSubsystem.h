#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSMapSubsystem.generated.h"
class UUserWidget;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UTexture;
class UButton;
class USlider;
class USpinBox;
class UTextBlock;

/** Local map artwork only. Flight coordinates use the immutable scene frame. */
UCLASS()
class DRONESHOWPLATFORM1_API UGCSMapSubsystem : public UGameInstanceSubsystem
{
 GENERATED_BODY()
public:
 virtual void Deinitialize() override;
 void BindUI(UUserWidget* Widget);
 void UnbindUI();
 bool EnsureMap();
 bool GetSceneFrame(FTransform& Out);
 bool SetLayout(double WidthMeters, double EastMeters, double NorthMeters, double Degrees);
 void ImportImage(const FString& Path);
private:
 friend class FGCSMapControlsTest;
 TWeakObjectPtr<UStaticMeshComponent> Plane;
 TWeakObjectPtr<UUserWidget> Panel;
 FTransform SceneFrame;
 FVector MeshSize = FVector(100,100,0);
 double DefaultWidth = 1000, DefaultAspect = 1;
 double Width = 1000, East = 0, North = 0, Angle = 0, Aspect = 1;
 float Opacity = 1;
 bool bSync = false, bLoading = false, bPersistent = true;
 int32 ImportSerial = 0;
 FString ImagePath, Status, TestSettingsPath;
 UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> Material;
 UPROPERTY(Transient) TObjectPtr<UTexture> Texture;
 TWeakObjectPtr<UButton> ImportButton, FitButton, ResetButton;
 TWeakObjectPtr<USlider> OpacitySlider;
 TWeakObjectPtr<USpinBox> WidthInput, EastInput, NorthInput, AngleInput;
 TWeakObjectPtr<UTextBlock> StatusText, OpacityText;
 FTimerHandle SaveTimer;
 FString SettingsPath() const;
 void Restore();
 void Save();
 void ScheduleSave();
 void Apply();
 void SyncUI();
 UFUNCTION() void OnImport();
 UFUNCTION() void OnFit();
 UFUNCTION() void OnReset();
 UFUNCTION() void OnOpacity(float Value);
 UFUNCTION() void OnWidth(float Value);
 UFUNCTION() void OnEast(float Value);
 UFUNCTION() void OnNorth(float Value);
 UFUNCTION() void OnAngle(float Value);
};
