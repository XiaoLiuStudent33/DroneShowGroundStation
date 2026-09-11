#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "GCSSimulationSubsystem.generated.h"

class UUserWidget;
class UButton;
class USpinBox;
class UTextBlock;
class FGCSSimulationWorker;

/** Local UDP telemetry simulator. Never changes flight/route data. */
UCLASS()
class DRONESHOWPLATFORM1_API UGCSSimulationSubsystem : public UGameInstanceSubsystem
{
 GENERATED_BODY()
public:
 virtual void Deinitialize() override;
 void BindUI(UUserWidget* Widget);
 void UnbindUI();
 bool StartSimulation(int32 TargetPort, int32 Count, float InSpacingMeters = 0.5f);
 void StopSimulation();
 bool IsRunning() const { return Worker != nullptr; }
private:
 friend class FGCSSimulationTest;
 FGCSSimulationWorker* Worker = nullptr;
 TWeakObjectPtr<UButton> StartButton, StopButton;
 TWeakObjectPtr<USpinBox> PortInput, CountInput, SpacingInput;
 TWeakObjectPtr<UTextBlock> StatusText;
 FTimerHandle StatusTimer;
 FString Status = TEXT("状态：未启动");
 int32 Port = 10001, DroneCount = 100;
 float SpacingMeters = 0.5f;
 UFUNCTION() void OnStart();
 UFUNCTION() void OnStop();
 void Refresh();
};
