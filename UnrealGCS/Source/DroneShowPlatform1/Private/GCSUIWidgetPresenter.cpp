#include "GCSUIWidgetPresenter.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "GCSUIBridgeSubsystem.h"
#include "UObject/UnrealType.h"

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

namespace
{
	bool SetTelemetryInteger(UObject* Object, const FName Name, const int32 Value)
	{
		if (FIntProperty* Property = FindFProperty<FIntProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetTelemetryInteger64(UObject* Object, const FName Name, const int64 Value)
	{
		if (FInt64Property* Property = FindFProperty<FInt64Property>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetTelemetryString(UObject* Object, const FName Name, const FString& Value)
	{
		if (FStrProperty* Property = FindFProperty<FStrProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		return false;
	}

	bool SetTelemetryReal(UObject* Object, const FName Name, const double Value)
	{
		if (FDoubleProperty* Property = FindFProperty<FDoubleProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, Value);
			return true;
		}
		if (FFloatProperty* Property = FindFProperty<FFloatProperty>(Object->GetClass(), Name))
		{
			Property->SetPropertyValue_InContainer(Object, static_cast<float>(Value));
			return true;
		}
		return false;
	}
}

bool FGCSUIWidgetPresenter::ApplyTelemetryRow(UObject* RowObject, const FMDHKTelemetryTableRow& Row)
{
	if (!RowObject) return false;
		int32 DigitStart = Row.DroneID.Len();
		while (DigitStart > 0 && FChar::IsDigit(Row.DroneID[DigitStart - 1])) --DigitStart;
		const int32 LogicalID = DigitStart < Row.DroneID.Len() ? FCString::Atoi(*Row.DroneID.Mid(DigitStart)) : INDEX_NONE;
		return
			SetTelemetryInteger(RowObject, TEXT("UavID"), LogicalID) &&
			SetTelemetryInteger(RowObject, TEXT("Battery"),
				Row.bBatteryRemainingValid ? Row.BatteryRemainingPercent : -1) &&
			SetTelemetryReal(RowObject, TEXT("Temperature"),
				Row.bBatteryTemperatureValid ? Row.BatteryTemperatureCelsius : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell1"), Row.bCell1Valid ? Row.Cell1Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell2"), Row.bCell2Valid ? Row.Cell2Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell3"), Row.bCell3Valid ? Row.Cell3Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Cell4"), Row.bCell4Valid ? Row.Cell4Volts : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Latitude"), Row.bGPSValid ? Row.LatitudeDeg : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Longitude"), Row.bGPSValid ? Row.LongitudeDeg : 0.0) &&
			SetTelemetryReal(RowObject, TEXT("Altitude"),
				Row.bAltitudeValid ? Row.AltitudeAmslMeters : 0.0) &&
			SetTelemetryInteger64(RowObject, TEXT("TimeBootMs"), Row.TimeBootMs) &&
			SetTelemetryString(RowObject, TEXT("GPSUtc"), Row.GPSUtc) &&
			SetTelemetryString(RowObject, TEXT("ValidFlags"), Row.ValidFlagsText) &&
			SetTelemetryReal(RowObject, TEXT("RelativeAltitude"), Row.RelativeAltitudeMeters) &&
			SetTelemetryInteger64(RowObject, TEXT("FreeMemoryBytes"), Row.FreeMemoryBytes) &&
			SetTelemetryString(RowObject, TEXT("FlightSWVersion"), Row.FlightSoftwareVersionText) &&
			SetTelemetryReal(RowObject, TEXT("WaypointDistance"), Row.WaypointDistanceMeters) &&
			SetTelemetryReal(RowObject, TEXT("CompassVariance"), Row.CompassVariance) &&
			SetTelemetryReal(RowObject, TEXT("MagField1"), Row.MagneticField1Mgauss) &&
			SetTelemetryReal(RowObject, TEXT("MagField2"), Row.MagneticField2Mgauss) &&
			SetTelemetryInteger(RowObject, TEXT("BatteryCycleCount"), Row.BatteryCycleCount) &&
			SetTelemetryInteger(RowObject, TEXT("CellVoltageDeltaMv"), Row.CellVoltageDeltaMv) &&
			SetTelemetryString(RowObject, TEXT("EKFFlags"), Row.EKFFlagsText) &&
			SetTelemetryInteger(RowObject, TEXT("MissionSeq"), Row.MissionSequence) &&
			SetTelemetryInteger(RowObject, TEXT("MissionTotal"), Row.MissionTotal) &&
			SetTelemetryReal(RowObject, TEXT("BatteryCurrent"), Row.BatteryCurrentAmps) &&
			SetTelemetryInteger(RowObject, TEXT("GPSFixType"), Row.GPSFixType) &&
			SetTelemetryInteger(RowObject, TEXT("SatellitesVisible"), Row.SatellitesVisible) &&
			SetTelemetryInteger(RowObject, TEXT("PayloadSchemaVersion"), Row.PayloadSchemaVersion) &&
			SetTelemetryString(RowObject, TEXT("RGB"), Row.RGBText) &&
			SetTelemetryString(RowObject, TEXT("FlightCustomVersion"), Row.FlightCustomVersionText) &&
			SetTelemetryString(RowObject, TEXT("HardwareID"), Row.HardwareID);
}

FString FGCSUIWidgetPresenter::CommandResultText(const FGCSProductionCommandResult& Result)
{
	const FString StateText =
		Result.CommandType == EGCSProductionCommandType::HoverTest && Result.bSimulated
			? TEXT("WAITING_COMMAND")
			: Result.AckState;
	const FString Diagnostic = !Result.Detail.IsEmpty()
		? Result.Detail
		: Result.ErrorCode;
	return FString::Printf(
		TEXT("CMD %s  %s%s%s  %s"),
		*Result.DroneID,
		*StateText,
		Diagnostic.IsEmpty() ? TEXT("") : TEXT("  "),
		*Diagnostic,
		*Result.CommandID);
}
