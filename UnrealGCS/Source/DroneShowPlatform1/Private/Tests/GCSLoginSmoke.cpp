// Explicit development-only end-to-end runner: -ExecCmds="GCS.Login.Smoke".
// Uses the real widget submit, hashing worker, OpenLevel and main-widget bootstrap.
#if WITH_DEV_AUTOMATION_TESTS
#include "GCSLoginSubsystem.h"
#include "GCSLoginWidget.h"
#include "GCSMockWorkbench.h"
#include "Blueprint/WidgetTree.h"
#include "Components/WidgetSwitcher.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SEditableTextBox.h"

struct FGCSLoginSmoke
{
    static void Run(UWorld* InitialWorld)
    {
        if (InitialWorld && InitialWorld->WorldType != EWorldType::Game) return;
        struct FState
        {
            TWeakObjectPtr<UGameInstance> Instance;
            FString Path;
            FString Password;
            double Start = FPlatformTime::Seconds();
            double SuccessTime = 0;
            bool Submitted = false;
            bool V3 = false;
        };
        auto State = MakeShared<FState>();
        if (InitialWorld) State->Instance = InitialWorld->GetGameInstance();
        State->Path = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), FGuid::NewGuid().ToString() + TEXT("-smoke.dat"));
        State->Password = FGuid::NewGuid().ToString().Left(18);
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([State](float)
        {
            const FString ResultPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Login-smoke-result.txt"));
            if (FPlatformTime::Seconds() - State->Start > 90)
            {
                FFileHelper::SaveStringToFile(TEXT("FAIL: login-to-station timeout"), *ResultPath);
                IFileManager::Get().Delete(*State->Path);
                FPlatformMisc::RequestExit(false); return false;
            }
            if (!State->Instance.IsValid())
            {
                for (const FWorldContext& Context : GEngine->GetWorldContexts())
                    if (Context.WorldType == EWorldType::Game && Context.World())
                        State->Instance = Context.World()->GetGameInstance();
                if (!State->Instance.IsValid()) return true;
            }
            UWorld* World = State->Instance->GetWorld();
            auto* Login = State->Instance->GetSubsystem<UGCSLoginSubsystem>();
            Login->TestAccountPath = State->Path;
            if (!State->Submitted) State->V3 = World->GetMapName().Contains(TEXT("L_GCS_Workbench_V3"));
            if (!State->Submitted)
            {
                for (TObjectIterator<UGCSLoginWidget> It; It; ++It)
                {
                    if (It->GetWorld() != World || !It->IsInViewport() || !It->UsernameBox) continue;
                    // The test account is isolated even if a real workstation account exists.
                    FString Error;
                    if (!UGCSLoginSubsystem::ProcessCredentials(State->Path, TEXT("smoke-operator"), State->Password, State->Password, Error))
                    {
                        FFileHelper::SaveStringToFile(TEXT("FAIL: isolated test setup"), *ResultPath);
                        FPlatformMisc::RequestExit(false); return false;
                    }
                    It->UsernameBox->SetText(FText::FromString(TEXT("smoke-operator")));
                    It->PasswordBox->SetText(FText::FromString(State->Password));
                    if (It->ConfirmationBox) It->ConfirmationBox->SetText(FText::FromString(State->Password));
                    It->AccountMode=0;
                    It->Submit();
                    State->Submitted = true;
                    break;
                }
                return true;
            }
            bool HasMain = false, HasLogin = false;
            for (TObjectIterator<UUserWidget> It; It; ++It)
            {
                if (It->GetWorld() != World || !It->IsInViewport()) continue;
                HasMain |= State->V3 ? It->IsA<UGCSMockWorkbench>() : It->GetClass()->GetPathName() == TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C");
                HasLogin |= It->IsA<UGCSLoginWidget>();
            }
            if (Login->IsAuthenticated() && World->GetMapName() == (State->V3 ? TEXT("L_GCS_Workbench_V3") : TEXT("DroneShowPlatform1")) && HasMain && !HasLogin)
            {
                if (!State->SuccessTime)
                {
                    State->SuccessTime = FPlatformTime::Seconds();
                    FFileHelper::SaveStringToFile(TEXT("PASS: real widget submit -> asynchronous password verification -> OpenLevel -> authenticated same GameInstance -> main widget visible, login widget removed"), *ResultPath);
                    FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/Login-station.png")), true, false);
                }
                if (FPlatformTime::Seconds() - State->SuccessTime > 2)
                {
                    IFileManager::Get().Delete(*State->Path);
                    FPlatformMisc::RequestExit(false); return false;
                }
            }
            return true;
        }), 0.1f);
    }
};
static FAutoConsoleCommandWithWorld LoginSmokeCommand(TEXT("GCS.Login.Smoke"),
    TEXT("Run an isolated development-only login-to-station smoke test and exit."),
    FConsoleCommandWithWorldDelegate::CreateStatic(&FGCSLoginSmoke::Run));
#endif
