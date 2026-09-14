// Development-only real widget registration -> recovery -> login flow, isolated storage.
#if WITH_DEV_AUTOMATION_TESTS
#include "GCSLoginWidget.h"
#include "GCSLoginSubsystem.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SEditableTextBox.h"
struct FGCSLoginAccountSmoke
{
 static void Run(UWorld* World)
 {
  if(!World || World->WorldType!=EWorldType::Game)return;
  struct FState{double Start=FPlatformTime::Seconds();int Stage=0;FString Base=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation"),FGuid::NewGuid().ToString()+TEXT("-account-flow.dat"));FString Code;TWeakObjectPtr<UGCSLoginWidget> Widget;TWeakObjectPtr<UGCSLoginSubsystem> Auth;};
  auto S=MakeShared<FState>();
  FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([S](float)
  {
   auto Finish=[S](bool Pass)
   {
    FFileHelper::SaveStringToFile(Pass?TEXT("PASS: widget registration -> recovery code result -> password reset -> rotated recovery code -> login -> station; isolated account removed"):TEXT("FAIL: account widget flow"),*FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/Login-account-flow.txt")));
    const FString Path=UGCSLoginSubsystem::ResolveAccountPath(S->Base,TEXT("flow-operator"));IFileManager::Get().Delete(*Path);IFileManager::Get().DeleteDirectory(*(S->Base+TEXT(".accounts")),false,false);
    FPlatformMisc::RequestExit(false);return false;
   };
   if(FPlatformTime::Seconds()-S->Start>80)return Finish(false);
   if(!S->Widget.IsValid() && S->Stage==0)for(TObjectIterator<UGCSLoginWidget> It;It;++It)if(It->IsInViewport() && It->UsernameBox){S->Widget=*It;S->Auth=It->GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>();break;}
   auto* W=S->Widget.Get();auto* Auth=S->Auth.Get();if(!Auth || Auth->IsBusy())return true;
   if(S->Stage==0 && W)
   {
    Auth->TestAccountPath=S->Base;W->SwitchAccountMode(1);W->UsernameBox->SetText(FText::FromString(TEXT("flow-operator")));W->PasswordBox->SetText(FText::FromString(TEXT("flow-password-123")));W->ConfirmationBox->SetText(FText::FromString(TEXT("flow-password-123")));W->Submit();S->Stage=1;
   }
   else if(S->Stage==1 && W)
   {
    if(W->AccountMode!=4 || W->RecoveryCode.Len()!=48)return Finish(false);
    S->Code=W->RecoveryCode;W->SwitchAccountMode(2);W->PasswordBox->SetText(FText::FromString(TEXT("flow-password-456")));W->ConfirmationBox->SetText(FText::FromString(TEXT("flow-password-456")));W->RecoveryBox->SetText(FText::FromString(S->Code));W->Submit();S->Stage=2;
   }
   else if(S->Stage==2 && W)
   {
    if(W->AccountMode!=4 || W->RecoveryCode.Len()!=48 || W->RecoveryCode==S->Code)return Finish(false);
    W->SwitchAccountMode(0);W->PasswordBox->SetText(FText::FromString(TEXT("flow-password-456")));W->Submit();S->Stage=3;
   }
   else if(S->Stage==3 && Auth->IsAuthenticated() && Auth->GetWorld()->GetMapName()==TEXT("DroneShowPlatform1") && (!W || !W->IsInViewport()))return Finish(true);
   return true;
  }),.1f);
 }
};
static FAutoConsoleCommandWithWorld Command(TEXT("GCS.Login.AccountSmoke"),TEXT("Isolated register/recover/login UI check."),FConsoleCommandWithWorldDelegate::CreateStatic(&FGCSLoginAccountSmoke::Run));
#endif
