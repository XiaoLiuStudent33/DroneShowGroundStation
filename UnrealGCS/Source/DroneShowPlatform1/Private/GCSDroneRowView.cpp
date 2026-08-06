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
