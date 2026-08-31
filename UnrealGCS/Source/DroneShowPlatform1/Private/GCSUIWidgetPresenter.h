#pragma once

#include "CoreMinimal.h"

class UButton;
class UTextBlock;
class UUserWidget;
enum class EGCSDroneDisplayStatus : uint8;

/** Stateless widget lookup and presentation helpers for GCSUIBridgeSubsystem. */
struct FGCSUIWidgetPresenter final
{
	static UUserWidget* ResolveChildWidget(UUserWidget* Root, FName Name);
	static UUserWidget* ResolveDroneDetail(UUserWidget* MainWidget);
	static UButton* FindButton(UUserWidget* Root, FName Name);
	static UTextBlock* FindText(UUserWidget* Root, FName Name);

	static void SetText(UUserWidget* Root, FName Name, const FString& Value);
	static void SetStatus(
		UUserWidget* Root,
		FName Name,
		const FString& Value,
		const FLinearColor& Color);
	static void SetButtonLabel(
		UUserWidget* Root,
		FName ButtonName,
		const FString& Value);

	static FString DisplayStatusText(EGCSDroneDisplayStatus Status);
	static FLinearColor DisplayStatusColor(EGCSDroneDisplayStatus Status);
	static void SetWidgetVisible(
		const TWeakObjectPtr<UUserWidget>& Widget,
		bool bVisible,
		bool bSelfHitTestInvisibleWhenVisible = false);
};
