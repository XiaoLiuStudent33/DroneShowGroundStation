#if WITH_DEV_AUTOMATION_TESTS
#include "GCSLoginWidget.h"
#include "GCSLoginMedia.h"
#include "GCSLoginPresentation.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"
#include "Widgets/Input/SEditableTextBox.h"
struct FGCSLoginMediaCheck
{
 static void Run(UWorld* World)
 {
  if(!World || World->WorldType!=EWorldType::Game)return;
  IFileManager::Get().MakeDirectory(*FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/LoginMedia")),true);
  struct FState{double Start=FPlatformTime::Seconds(),PausedTime=0;int Stage=0,Language=0;TWeakObjectPtr<UGCSLoginWidget> Widget;TWeakObjectPtr<UGCSLoginMedia> Media;};
  auto S=MakeShared<FState>();
  FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([S](float)
  {
   double T=FPlatformTime::Seconds()-S->Start;
   auto Finish=[S](FString Result)
   {
    if(auto* M=S->Media.Get())
    {
     auto Stats=[](TArray<float> F){if(F.IsEmpty())return FString(TEXT("no samples"));F.Sort();double Sum=0;for(float V:F)Sum+=V;return FString::Printf(TEXT("n=%d mean=%.2fms p95=%.2fms max=%.2fms"),F.Num(),Sum/F.Num(),F[FMath::Min(F.Num()-1,FMath::FloorToInt(F.Num()*.95f))],F.Last());};
     Result+=TEXT("\nSteady frame intervals: ")+Stats(M->SteadyFrameMs)+TEXT("\nTransition frame intervals: ")+Stats(M->TransitionFrameMs);
    }
    if(auto* W=S->Widget.Get())W->SetLanguage(S->Language);
    FFileHelper::SaveStringToFile(Result,*FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/LoginMedia/result.txt")));
    FPlatformMisc::RequestExit(false);return false;
   };
   if(T>45)return Finish(TEXT("FAIL: timeout"));
   if(!S->Widget.IsValid() && S->Stage==0)for(TObjectIterator<UGCSLoginWidget> It;It;++It)if(It->IsInViewport() && It->LoginMedia){S->Widget=*It;S->Media=It->LoginMedia;S->Language=It->Presentation->Language;break;}
   auto* W=S->Widget.Get();auto* M=S->Media.Get();if(!W || !M)return true;
   auto P=W->Presentation;
   auto Shot=[](const TCHAR* Name){FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/LoginMedia"),Name),true,false);};
   if(S->Stage==0 && T>3){W->SetLanguage(0);W->UsernameBox->SetText(FText::FromString(TEXT("operator")));P->Paused=false;M->bMeasureFrames=true;S->Stage++;}
   else if(S->Stage==1 && T>6){if(!M->IsVideoVisible() || M->GetMediaTheme()!=0)return Finish(TEXT("FAIL: Saturn video did not open"));M->bMeasureFrames=false;Shot(TEXT("01-saturn.png"));S->Stage++;}
   else if(S->Stage==2 && T>8){P->Paused=true;S->Stage++;}
   else if(S->Stage==3 && T>9){S->PausedTime=M->GetPlaybackSeconds();S->Stage++;}
   else if(S->Stage==4 && T>10){if(FMath::Abs(M->GetPlaybackSeconds()-S->PausedTime)>.12)return Finish(TEXT("FAIL: pause"));P->Next(1);M->bMeasureFrames=true;S->Stage++;}
   else if(S->Stage==5 && T>13){if(M->GetMediaTheme()!=1 || !M->IsVideoVisible())return Finish(TEXT("FAIL: Wing video did not open"));M->bMeasureFrames=false;Shot(TEXT("02-wing.png"));P->Paused=false;S->Stage++;}
   else if(S->Stage==6 && T>15){M->bMeasureFrames=true;P->Next(1);S->Stage++;}
   else if(S->Stage==7 && T>18){if(M->GetMediaTheme()!=2 || !M->IsVideoVisible())return Finish(TEXT("FAIL: Tide video did not open"));M->bMeasureFrames=false;Shot(TEXT("03-tide.png"));S->Stage++;}
   else if(S->Stage==8 && T>20){M->SetStatic(true);S->Stage++;}
   else if(S->Stage==9 && T>22){if(!M->IsDecoderClosed() || M->IsVideoVisible())return Finish(TEXT("FAIL: static fallback decoder still active"));Shot(TEXT("04-static-fallback.png"));S->Stage++;}
   else if(S->Stage==10 && T>24){M->SetStatic(false);S->Stage++;}
   else if(S->Stage==11 && T>27){if(!M->IsVideoVisible())return Finish(TEXT("FAIL: video did not resume"));W->RemoveFromParent();S->Stage++;}
   else if(S->Stage==12 && T>28){return Finish(M->IsStopped()?TEXT("PASS: three movies opened; pause, manual switch, static fallback, resume and widget-removal decoder shutdown. One player instance. No authentication or flight commands submitted."):TEXT("FAIL: decoder survived widget removal"));}
   return true;
  }),.05f);
 }
};
static FAutoConsoleCommandWithWorld Command(TEXT("GCS.Login.MediaCheck"),TEXT("Login movie lifecycle and actual rendered check."),FConsoleCommandWithWorldDelegate::CreateStatic(&FGCSLoginMediaCheck::Run));
#endif
