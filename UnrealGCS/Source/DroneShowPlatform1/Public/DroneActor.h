/**
 * @brief 旧单无人机 Actor 可视化和本地状态模拟。
 *
 * 分类：Legacy Single-Drone Visualization。
 * 不参与当前 Fleet/Niagara 链；当前地图/Blueprint 仍有引用，Runtime 仍可能 Tick。
 * 本轮不得删除；千机 Production 可视化由 DroneFleetVisualizationActor 承担。
 */
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"
#include "DroneActor.generated.h"

// 无人机状态
UENUM(BlueprintType)
enum class EDroneState : uint8
{
    Disarmed      UMETA(DisplayName = "Disarmed"),
    Armed         UMETA(DisplayName = "Armed"),
    Guided        UMETA(DisplayName = "Guided"),
    TakingOff     UMETA(DisplayName = "Taking Off"),
    Hovering      UMETA(DisplayName = "Hovering"),
    Landing       UMETA(DisplayName = "Landing"),
    RTL           UMETA(DisplayName = "Return Home")
};

UCLASS()
class DRONESHOWPLATFORM1_API ADroneActor : public AActor
{
    GENERATED_BODY()

public:

    ADroneActor();

protected:

    virtual void BeginPlay() override;

public:

    virtual void Tick(float DeltaTime) override;

    //-------------------------------
    // 蓝图控制接口
    //-------------------------------

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void ARM();

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void DISARM();

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void Guided();

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void TakeOff();

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void Land();

    UFUNCTION(BlueprintCallable, Category = "Drone Control")
    void RTL();

    //-------------------------------
    // 获取状态
    //-------------------------------

    UFUNCTION(BlueprintPure, Category = "Drone Control")
    EDroneState GetDroneState() const
    {
        return DroneState;
    }

private:

    UPROPERTY(BlueprintReadOnly,
        Category = "Drone",
        meta = (AllowPrivateAccess = "true"))
    EDroneState DroneState;

    UPROPERTY(EditAnywhere,
        BlueprintReadOnly,
        Category = "Drone",
        meta = (AllowPrivateAccess = "true"))
    UStaticMeshComponent* DroneMesh;

    float StartHeight;

    float TimeCounter;
};