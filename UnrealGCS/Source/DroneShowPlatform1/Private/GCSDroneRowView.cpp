/**
 * @brief 实现 Fleet 行视图与已编写 UMG Border 的轻量绑定。
 *
 * 分类：Production UI/View；不创建 Fleet 数据，也不决定命令目标。
 */
#include "GCSDroneRowView.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"

bool UGCSDroneRowView::Initialize(UUserWidget* InFleetPanel, const int32 RowIndex)
{
	SelectedOverlay = InFleetPanel && InFleetPanel->WidgetTree
		? Cast<UBorder>(InFleetPanel->WidgetTree->FindWidget(
			*FString::Printf(TEXT("SelectedOverlay_Row%d"), RowIndex + 1)))
		: nullptr;

	SetSelectedVisual(false);
	return SelectedOverlay != nullptr;
}

void UGCSDroneRowView::SetDroneID(const FString& InDroneID)
{
	DroneID = InDroneID;
	if (DroneID.IsEmpty())
	{
		SetSelectedVisual(false);
	}
}

void UGCSDroneRowView::NotifyCurrentSelectedDroneID(
	const FString& CurrentSelectedDroneID)
{
	SetSelectedVisual(
		!DroneID.IsEmpty()
		&& DroneID.Equals(CurrentSelectedDroneID, ESearchCase::IgnoreCase));
}

void UGCSDroneRowView::SetSelectedVisual(const bool bSelected)
{
	bSelectedVisual = bSelected;
	if (SelectedOverlay)
	{
		SelectedOverlay->SetVisibility(
			bSelected ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Hidden);
	}
}
