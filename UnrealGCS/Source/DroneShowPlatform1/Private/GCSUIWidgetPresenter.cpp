#include "GCSUIWidgetPresenter.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "GCSUIBridgeSubsystem.h"

UUserWidget* FGCSUIWidgetPresenter::ResolveChildWidget(
	UUserWidget* Root,
	const FName Name)
{
	return Root && Root->WidgetTree
		? Cast<UUserWidget>(Root->WidgetTree->FindWidget(Name))
		: nullptr;
}

UUserWidget* FGCSUIWidgetPresenter::ResolveDroneDetail(UUserWidget* MainWidget)
{
	if (!MainWidget || !MainWidget->WidgetTree)
	{
		return nullptr;
	}

	if (UUserWidget* NamedChild = ResolveChildWidget(
		MainWidget,
		TEXT("CHILD_DroneDetail")))
	{
		return NamedChild;
	}

	return MainWidget->GetClass()->GetName().Contains(TEXT("WBP_GCS_DroneDetail"))
		? MainWidget
		: nullptr;
}

UButton* FGCSUIWidgetPresenter::FindButton(
	UUserWidget* Root,
	const FName Name)
{
	return Root && Root->WidgetTree
		? Cast<UButton>(Root->WidgetTree->FindWidget(Name))
		: nullptr;
}

UTextBlock* FGCSUIWidgetPresenter::FindText(
	UUserWidget* Root,
	const FName Name)
{
	return Root && Root->WidgetTree
		? Cast<UTextBlock>(Root->WidgetTree->FindWidget(Name))
		: nullptr;
}

void FGCSUIWidgetPresenter::SetText(
	UUserWidget* Root,
	const FName Name,
	const FString& Value)
{
	if (UTextBlock* Text = FindText(Root, Name))
	{
		Text->SetText(FText::FromString(Value));
	}
}

void FGCSUIWidgetPresenter::SetStatus(
	UUserWidget* Root,
	const FName Name,
	const FString& Value,
	const FLinearColor& Color)
{
	if (UTextBlock* Text = FindText(Root, Name))
	{
		Text->SetText(FText::FromString(Value));
		Text->SetColorAndOpacity(FSlateColor(Color));
		Text->SetVisibility(
			Value.IsEmpty()
				? ESlateVisibility::Hidden
				: ESlateVisibility::HitTestInvisible);
	}
}

void FGCSUIWidgetPresenter::SetButtonLabel(
	UUserWidget* Root,
	const FName ButtonName,
	const FString& Value)
{
	if (UButton* Button = FindButton(Root, ButtonName))
	{
		if (UTextBlock* Label = Cast<UTextBlock>(Button->GetContent()))
		{
			Label->SetText(FText::FromString(Value));
		}
	}
}

FString FGCSUIWidgetPresenter::DisplayStatusText(
	const EGCSDroneDisplayStatus Status)
{
	switch (Status)
	{
	case EGCSDroneDisplayStatus::Online: return TEXT("Online");
	case EGCSDroneDisplayStatus::Offline: return TEXT("Offline");
	case EGCSDroneDisplayStatus::LinkLost: return TEXT("Link Lost");
	}
	return TEXT("Link Lost");
}

FLinearColor FGCSUIWidgetPresenter::DisplayStatusColor(
	const EGCSDroneDisplayStatus Status)
{
	switch (Status)
	{
	case EGCSDroneDisplayStatus::Online:
		return FLinearColor(0.15f, 1.0f, 0.45f, 1.0f);
	case EGCSDroneDisplayStatus::Offline:
		return FLinearColor(1.0f, 0.18f, 0.20f, 1.0f);
	case EGCSDroneDisplayStatus::LinkLost:
		return FLinearColor(1.0f, 0.65f, 0.10f, 1.0f);
	}
	return FLinearColor::White;
}

void FGCSUIWidgetPresenter::SetWidgetVisible(
	const TWeakObjectPtr<UUserWidget>& Widget,
	const bool bVisible,
	const bool bSelfHitTestInvisibleWhenVisible)
{
	if (UUserWidget* ResolvedWidget = Widget.Get())
	{
		ResolvedWidget->SetVisibility(
			bVisible
				? (bSelfHitTestInvisibleWhenVisible
					? ESlateVisibility::SelfHitTestInvisible
					: ESlateVisibility::Visible)
				: ESlateVisibility::Collapsed);
	}
}
