#include "GCSLoginWidget.h"
#include "GCSLoginSubsystem.h"
#include "Brushes/SlateColorBrush.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/PackageName.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
FSlateFontInfo Font(int32 Size, bool Bold = false)
{ return FCoreStyle::GetDefaultFontStyle(Bold ? "Bold" : "Regular", Size); }
TSharedRef<STextBlock> Label(const FString& Value, int32 Size, FLinearColor Color, bool Bold = false)
{ return SNew(STextBlock).Text(FText::FromString(Value)).Font(Font(Size, Bold)).ColorAndOpacity(Color); }
}
bool UGCSLoginWidget::IsWorking() const
{
    const auto* Auth = GetGameInstance() ? GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>() : nullptr;
    return bLoading || !Auth || Auth->IsBusy();
}
void UGCSLoginWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (UsernameBox) FSlateApplication::Get().SetKeyboardFocus(UsernameBox, EFocusCause::SetDirectly);
}
TSharedRef<SWidget> UGCSLoginWidget::RebuildWidget()
{
    const auto* Auth = GetGameInstance() ? GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>() : nullptr;
    bSetup = Auth && !Auth->HasLocalAccount();
    const FLinearColor Cyan(0.10f, 0.82f, 0.83f), White(0.88f, 0.94f, 0.96f), Muted(0.42f, 0.55f, 0.61f);
    LoginButtonStyle = FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
    LoginButtonStyle.SetNormal(FSlateColorBrush(FLinearColor::White));
    LoginButtonStyle.SetHovered(FSlateColorBrush(FLinearColor(0.8f, 1.f, 1.f)));
    LoginButtonStyle.SetPressed(FSlateColorBrush(FLinearColor(0.5f, 0.8f, 0.8f)));
    TSharedPtr<SVerticalBox> Form;
    auto Commit = [this](const FText&, ETextCommit::Type Type) { if (Type == ETextCommit::OnEnter) Submit(); };
    auto Field = [&](TSharedPtr<SEditableTextBox>& Box, const FString& Hint, bool Secret)
    {
        return SNew(SBox).HeightOverride(48)[
            SAssignNew(Box, SEditableTextBox).Font(Font(16)).Padding(FMargin(12, 8))
            .ForegroundColor(White)
            .IsPassword(Secret).HintText(FText::FromString(Hint)).OnTextCommitted_Lambda(Commit)
            .IsEnabled_Lambda([this] { return !IsWorking(); })];
    };
    TSharedRef<SWidget> Result = SNew(SBorder)
        .BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
        .BorderBackgroundColor(FLinearColor(0.012f, 0.022f, 0.036f)).Padding(0)
        [SNew(SOverlay)
        + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(40, 28)
          [Label(TEXT("DRONE GCS   /   无人机表演地面站"), 18, Cyan, true)]
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center).Padding(32, 90)
          [SNew(SScaleBox).Stretch(EStretch::ScaleToFit).StretchDirection(EStretchDirection::DownOnly)
          [SNew(SHorizontalBox)
          + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 80, 0)
            [SNew(SBox).WidthOverride(420)[SNew(SVerticalBox)
              + SVerticalBox::Slot().AutoHeight()[Label(TEXT("连接机群\n掌控每一场演出"), 38, White, true)]
              + SVerticalBox::Slot().AutoHeight().Padding(0, 24, 0, 0)
                [Label(TEXT("DRONE SHOW GROUND CONTROL"), 13, Cyan)]
              + SVerticalBox::Slot().AutoHeight().Padding(0, 20, 0, 0)
                [Label(TEXT("任务规划  /  机群监测  /  演出控制"), 16, Muted)]
              + SVerticalBox::Slot().AutoHeight().Padding(0, 48, 0, 0)
                [Label(TEXT("本机工作站登录\n登录后进入三维地面站工作空间"), 14, Muted)]]]
          + SHorizontalBox::Slot().AutoWidth()
            [SNew(SBox).WidthOverride(440)
            [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
              .BorderBackgroundColor(FLinearColor(0.035f, 0.055f, 0.077f)).Padding(36)
              [SAssignNew(Form, SVerticalBox)]]]]]
        + SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Bottom).Padding(40, 24)
          [Label(TEXT("DroneGCS  ·  本机账号  ·  Windows 工作站"), 12, Muted)]
        + SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Bottom).Padding(40, 20)
          [SNew(SButton).ContentPadding(FMargin(20, 8)).IsEnabled_Lambda([this] { return !IsWorking(); })
            .OnClicked_Lambda([this] { UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false); return FReply::Handled(); })
            [Label(TEXT("退出程序"), 13, White)]]];
    Form->AddSlot().AutoHeight()[Label(bSetup ? TEXT("创建本机账号") : TEXT("欢迎回来"), 28, White, true)];
    Form->AddSlot().AutoHeight().Padding(0, 10, 0, 24)
        [Label(bSetup ? TEXT("首次使用，请设置工作站登录凭据") : TEXT("登录以进入地面站"), 14, Muted)];
#if UE_BUILD_DEVELOPMENT || UE_BUILD_DEBUG
    Form->AddSlot().AutoHeight().Padding(0, 0, 0, 16)
        [Label(TEXT("测试账号：123 / 123，可直接登录"), 12, Cyan)];
#endif
    Form->AddSlot().AutoHeight().Padding(0, 0, 0, 8)[Label(TEXT("账号"), 14, White)];
    Form->AddSlot().AutoHeight()[Field(UsernameBox, TEXT("输入账号"), false)];
    Form->AddSlot().AutoHeight().Padding(0, 18, 0, 8)[Label(TEXT("密码"), 14, White)];
    Form->AddSlot().AutoHeight()[Field(PasswordBox, bSetup ? TEXT("至少 12 位密码") : TEXT("输入密码"), true)];
    if (bSetup)
    {
        Form->AddSlot().AutoHeight().Padding(0, 18, 0, 8)[Label(TEXT("确认密码"), 14, White)];
        Form->AddSlot().AutoHeight()[Field(ConfirmationBox, TEXT("再次输入密码"), true)];
    }
    Form->AddSlot().AutoHeight().Padding(0, 16, 0, 12)
        [SNew(SBox).MinDesiredHeight(40)[SNew(STextBlock).Font(Font(12))
          .ColorAndOpacity(FLinearColor(1, 0.62f, 0.32f)).AutoWrapText(true)
          .Text_Lambda([this] { return FText::FromString(Status); })]];
    Form->AddSlot().AutoHeight()[SNew(SBox).HeightOverride(48)
        [SNew(SButton).ButtonStyle(&LoginButtonStyle).HAlign(HAlign_Center).VAlign(VAlign_Center).ButtonColorAndOpacity(Cyan)
          .IsEnabled_Lambda([this] { return !IsWorking(); }).OnClicked_UObject(this, &UGCSLoginWidget::Submit)
          [SNew(STextBlock).Font(Font(16, true)).ColorAndOpacity(FLinearColor(0.01f, 0.04f, 0.05f))
            .Text_Lambda([this] { return FText::FromString(IsWorking() ? TEXT("正在处理…") :
                (bSetup ? TEXT("创建并进入地面站") : TEXT("登录地面站"))); })]]];
    return Result;
}
FReply UGCSLoginWidget::Submit()
{
    if (IsWorking() || !UsernameBox || !PasswordBox) return FReply::Handled();
    auto* Auth = GetGameInstance()->GetSubsystem<UGCSLoginSubsystem>();
    Status = TEXT("正在校验本机账号…");
    TWeakObjectPtr<UGCSLoginWidget> WeakThis(this);
    Auth->SignIn(UsernameBox->GetText().ToString().TrimStartAndEnd(), PasswordBox->GetText().ToString(),
        ConfirmationBox ? ConfirmationBox->GetText().ToString() : FString(),
        [WeakThis](bool Success, FString Error)
    {
        auto* Self = WeakThis.Get();
        if (!Self || !Self->IsInViewport()) return;
        if (Self->PasswordBox) Self->PasswordBox->SetText(FText::GetEmpty());
        if (Self->ConfirmationBox) Self->ConfirmationBox->SetText(FText::GetEmpty());
        if (!Success) { Self->Status = Error; return; }
        if (!FPackageName::DoesPackageExist(TEXT("/Game/Maps/DroneShowPlatform1")))
        { Self->Status = TEXT("地面站场景缺失，请检查安装文件。"); return; }
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
void UGCSLoginWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    Super::ReleaseSlateResources(bReleaseChildren);
    UsernameBox.Reset(); PasswordBox.Reset(); ConfirmationBox.Reset();
}
