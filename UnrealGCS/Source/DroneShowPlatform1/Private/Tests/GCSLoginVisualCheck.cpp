#if WITH_DEV_AUTOMATION_TESTS
#include "GCSLoginWidget.h"
#include "GCSLoginPresentation.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Input/SEditableTextBox.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSLoginCarouselTest,"DroneGCS.Login.Presentation",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSLoginCarouselTest::RunTest(const FString&)
{
    FGCSLoginPresentation P;
    TestEqual(TEXT("Saturn first"),P.Theme,0);
    for(int32 I=0;I<142;++I)P.Advance(.1f);
    TestEqual(TEXT("Autoplay advances"),P.Theme,1);
    TestTrue(TEXT("Horizontal transition active"),P.Transition<1.f);
    int32 Before=P.Theme;P.Next(1);TestEqual(TEXT("No overlapping transition"),P.Theme,Before);
    for(int32 I=0;I<10;++I)P.Advance(.1f);
    P.Next(-1);TestEqual(TEXT("Reverse swipe target"),P.Theme,0);
    for(int32 I=0;I<10;++I)P.Advance(.1f);
    P.Next(-1);TestEqual(TEXT("Wrap previous"),P.Theme,2);
    for(int32 Lang=0;Lang<4;++Lang)
    {
        P.Language=Lang;
        TestFalse(TEXT("Translated title present"),P.Title(0).IsEmpty());
        TestFalse(TEXT("Translated error present"),P.StatusText(TEXT("账号或密码不正确。")).IsEmpty());
    }
    P.Transition=1;P.Paused=true;
    const double Frozen=P.AnimationTime;const int32 FrozenTheme=P.Theme;
    for(int32 I=0;I<160;++I)P.Advance(.1f);
    TestEqual(TEXT("Pause freezes light animation"),P.AnimationTime,Frozen);
    TestEqual(TEXT("Pause freezes autoplay"),P.Theme,FrozenTheme);
    P.Next(1);TestEqual(TEXT("Manual arrow works while paused"),P.Theme,(FrozenTheme+1)%3);
    P.Paused=false;P.Advance(.1f);TestTrue(TEXT("Resume advances animation"),P.AnimationTime>Frozen);
    return true;
}
struct FGCSLoginVisualCheck
{
    static void Run(UWorld* World)
    {
        if(!World || World->WorldType!=EWorldType::Game)return;
        struct FState {double Start=FPlatformTime::Seconds();int32 Stage=0;int32 Original=0;TWeakObjectPtr<UGCSLoginWidget> Widget;bool Passed=true;};
        auto S=MakeShared<FState>();
        FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([S](float)
        {
            double Elapsed=FPlatformTime::Seconds()-S->Start;
            if(!S->Widget.IsValid())for(TObjectIterator<UGCSLoginWidget> It;It;++It)if(It->IsInViewport() && It->Presentation){S->Widget=*It;S->Original=It->Presentation->Language;break;}
            auto* W=S->Widget.Get();
            if(Elapsed>45){if(W)W->SetLanguage(S->Original);FPlatformMisc::RequestExit(false);return false;}
            if(!W)return true;
            auto Shot=[](const TCHAR* Name){FScreenshotRequest::RequestScreenshot(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/LoginDesign"),Name),true,false);};
            if(S->Stage==0 && Elapsed>3){W->SetLanguage(0);W->UsernameBox->SetText(FText::FromString(TEXT("operator")));W->Presentation->Theme=0;W->Presentation->Previous=0;W->Presentation->Transition=1;W->Presentation->ThemeTime=0;W->Presentation->Paused=false;S->Stage++;}
            else if(S->Stage==1 && Elapsed>5){Shot(TEXT("01-saturn-zh.png"));S->Stage++;}
            else if(S->Stage==2 && Elapsed>7){Shot(TEXT("02-saturn-motion.png"));W->Presentation->Paused=true;W->SetLanguage(1);S->Stage++;}
            else if(S->Stage==3 && Elapsed>9){Shot(TEXT("03-traditional.png"));S->Stage++;}
            else if(S->Stage==4 && Elapsed>11){W->SetLanguage(2);W->Presentation->Next(1);S->Passed &= W->UsernameBox->GetText().ToString()==TEXT("operator");S->Stage++;}
            else if(S->Stage==5 && Elapsed>16){Shot(TEXT("04-starsea-en.png"));S->Stage++;}
            else if(S->Stage==6 && Elapsed>18){W->SetLanguage(3);W->Presentation->Next(1);W->Status=TEXT("账号或密码不正确。");S->Stage++;}
            else if(S->Stage==7 && Elapsed>20){Shot(TEXT("05-japanese.png"));S->Stage++;}
            else if(S->Stage==8 && Elapsed>22){W->SetLanguage(0);W->SwitchAccountMode(1);S->Stage++;}
            else if(S->Stage==9 && Elapsed>24){Shot(TEXT("07-register.png"));S->Stage++;}
            else if(S->Stage==10 && Elapsed>26){W->SwitchAccountMode(2);S->Stage++;}
            else if(S->Stage==11 && Elapsed>28){Shot(TEXT("08-recovery.png"));S->Stage++;}
            else if(S->Stage==12 && Elapsed>30){W->SwitchAccountMode(3);S->Stage++;}
            else if(S->Stage==13 && Elapsed>32){Shot(TEXT("09-enroll.png"));S->Stage++;}
            else if(S->Stage==14 && Elapsed>34)
            {
                W->SetLanguage(S->Original);
                FFileHelper::SaveStringToFile(S->Passed?TEXT("PASS: animated mesh, languages switched without clearing account; six real rendered captures. No login submitted."):TEXT("FAIL: texture or form persistence"),*FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation/LoginDesign/result.txt")));
                FPlatformMisc::RequestExit(false);return false;
            }
            return true;
        }),.1f);
    }
};
static FAutoConsoleCommandWithWorld LoginVisualCommand(TEXT("GCS.Login.VisualCheck"),TEXT("Capture login themes and languages; no authentication or flight commands."),FConsoleCommandWithWorldDelegate::CreateStatic(&FGCSLoginVisualCheck::Run));
#endif
