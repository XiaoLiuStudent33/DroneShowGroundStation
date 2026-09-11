// Copyright Epic Games, Inc. All Rights Reserved.

/**
 * @brief UE Runtime Module 启动入口和主 GCS Widget 装配逻辑。
 *
 * 分类：Production。
 * 输入：PIE/Game World 初始化；输出：WBP_GCS_Main_V2 与 GCSUIBridgeSubsystem 绑定。
 * 调用链：Module Startup -> World Init -> Main Widget -> UI Bridge -> Fleet/Command。
 */

#include "DroneShowPlatform1.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameInstance.h"
#include "GCSUIBridgeSubsystem.h"
#include "GCSLoginSubsystem.h"
#include "GCSLoginWidget.h"
#include "GCSMockWorkbench.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GCSInvisiblePawn.h"
#include "Modules/ModuleManager.h"
#include "TimerManager.h"
#include "UObject/UObjectIterator.h"

class FDroneShowPlatform1Module final : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		FDefaultGameModuleImpl::StartupModule();
		WorldInitializedHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(
			this,
			&FDroneShowPlatform1Module::HandleWorldInitialized);
	}

	virtual void ShutdownModule() override
	{
		if (WorldInitializedHandle.IsValid())
		{
			FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitializedHandle);
			WorldInitializedHandle.Reset();
		}

		FDefaultGameModuleImpl::ShutdownModule();
	}

private:
	static void ConfigureGroundStationInput(APlayerController* PlayerController)
	{
		if (!PlayerController)
		{
			return;
		}

		PlayerController->bShowMouseCursor = true;
		FInputModeGameAndUI InputMode;
		InputMode.SetHideCursorDuringCapture(false);
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}

	static void CreatePrototypeUI(const TWeakObjectPtr<UWorld> WeakWorld)
	{
		UWorld* World = WeakWorld.Get();
		if (!World)
		{
			return;
		}

		APlayerController* PlayerController = World->GetFirstPlayerController();
		if (!PlayerController || !PlayerController->IsLocalController())
		{
			return;
		}
		UGameInstance* Instance = World->GetGameInstance();
		UGCSLoginSubsystem* Login = Instance ? Instance->GetSubsystem<UGCSLoginSubsystem>() : nullptr;
		const bool bMock = World->GetOutermost()->GetName().Contains(TEXT("L_GCS_Workbench_V3"));
		bool bMockCapture = false;
#if WITH_DEV_AUTOMATION_TESTS
        bMockCapture = bMock && FParse::Param(FCommandLine::Get(), TEXT("GCSMockCapture"));
#endif
		if ((!Login || !Login->IsAuthenticated()) && !bMockCapture)
		{
			for (TObjectIterator<UGCSLoginWidget> It; It; ++It)
				if (It->GetWorld() == World && It->IsInViewport()) return;
			UGCSLoginWidget* Widget = CreateWidget<UGCSLoginWidget>(PlayerController);
			if (Widget)
			{
				Widget->AddToViewport(1000);
				PlayerController->bShowMouseCursor = true;
				FInputModeUIOnly Input;
				Input.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
				PlayerController->SetInputMode(Input);
			}
			return;
		}
		ConfigureGroundStationInput(PlayerController);
		// Reframe after the destination map has completed actor initialization.
		if (AGCSInvisiblePawn* Pawn = Cast<AGCSInvisiblePawn>(PlayerController->GetPawn()))
		{
			Pawn->ResetOrbitView();
		}


        if (bMock)
        {
            // V3 uses its own fleet/view adapter; do not bind legacy Widget events.
            for (TActorIterator<AActor> It(World); It; ++It)
                if (It->GetClass()->GetName().StartsWith(TEXT("BP_GCS_CoordinateAxes"))) It->SetActorHiddenInGame(true);
            for (TObjectIterator<UGCSMockWorkbench> It; It; ++It)
                if (It->GetWorld() == World && It->IsInViewport()) return;
            TArray<UUserWidget*> Previous;
            for (TObjectIterator<UUserWidget> It; It; ++It)
                if (It->GetWorld() == World && It->IsInViewport()) Previous.Add(*It);
            for (UUserWidget* View : Previous) View->RemoveFromParent();
            if (auto* Mock = CreateWidget<UGCSMockWorkbench>(PlayerController))
            {
                Mock->AddToViewport(100);
                FInputModeGameAndUI Input;
                Input.SetWidgetToFocus(Mock->TakeWidget());
                Input.SetHideCursorDuringCapture(false);
                PlayerController->SetInputMode(Input);
            }
            return;
        }

		const TSubclassOf<UUserWidget> MainWidgetClass = LoadClass<UUserWidget>(
			nullptr,
			World->GetOutermost()->GetName().Contains(TEXT("L_GCS_Workbench_V3"))
            ? TEXT("/Game/UI_V3/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C")
            : TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
		if (!MainWidgetClass)
		{
			UE_LOG(LogTemp, Error, TEXT("Unable to load editable GCS UI: /Game/UI/WBP_GCS_Main_V2"));
			return;
		}

		for (TObjectIterator<UUserWidget> WidgetIt; WidgetIt; ++WidgetIt)
		{
			UUserWidget* ExistingWidget = *WidgetIt;
			if (!ExistingWidget
				|| ExistingWidget->GetWorld() != World
				|| !ExistingWidget->IsInViewport()
				|| !ExistingWidget->IsA(MainWidgetClass))
			{
				continue;
			}

			if (UGameInstance* GameInstance = World->GetGameInstance())
			{
				if (UGCSUIBridgeSubsystem* UIBridge =
					GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>())
				{
					UIBridge->BindMainWidget(ExistingWidget);
				}
			}

			return;
		}

		UUserWidget* MainWidget = CreateWidget<UUserWidget>(PlayerController, MainWidgetClass);
		if (!MainWidget)
		{
			return;
		}

		MainWidget->AddToViewport(100);
		MainWidget->SetAnchorsInViewport(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		MainWidget->SetAlignmentInViewport(FVector2D::ZeroVector);

		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			if (UGCSUIBridgeSubsystem* UIBridge =
				GameInstance->GetSubsystem<UGCSUIBridgeSubsystem>())
			{
				UIBridge->BindMainWidget(MainWidget);
			}
		}

	}

	void HandleWorldInitialized(UWorld* World, const UWorld::InitializationValues)
	{
		if (!World || (World->WorldType != EWorldType::PIE && World->WorldType != EWorldType::Game))
		{
			return;
		}

		const TWeakObjectPtr<UWorld> WeakWorld(World);
		FTimerHandle TimerHandle;
		World->GetTimerManager().SetTimer(
			TimerHandle,
			FTimerDelegate::CreateStatic(&FDroneShowPlatform1Module::CreatePrototypeUI, WeakWorld),
			0.15f,
			false);
	}

	FDelegateHandle WorldInitializedHandle;
};

IMPLEMENT_PRIMARY_GAME_MODULE(FDroneShowPlatform1Module, DroneShowPlatform1, "DroneShowPlatform1");
