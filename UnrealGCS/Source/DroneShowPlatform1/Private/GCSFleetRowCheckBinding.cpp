#include "GCSFleetRowCheckBinding.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "GCSUIBridgeSubsystem.h"
#include "GCSUIWidgetPresenter.h"
#include "UObject/UnrealType.h"

namespace
{
	bool GetRowString(UObject* RowObject, const FName PropertyName, FString& OutValue)
	{
		if (RowObject)
		{
			if (const FStrProperty* Property =
				FindFProperty<FStrProperty>(RowObject->GetClass(), PropertyName))
			{
				OutValue = Property->GetPropertyValue_InContainer(RowObject);
				return true;
			}
		}
		OutValue.Reset();
		return false;
	}

	bool GetRowBool(UObject* RowObject, const FName PropertyName, bool& OutValue)
	{
		if (RowObject)
		{
			if (const FBoolProperty* Property =
				FindFProperty<FBoolProperty>(RowObject->GetClass(), PropertyName))
			{
				OutValue = Property->GetPropertyValue_InContainer(RowObject);
				return true;
			}
		}
		OutValue = false;
		return false;
	}

	UTextBlock* FindRowText(UUserWidget* EntryWidget, const FName WidgetName)
	{
		return EntryWidget && EntryWidget->WidgetTree
			? Cast<UTextBlock>(EntryWidget->WidgetTree->FindWidget(WidgetName))
			: nullptr;
	}

	EGCSDroneDisplayStatus ParseDisplayStatus(const FString& StatusText)
	{
		if (StatusText.Equals(TEXT("Online"), ESearchCase::IgnoreCase))
		{
			return EGCSDroneDisplayStatus::Online;
		}
		if (StatusText.Equals(TEXT("Offline"), ESearchCase::IgnoreCase))
		{
			return EGCSDroneDisplayStatus::Offline;
		}
		return EGCSDroneDisplayStatus::LinkLost;
	}
}

void UGCSFleetRowCheckBinding::Initialize(
	UGCSUIBridgeSubsystem* InBridge,
	UObject* InRowObject,
	UCheckBox* InCheckBox,
	UButton* InDetailButton,
	UUserWidget* InEntryWidget)
{
	Shutdown();
	Bridge = InBridge;
	RowObject = InRowObject;
	CheckBox = InCheckBox;
	DetailButton = InDetailButton;
	EntryWidget = InEntryWidget;
	if (CheckBox)
	{
		CheckBox->OnCheckStateChanged.AddUniqueDynamic(
			this,
			&UGCSFleetRowCheckBinding::HandleCheckStateChanged);
	}
	if (DetailButton)
	{
		DetailButton->OnClicked.AddUniqueDynamic(
			this,
			&UGCSFleetRowCheckBinding::HandleDetailClicked);
	}
}

void UGCSFleetRowCheckBinding::RefreshFromRowObject(
	const FString& CurrentSelectedDroneID)
{
	if (!RowObject || !EntryWidget || !EntryWidget->WidgetTree)
	{
		return;
	}

	struct FTextMapping
	{
		FName PropertyName;
		FName WidgetName;
	};
	static const FTextMapping TextMappings[] =
	{
		{ TEXT("DroneID"), TEXT("TXT_FleetRowID") },
		{ TEXT("Mode"), TEXT("TXT_FleetRowMode") },
		{ TEXT("GPSText"), TEXT("TXT_FleetRowGPS") },
		{ TEXT("BatteryText"), TEXT("TXT_FleetRowBattery") },
		{ TEXT("HeightText"), TEXT("TXT_FleetRowHeight") },
		{ TEXT("RSSIText"), TEXT("TXT_FleetRowRSSI") },
		{ TEXT("StatusText"), TEXT("TXT_FleetRowStatus") }
	};

	FString Value;
	for (const FTextMapping& Mapping : TextMappings)
	{
		if (GetRowString(RowObject, Mapping.PropertyName, Value))
		{
			if (UTextBlock* Text = FindRowText(EntryWidget, Mapping.WidgetName))
			{
				if (!Text->GetText().ToString().Equals(Value, ESearchCase::CaseSensitive))
				{
					Text->SetText(FText::FromString(Value));
				}
			}
		}
	}

	FString StatusText;
	if (GetRowString(RowObject, TEXT("StatusText"), StatusText))
	{
		if (UTextBlock* Status = FindRowText(EntryWidget, TEXT("TXT_FleetRowStatus")))
		{
			Status->SetColorAndOpacity(
				FGCSUIWidgetPresenter::DisplayStatusColor(ParseDisplayStatus(StatusText)));
		}
	}

	bool bCommandSelected = false;
	bool bCommandEnabled = false;
	GetRowBool(RowObject, TEXT("bCommandSelected"), bCommandSelected);
	GetRowBool(RowObject, TEXT("bCommandEnabled"), bCommandEnabled);
	if (CheckBox)
	{
		TGuardValue<bool> ApplyingGuard(bApplyingState, true);
		CheckBox->SetIsChecked(bCommandSelected);
		CheckBox->SetIsEnabled(bCommandEnabled);
	}

	FString DroneID;
	const bool bIsDetailSelected =
		GetRowString(RowObject, TEXT("DroneID"), DroneID)
		&& !CurrentSelectedDroneID.IsEmpty()
		&& DroneID.Equals(CurrentSelectedDroneID, ESearchCase::IgnoreCase);
	if (UWidget* DetailSelected =
		EntryWidget->WidgetTree->FindWidget(TEXT("B_DetailSelected")))
	{
		DetailSelected->SetVisibility(
			bIsDetailSelected
				? ESlateVisibility::HitTestInvisible
				: ESlateVisibility::Collapsed);
	}
}

void UGCSFleetRowCheckBinding::Shutdown()
{
	if (CheckBox)
	{
		CheckBox->OnCheckStateChanged.RemoveDynamic(
			this,
			&UGCSFleetRowCheckBinding::HandleCheckStateChanged);
	}
	if (DetailButton)
	{
		DetailButton->OnClicked.RemoveDynamic(
			this,
			&UGCSFleetRowCheckBinding::HandleDetailClicked);
	}
	Bridge.Reset();
	RowObject = nullptr;
	CheckBox = nullptr;
	DetailButton = nullptr;
	EntryWidget = nullptr;
	bApplyingState = false;
}

void UGCSFleetRowCheckBinding::HandleCheckStateChanged(const bool bIsChecked)
{
	if (bApplyingState)
	{
		return;
	}
	if (UGCSUIBridgeSubsystem* Owner = Bridge.Get())
	{
		Owner->HandleFleetListCommandCheckChanged(RowObject, bIsChecked);
	}
}

void UGCSFleetRowCheckBinding::HandleDetailClicked()
{
	if (UGCSUIBridgeSubsystem* Owner = Bridge.Get())
	{
		Owner->HandleFleetListDetailClicked(RowObject);
	}
}
