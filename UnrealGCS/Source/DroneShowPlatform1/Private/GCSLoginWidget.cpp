#include "GCSLoginWidget.h"
#include "GCSLoginSubsystem.h"
#include "GCSLoginPresentation.h"
#include "GCSLoginMedia.h"
#include "SGCSLoginShow.h"
#include "SLoginDecoration.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/PackageName.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/PlatformApplicationMisc.h"

namespace
{
FLinearColor LoginColor(uint32 H) { return FLinearColor::FromSRGBColor(FColor(H>>16,(H>>8)&255,H&255)); }
FSlateFontInfo LoginFont(int32 Size,bool Bold=false) {return FCoreStyle::GetDefaultFontStyle(Bold?"Bold":"Regular",Size);}
const TCHAR* PreferenceSection=TEXT("MD.Login.Presentation");
}
bool UGCSLoginWidget::IsWorking() const
{
    const auto* Auth=GetGameInstance()?GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>():nullptr;
    return bLoading || !Auth || Auth->IsBusy();
}
void UGCSLoginWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if(UsernameBox)FSlateApplication::Get().SetKeyboardFocus(UsernameBox,EFocusCause::SetDirectly);
}
void UGCSLoginWidget::SetLanguage(int32 Language)
{
    Presentation->Language=FMath::Clamp(Language,0,3);
    GConfig->SetInt(PreferenceSection,TEXT("Language"),Presentation->Language,GGameUserSettingsIni);
    GConfig->Flush(false,GGameUserSettingsIni);
}
void UGCSLoginWidget::SavePreferences()
{
    GConfig->SetBool(PreferenceSection,TEXT("RememberAccount"),Presentation->Remember,GGameUserSettingsIni);
    GConfig->SetString(PreferenceSection,TEXT("Account"),Presentation->Remember && UsernameBox?*UsernameBox->GetText().ToString().TrimStartAndEnd():TEXT(""),GGameUserSettingsIni);
    GConfig->Flush(false,GGameUserSettingsIni);
}
TSharedRef<SWidget> UGCSLoginWidget::RebuildWidget()
{
    const auto* Auth=GetGameInstance()?GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>():nullptr;
    bSetup=Auth && !Auth->HasLocalAccount();
    AccountMode=0; // Registration is explicit; first launch also supports the development test login.
    if(LoginMedia)LoginMedia->Shutdown();
    LoginMedia=NewObject<UGCSLoginMedia>(this);LoginMedia->Initialize();
    Presentation=MakeShared<FGCSLoginPresentation>();
    GConfig->GetInt(PreferenceSection,TEXT("Language"),Presentation->Language,GGameUserSettingsIni);
    Presentation->Language=FMath::Clamp(Presentation->Language,0,3);
    GConfig->GetBool(PreferenceSection,TEXT("RememberAccount"),Presentation->Remember,GGameUserSettingsIni);
    const FLinearColor White=LoginColor(0xe6eef9),Muted=LoginColor(0x8fa7c2);
    LoginButtonStyle=FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
    LoginButtonStyle.SetNormal(FSlateRoundedBoxBrush(LoginColor(0x2788f5),7.f));
    LoginButtonStyle.SetHovered(FSlateRoundedBoxBrush(LoginColor(0x58a5ff),7.f));
    LoginButtonStyle.SetPressed(FSlateRoundedBoxBrush(LoginColor(0x2866af),7.f));
    LoginButtonStyle.SetDisabled(FSlateRoundedBoxBrush(LoginColor(0x284467),7.f));
    SecondaryStyle=LoginButtonStyle;
    SecondaryStyle.SetNormal(FSlateRoundedBoxBrush(FLinearColor::Transparent,6.f));
    SecondaryStyle.SetHovered(FSlateRoundedBoxBrush(LoginColor(0x243d5a),6.f));
    SecondaryStyle.SetPressed(FSlateRoundedBoxBrush(LoginColor(0x304d70),6.f));
    FieldStyle=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    FieldStyle.SetBackgroundImageNormal(FSlateRoundedBoxBrush(LoginColor(0x0e1c2c),7.f,LoginColor(0x30465f),1.f));
    FieldStyle.SetBackgroundImageHovered(FSlateRoundedBoxBrush(LoginColor(0x14283e),7.f,LoginColor(0x5d7da5),1.f));
    FieldStyle.SetBackgroundImageFocused(FSlateRoundedBoxBrush(LoginColor(0x14283e),7.f,LoginColor(0x65b4ff),1.f));
    FieldStyle.SetBackgroundImageReadOnly(FieldStyle.BackgroundImageNormal);
    FieldStyle.SetForegroundColor(White);
    RememberStyle=FCoreStyle::Get().GetWidgetStyle<FCheckBoxStyle>("Checkbox");
    FSlateRoundedBoxBrush Empty(LoginColor(0x192e49),3.f,LoginColor(0x7795b9),1.f); Empty.ImageSize=FVector2D(18,18);
    FSlateRoundedBoxBrush Checked(LoginColor(0x409afa),3.f,LoginColor(0x8dceff),2.f);Checked.ImageSize=FVector2D(18,18);
    RememberStyle.SetUncheckedImage(Empty).SetUncheckedHoveredImage(Empty).SetUncheckedPressedImage(Checked);
    RememberStyle.SetCheckedImage(Checked).SetCheckedHoveredImage(Checked).SetCheckedPressedImage(Empty);
    auto P=Presentation;
    auto L=[P,White](const TCHAR* S,const TCHAR* T,const TCHAR* E,const TCHAR* J,int32 Size=15,bool Bold=false)
    {return SNew(STextBlock).Font(LoginFont(Size,Bold)).ColorAndOpacity(White)
        .Text_Lambda([P,S,T,E,J]{return FText::FromString(P->Text(S,T,E,J));});};
    auto Commit=[this](const FText&,ETextCommit::Type Type){if(Type==ETextCommit::OnEnter)Submit();};
    auto Field=[&](TSharedPtr<SEditableTextBox>& Box,bool Secret,bool Confirm=false)->TSharedRef<SWidget>
    {
        return SNew(SBox).HeightOverride(58)
        [SNew(SOverlay)
        +SOverlay::Slot()
        [SAssignNew(Box,SEditableTextBox).Style(&FieldStyle).Font(LoginFont(16)).Padding(FMargin(48,15,Secret?55:16,15))
         .IsPassword_Lambda([P,Secret]{return Secret && !P->RevealPassword;})
         .HintText_Lambda([P,Secret,Confirm]{return FText::FromString(Confirm?P->Text(TEXT("再次输入密码"),TEXT("再次輸入密碼"),TEXT("Repeat password"),TEXT("パスワードを再入力")):
            Secret?P->Text(TEXT("请输入密码"),TEXT("請輸入密碼"),TEXT("Enter password"),TEXT("パスワードを入力")):
            P->Text(TEXT("请输入账号"),TEXT("請輸入帳號"),TEXT("Enter account"),TEXT("アカウントを入力")));})
         .OnTextCommitted_Lambda(Commit).IsEnabled_Lambda([this]{return !IsWorking();})]
        +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Center).Padding(14,0)
         [SNew(SLoginDecoration).Kind(Secret?2:1)]
        +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Center).Padding(4)
        [SNew(SButton).ButtonStyle(&SecondaryStyle).ContentPadding(FMargin(8,5))
         .Visibility(Secret&&!Confirm?EVisibility::Visible:EVisibility::Collapsed)
         .OnClicked_Lambda([P]{P->RevealPassword=!P->RevealPassword;return FReply::Handled();})
         [SNew(SLoginDecoration).Kind(5)]]];
    };
    CardBrush=FSlateRoundedBoxBrush(LoginColor(0x0c192b),14.f,LoginColor(0x2c4565),1.f);
    auto Link=[&](const TCHAR* Z,const TCHAR* T,const TCHAR* E,const TCHAR* J,int32 Mode)
    {
        return SNew(SButton).ButtonStyle(&SecondaryStyle).ContentPadding(FMargin(4,6))
            .IsEnabled_Lambda([this]{return !IsWorking();})
            .OnClicked_Lambda([this,Mode]{SwitchAccountMode(Mode);return FReply::Handled();})
            [SNew(STextBlock).Font(LoginFont(14)).ColorAndOpacity(LoginColor(0x77baff))
             .Text_Lambda([P,Z,T,E,J]{return FText::FromString(P->Text(Z,T,E,J));})];
    };
    auto Form=SNew(SVerticalBox);
    Form->AddSlot().AutoHeight().Padding(0,0,0,18)[SNew(STextBlock).Font(LoginFont(12)).ColorAndOpacity(LoginColor(0x459eff)).Text(FText::FromString(TEXT("M D  G R O U N D  S T A T I O N")))];
    Form->AddSlot().AutoHeight()[SNew(STextBlock).Font(LoginFont(34,true)).ColorAndOpacity(White).Text_Lambda([this,P]
    {
        return FText::FromString(AccountMode==1?P->Text(TEXT("注册账号"),TEXT("註冊帳號"),TEXT("Create account"),TEXT("アカウント登録")):
            AccountMode==2?P->Text(TEXT("重设密码"),TEXT("重設密碼"),TEXT("Reset password"),TEXT("パスワード再設定")):
            AccountMode==3?P->Text(TEXT("设置恢复码"),TEXT("設定復原碼"),TEXT("Set recovery code"),TEXT("復旧コード設定")):
            AccountMode==4?P->Text(TEXT("保存恢复码"),TEXT("儲存復原碼"),TEXT("Save recovery code"),TEXT("復旧コードを保存")):
            P->Text(TEXT("欢迎回来"),TEXT("歡迎回來"),TEXT("Welcome back"),TEXT("おかえりなさい")));
    })];
    Form->AddSlot().AutoHeight().Padding(0,12,0,30)[SNew(STextBlock).Font(LoginFont(15)).ColorAndOpacity(Muted).AutoWrapText(true).Text_Lambda([this,P]
    {
        return FText::FromString(AccountMode==1?P->Text(TEXT("创建本机账号，注册后请保存恢复码。"),TEXT("建立本機帳號，註冊後請保存復原碼。"),TEXT("Create a local account and save its recovery code."),TEXT("登録後に復旧コードを保存してください。")):
            AccountMode==2?P->Text(TEXT("使用恢复码验证身份并设置新密码。"),TEXT("使用復原碼驗證身分並設定新密碼。"),TEXT("Verify your recovery code to set a new password."),TEXT("復旧コードで新しいパスワードを設定します。")):
            AccountMode==3?P->Text(TEXT("验证原密码，为旧账号生成恢复码。"),TEXT("驗證原密碼，為舊帳號產生復原碼。"),TEXT("Verify your current password to enable recovery."),TEXT("現在のパスワードで復旧を設定します。")):
            AccountMode==4?P->Text(TEXT("恢复码只显示一次。请妥善保存；旧恢复码已失效。"),TEXT("復原碼僅顯示一次。請妥善保存；舊碼已失效。"),TEXT("Shown once. Save this code; previous codes are now invalid."),TEXT("一度だけ表示されます。保存してください。旧コードは無効です。")):
            P->Text(TEXT("登录摩登地面站"),TEXT("登入摩登地面站"),TEXT("Sign in to MD Ground Station"),TEXT("MD 地上局にログイン")));
    })];
    auto Inputs=SNew(SVerticalBox).Visibility_Lambda([this]{return AccountMode==4?EVisibility::Collapsed:EVisibility::Visible;});
    Form->AddSlot().AutoHeight()[Inputs];
    Inputs->AddSlot().AutoHeight().Padding(0,0,0,10)[L(TEXT("账号"),TEXT("帳號"),TEXT("Account"),TEXT("アカウント"))];
    Inputs->AddSlot().AutoHeight()[Field(UsernameBox,false)];
    Inputs->AddSlot().AutoHeight().Padding(0,24,0,10)[SNew(STextBlock).Font(LoginFont(15)).ColorAndOpacity(White).Text_Lambda([this,P]{return FText::FromString(AccountMode==2?P->Text(TEXT("新密码"),TEXT("新密碼"),TEXT("New password"),TEXT("新しいパスワード")):AccountMode==3?P->Text(TEXT("原密码"),TEXT("原密碼"),TEXT("Current password"),TEXT("現在のパスワード")):P->Text(TEXT("密码"),TEXT("密碼"),TEXT("Password"),TEXT("パスワード")));})];
    Inputs->AddSlot().AutoHeight()[Field(PasswordBox,true)];
    Inputs->AddSlot().AutoHeight()[SNew(SVerticalBox).Visibility_Lambda([this]{return AccountMode==1 || AccountMode==2?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,18,0,8)[L(TEXT("确认密码 · 6–18 位"),TEXT("確認密碼 · 6–18 位"),TEXT("Confirm password · 6–18 characters"),TEXT("確認用パスワード · 6–18文字"),13)]
        +SVerticalBox::Slot().AutoHeight()[Field(ConfirmationBox,true,true)]];
    Inputs->AddSlot().AutoHeight()[SNew(SVerticalBox).Visibility_Lambda([this]{return AccountMode==2?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight().Padding(0,18,0,8)[L(TEXT("恢复码"),TEXT("復原碼"),TEXT("Recovery code"),TEXT("復旧コード"),13)]
        +SVerticalBox::Slot().AutoHeight()[SNew(SBox).HeightOverride(50)[SAssignNew(RecoveryBox,SEditableTextBox).Style(&FieldStyle).Font(LoginFont(13)).Padding(12).IsPassword(true).OnTextCommitted_Lambda(Commit).IsEnabled_Lambda([this]{return !IsWorking();})]]];
    Inputs->AddSlot().AutoHeight().Padding(0,20,0,0)[SNew(SHorizontalBox).Visibility_Lambda([this]{return AccountMode==0?EVisibility::Visible:EVisibility::Collapsed;})
        +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)[SNew(SCheckBox).Style(&RememberStyle).IsChecked_Lambda([P]{return P->Remember?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
          .OnCheckStateChanged_Lambda([this,P](ECheckBoxState State){P->Remember=State==ECheckBoxState::Checked;SavePreferences();})
          [SNew(SBox).Padding(FMargin(8,0))[L(TEXT("记住账号"),TEXT("記住帳號"),TEXT("Remember account"),TEXT("アカウントを記憶"),14)]]]
        +SHorizontalBox::Slot().AutoWidth()[Link(TEXT("忘记密码？"),TEXT("忘記密碼？"),TEXT("Forgot password?"),TEXT("パスワードを忘れた"),2)]];
    Form->AddSlot().AutoHeight()[SNew(SVerticalBox).Visibility_Lambda([this]{return AccountMode==4?EVisibility::Visible:EVisibility::Collapsed;})
        +SVerticalBox::Slot().AutoHeight()[SNew(SEditableTextBox).Style(&FieldStyle).Font(LoginFont(13)).Padding(12).IsReadOnly(true).Text_Lambda([this]{return FText::FromString(RecoveryCode);})]
        +SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0,12)[SNew(SButton).ButtonStyle(&SecondaryStyle).ContentPadding(10).OnClicked_Lambda([this]{FPlatformApplicationMisc::ClipboardCopy(*RecoveryCode);return FReply::Handled();})[L(TEXT("复制恢复码"),TEXT("複製復原碼"),TEXT("Copy recovery code"),TEXT("コードをコピー"),14)]]];
    Form->AddSlot().AutoHeight().Padding(0,8,0,12)[SNew(SBox).MinDesiredHeight(16)[SNew(STextBlock).Font(LoginFont(12)).ColorAndOpacity(LoginColor(0xf4bd7e)).AutoWrapText(true).Text_Lambda([this,P]{return FText::FromString(P->StatusText(Status));})]];
    Form->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(60)[SNew(SButton).ButtonStyle(&LoginButtonStyle).HAlign(HAlign_Center).VAlign(VAlign_Center)
        .IsEnabled_Lambda([this]{return !IsWorking();}).OnClicked_UObject(this,&UGCSLoginWidget::Submit)
        [SNew(STextBlock).Font(LoginFont(18,true)).ColorAndOpacity(White).Text_Lambda([this,P]
        {
            return FText::FromString(IsWorking()?P->Text(TEXT("正在处理…"),TEXT("正在處理…"),TEXT("Please wait…"),TEXT("処理中…")):
                AccountMode==1?P->Text(TEXT("注册账号   →"),TEXT("註冊帳號   →"),TEXT("Create account   →"),TEXT("登録   →")):
                AccountMode==2?P->Text(TEXT("重设密码   →"),TEXT("重設密碼   →"),TEXT("Reset password   →"),TEXT("再設定   →")):
                AccountMode==3?P->Text(TEXT("生成恢复码   →"),TEXT("產生復原碼   →"),TEXT("Generate recovery code"),TEXT("復旧コードを生成")):
                AccountMode==4?P->Text(TEXT("已保存，返回登录"),TEXT("已儲存，返回登入"),TEXT("Saved · back to sign in"),TEXT("保存済み · ログインへ")):
                P->Text(TEXT("登录地面站   →"),TEXT("登入地面站   →"),TEXT("Sign in   →"),TEXT("ログイン   →")));
        })]]];
    Form->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0,18,0,0)[SNew(SHorizontalBox).Visibility_Lambda([this]{return AccountMode==0?EVisibility::Visible:EVisibility::Collapsed;})
        +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[L(TEXT("还没有账号？"),TEXT("還沒有帳號？"),TEXT("New here?"),TEXT("初めての方"),14)]
        +SHorizontalBox::Slot().AutoWidth().Padding(8,0)[Link(TEXT("注册账号"),TEXT("註冊帳號"),TEXT("Create account"),TEXT("アカウント登録"),1)]];
    Form->AddSlot().AutoHeight().HAlign(HAlign_Center)[SNew(SBox).Visibility_Lambda([this]{return AccountMode==2?EVisibility::Visible:EVisibility::Collapsed;})[Link(TEXT("旧账号设置恢复码"),TEXT("舊帳號設定復原碼"),TEXT("Set up recovery with current password"),TEXT("現在のパスワードで復旧を設定"),3)]];
    Form->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0,10)[SNew(SBox).Visibility_Lambda([this]{return AccountMode>0 && AccountMode<4?EVisibility::Visible:EVisibility::Collapsed;})[Link(TEXT("返回登录"),TEXT("返回登入"),TEXT("Back to sign in"),TEXT("ログインへ戻る"),0)]];
    Form->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0,24,0,0)[L(TEXT("本机工作站账号"),TEXT("本機工作站帳號"),TEXT("Local workstation account"),TEXT("ローカルワークステーション"),12)];
    FString Remembered;
    if(P->Remember && GConfig->GetString(PreferenceSection,TEXT("Account"),Remembered,GGameUserSettingsIni))UsernameBox->SetText(FText::FromString(Remembered));
    return SNew(SOverlay)
    +SOverlay::Slot()[SNew(SLoginDecoration).Kind(6).Visibility(EVisibility::HitTestInvisible)]
    +SOverlay::Slot()[SNew(SVerticalBox)
     +SVerticalBox::Slot().AutoHeight()
     [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor::Transparent).Padding(FMargin(44,24))
      [SNew(SHorizontalBox)
       +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,18,0)[SNew(SLoginDecoration).Kind(0)]
       +SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
       [L(TEXT("MD  /  摩登地面站"),TEXT("MD  /  摩登地面站"),TEXT("MD  /  GROUND STATION"),TEXT("MD  /  地上局"),19,false)]

       +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(18,0,0,0)[SNew(SLoginDecoration).Kind(3)]
       +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
       [SNew(SComboButton).ButtonStyle(&SecondaryStyle).ContentPadding(FMargin(12,5))
        .ButtonContent()[SNew(STextBlock).Font(LoginFont(14)).ColorAndOpacity(White)
          .Text_Lambda([P]{const TCHAR* Names[]={TEXT("简体中文"),TEXT("繁體中文"),TEXT("English"),TEXT("日本語")};return FText::FromString(Names[P->Language]);})]
        .OnGetMenuContent_Lambda([this,P,White]()->TSharedRef<SWidget>
        {
            auto Menu=SNew(SVerticalBox);
            const TCHAR* Names[]={TEXT("简体中文"),TEXT("繁體中文"),TEXT("English"),TEXT("日本語")};
            for(int32 I=0;I<4;++I)Menu->AddSlot().AutoHeight()
                [SNew(SButton).ButtonStyle(&SecondaryStyle).ContentPadding(FMargin(18,10))
                 .OnClicked_Lambda([this,I]{SetLanguage(I);FSlateApplication::Get().DismissAllMenus();return FReply::Handled();})
                 [SNew(STextBlock).Text(FText::FromString(Names[I])).Font(LoginFont(14)).ColorAndOpacity(White)]];
            return SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(LoginColor(0x182b44)).Padding(5)[Menu];
        })]]]
     +SVerticalBox::Slot().FillHeight(1)
     [SNew(SHorizontalBox)
      +SHorizontalBox::Slot().FillWidth(.60f)[SNew(SGCSLoginShow).State(P).Media(LoginMedia)]
      +SHorizontalBox::Slot().FillWidth(.40f)
      [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush")).BorderBackgroundColor(FLinearColor::Transparent).Padding(0)
       [SNew(SOverlay)

        +SOverlay::Slot().Padding(10,22,44,64).HAlign(HAlign_Fill).VAlign(VAlign_Center)
         [SNew(SScaleBox).Stretch(EStretch::ScaleToFit).StretchDirection(EStretchDirection::DownOnly)
          [SNew(SBox).WidthOverride(590)[SNew(SBorder).BorderImage(&CardBrush).Padding(42)[Form]]]]
        +SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(44,24)
         [SNew(SButton).ButtonStyle(&SecondaryStyle).ContentPadding(FMargin(12,8)).IsEnabled_Lambda([this]{return !IsWorking();})
          .OnClicked_Lambda([this]{UKismetSystemLibrary::QuitGame(this,GetOwningPlayer(),EQuitPreference::Quit,false);return FReply::Handled();})
          [SNew(SHorizontalBox)
           +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,10,0)[SNew(SBox).WidthOverride(22).HeightOverride(22)[SNew(SLoginDecoration).Kind(7)]]
           +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[L(TEXT("退出"),TEXT("退出"),TEXT("Exit"),TEXT("終了"),12)]]]]]]]
    +SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(44,32)
        [SNew(STextBlock).Font(LoginFont(11)).ColorAndOpacity(Muted).Visibility(EVisibility::HitTestInvisible)
         .Text_Lambda([P]{return FText::FromString(P->Text(TEXT("任务规划    /    机群监测    /    演出控制"),TEXT("任務規劃    /    機群監測    /    演出控制"),TEXT("PLAN    /    MONITOR    /    PERFORM"),TEXT("計画    /    監視    /    演出")));})];
}
FReply UGCSLoginWidget::Submit()
{
    if (IsWorking() || !UsernameBox || !PasswordBox) return FReply::Handled();
    auto* Auth = GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>();
    if(AccountMode==4){SwitchAccountMode(0);return FReply::Handled();}
    if(AccountMode>0)
    {
        Status.Reset();TWeakObjectPtr<UGCSLoginWidget> Weak(this);
        Auth->ManageAccount(AccountMode,UsernameBox->GetText().ToString().TrimStartAndEnd(),PasswordBox->GetText().ToString(),ConfirmationBox->GetText().ToString(),RecoveryBox->GetText().ToString(),
            [Weak](bool Success,FString Error,FString Code){if(auto* Self=Weak.Get()){Self->Status=Error;if(Success){Self->PasswordBox->SetText(FText::GetEmpty());Self->ConfirmationBox->SetText(FText::GetEmpty());Self->RecoveryBox->SetText(FText::GetEmpty());Self->RecoveryCode=MoveTemp(Code);Self->AccountMode=4;}}});
        return FReply::Handled();
    }
    Status = TEXT("正在校验本机账号…");
    TWeakObjectPtr<UGCSLoginWidget> WeakThis(this);
    Auth->SignIn(UsernameBox->GetText().ToString().TrimStartAndEnd(), PasswordBox->GetText().ToString(),
        FString(),
        [WeakThis](bool Success, FString Error)
    {
        auto* Self = WeakThis.Get();
        if (!Self || !Self->IsInViewport()) return;
        if (Self->PasswordBox) Self->PasswordBox->SetText(FText::GetEmpty());
        if (Self->ConfirmationBox) Self->ConfirmationBox->SetText(FText::GetEmpty());
        if (!Success) { Self->Status = Error; return; }
        if (!FPackageName::DoesPackageExist(TEXT("/Game/Maps/DroneShowPlatform1")))
        { Self->Status = TEXT("地面站场景缺失，请检查安装文件。"); return; }
        Self->SavePreferences();
        Self->bLoading = true;
        Self->Status = TEXT("登录成功，正在加载地面站场景…");
        FTimerHandle Handle;
        Self->GetWorld()->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(Self, [WeakThis]
        {
            if (auto* Widget = WeakThis.Get()) Widget->GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>()->EnterStation();
        }), 0.15f, false);
    });
    return FReply::Handled();
}
void UGCSLoginWidget::NativeDestruct()
{
    if(LoginMedia)LoginMedia->Shutdown();
    Super::NativeDestruct();
}
void UGCSLoginWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    if(LoginMedia)LoginMedia->Shutdown();
    Super::ReleaseSlateResources(bReleaseChildren);
    UsernameBox.Reset(); PasswordBox.Reset(); ConfirmationBox.Reset();
}

void UGCSLoginWidget::SwitchAccountMode(int32 Mode)
{
    if(IsWorking())return;
    AccountMode=Mode;Status.Reset();RecoveryCode.Reset();
    if(PasswordBox)PasswordBox->SetText(FText::GetEmpty());
    if(ConfirmationBox)ConfirmationBox->SetText(FText::GetEmpty());
    if(RecoveryBox)RecoveryBox->SetText(FText::GetEmpty());
}
