// Copyright Epic Games, Inc. All Rights Reserved.

#include "DroneShowPlatform1.h"

#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameInstance.h"
#include "GCSUIBridgeSubsystem.h"
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
		ConfigureGroundStationInput(PlayerController);

		const TSubclassOf<UUserWidget> MainWidgetClass = LoadClass<UUserWidget>(
			nullptr,
			TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
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
