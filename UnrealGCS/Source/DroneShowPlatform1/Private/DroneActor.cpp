/**
 * @brief 实现旧单机 Actor 的 ARM/Takeoff/Hover/Land 动画和每帧旋转。
 *
 * 分类：Legacy Single-Drone Visualization。
 * 当前地图 External Actor 仍引用该类，因此 Runtime 可能 Tick，但它不属于 Fleet/Niagara Production 链。
 */
#include "DroneActor.h"

ADroneActor::ADroneActor()
{
    PrimaryActorTick.bCanEverTick = true;

    DroneMesh =
        CreateDefaultSubobject<UStaticMeshComponent>(TEXT("DroneMesh"));

    RootComponent = DroneMesh;

    DroneState = EDroneState::Disarmed;
}

void ADroneActor::BeginPlay()
{
    Super::BeginPlay();

    StartHeight = GetActorLocation().Z;

    TimeCounter = 0.0f;
}

void ADroneActor::Tick(float DeltaTime)
{
    Super::Tick(DeltaTime);

    TimeCounter += DeltaTime;

    FVector NewLocation = GetActorLocation();

    switch (DroneState)
    {
    case EDroneState::Disarmed:
        break;

    case EDroneState::Armed:
        break;

    case EDroneState::Guided:
        break;

    case EDroneState::TakingOff:

        NewLocation.Z += DeltaTime * 100.f;

        if (NewLocation.Z >= StartHeight + 300.f)
        {
            NewLocation.Z = StartHeight + 300.f;

            DroneState = EDroneState::Hovering;

            UE_LOG(LogTemp, Warning, TEXT("Drone Hover"));
        }

        break;

    case EDroneState::Hovering:

        NewLocation.Z =
            StartHeight +
            300.f +
            FMath::Sin(TimeCounter * 2.f) * 5.f;

        break;

    case EDroneState::Landing:

        NewLocation.Z -= DeltaTime * 80.f;

        if (NewLocation.Z <= StartHeight)
        {
            NewLocation.Z = StartHeight;

            DroneState = EDroneState::Disarmed;

            UE_LOG(LogTemp, Warning, TEXT("Drone Landed"));
        }

        break;

    case EDroneState::RTL:

        // 后续实现返航
        break;
    }

    SetActorLocation(NewLocation);

    // 模拟旋转

    FRotator Rotation = GetActorRotation();

    Rotation.Yaw += DeltaTime * 30.f;

    SetActorRotation(Rotation);
}

//=================================================
// 控制函数
//=================================================

void ADroneActor::ARM()
{
    if (DroneState == EDroneState::Disarmed)
    {
        DroneState = EDroneState::Armed;

        UE_LOG(LogTemp, Warning, TEXT("Drone Armed"));

        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                -1,
                2.f,
                FColor::Green,
                TEXT("Drone Armed"));
        }
    }
}

void ADroneActor::DISARM()
{
    DroneState = EDroneState::Disarmed;

    UE_LOG(LogTemp, Warning, TEXT("Drone Disarmed"));

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(
            -1,
            2.f,
            FColor::Red,
            TEXT("Drone Disarmed"));
    }
}

void ADroneActor::Guided()
{
    if (DroneState == EDroneState::Armed)
    {
        DroneState = EDroneState::Guided;

        UE_LOG(LogTemp, Warning, TEXT("Drone Guided"));

        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                -1,
                2.f,
                FColor::Blue,
                TEXT("Drone Guided"));
        }
    }
}

void ADroneActor::TakeOff()
{
    if (DroneState == EDroneState::Guided)
    {
        DroneState = EDroneState::TakingOff;

        UE_LOG(LogTemp, Warning, TEXT("Drone Take Off"));

        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                -1,
                2.f,
                FColor::Yellow,
                TEXT("Drone Take Off"));
        }
    }
}

void ADroneActor::Land()
{
    if (DroneState == EDroneState::Hovering)
    {
        DroneState = EDroneState::Landing;

        UE_LOG(LogTemp, Warning, TEXT("Drone Landing"));

        if (GEngine)
        {
            GEngine->AddOnScreenDebugMessage(
                -1,
                2.f,
                FColor::Orange,
                TEXT("Drone Landing"));
        }
    }
}

void ADroneActor::RTL()
{
    DroneState = EDroneState::RTL;

    UE_LOG(LogTemp, Warning, TEXT("Drone RTL"));

    if (GEngine)
    {
        GEngine->AddOnScreenDebugMessage(
            -1,
            2.f,
            FColor::Purple,
            TEXT("Drone RTL"));
    }
}