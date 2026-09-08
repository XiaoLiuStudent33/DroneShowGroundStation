#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CheckBox.h"
#include "Components/Button.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGCSFleetListContractTest,
	"DroneGCS.UI.FleetList.AuthoredContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGCSFleetListContractTest::RunTest(const FString& Parameters)
{
	(void)Parameters;

	UClass* RowDataClass = LoadObject<UClass>(
		nullptr,
		TEXT("/Game/UI/BP_FleetRowData.BP_FleetRowData_C"));
	if (!TestNotNull(TEXT("BP_FleetRowData loads"), RowDataClass))
	{
		return false;
	}
	for (const FName Name :
		{ FName(TEXT("DroneID")), FName(TEXT("Mode")), FName(TEXT("GPSText")),
		  FName(TEXT("BatteryText")), FName(TEXT("HeightText")),
		  FName(TEXT("RSSIText")), FName(TEXT("StatusText")) })
	{
		TestNotNull(
			*FString::Printf(TEXT("BP_FleetRowData string %s"), *Name.ToString()),
			FindFProperty<FStrProperty>(RowDataClass, Name));
	}
	for (const FName Name :
		{ FName(TEXT("bCommandSelected")), FName(TEXT("bCommandEnabled")) })
	{
		TestNotNull(
			*FString::Printf(TEXT("BP_FleetRowData bool %s"), *Name.ToString()),
			FindFProperty<FBoolProperty>(RowDataClass, Name));
	}

	UGameInstance* GameInstance = NewObject<UGameInstance>(GEngine);
	GameInstance->InitializeStandalone(TEXT("GCSFleetListContractWorld"));
	UClass* PanelClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_FleetPanel_V2.WBP_GCS_FleetPanel_V2_C"));
	UUserWidget* Panel = PanelClass
		? CreateWidget<UUserWidget>(GameInstance, PanelClass)
		: nullptr;
	TestNotNull(TEXT("WBP_GCS_FleetPanel_V2 creates"), Panel);
	if (Panel && Panel->WidgetTree)
	{
		TestNotNull(
			TEXT("Fleet panel exposes LV_FleetDrones"),
			Cast<UListView>(Panel->WidgetTree->FindWidget(TEXT("LV_FleetDrones"))));
		TestNotNull(
			TEXT("Fleet panel exposes CHK_FleetSelectAll"),
			Cast<UCheckBox>(Panel->WidgetTree->FindWidget(TEXT("CHK_FleetSelectAll"))));
		TestNotNull(
			TEXT("Fleet panel exposes FleetCount"),
			Panel->WidgetTree->FindWidget(TEXT("FleetCount")));
		TestNotNull(
			TEXT("Fleet panel exposes TXT_FleetSelectedCount"),
			Panel->WidgetTree->FindWidget(TEXT("TXT_FleetSelectedCount")));
	}

	UClass* EntryClass = LoadClass<UUserWidget>(
		nullptr,
		TEXT("/Game/UI/WBP_GCS_FleetRow.WBP_GCS_FleetRow_C"));
	UUserWidget* Entry = EntryClass
		? CreateWidget<UUserWidget>(GameInstance, EntryClass)
		: nullptr;
	TestNotNull(TEXT("WBP_GCS_FleetRow creates"), Entry);
	if (Entry && Entry->WidgetTree)
	{
		TestNotNull(
			TEXT("Fleet row exposes CHK_FleetRowSelect"),
			Cast<UCheckBox>(Entry->WidgetTree->FindWidget(TEXT("CHK_FleetRowSelect"))));
		TestNotNull(
			TEXT("Fleet row exposes BTN_FleetRowDetail"),
			Cast<UButton>(Entry->WidgetTree->FindWidget(TEXT("BTN_FleetRowDetail"))));
		TestNotNull(
			TEXT("Fleet row exposes B_DetailSelected"),
			Entry->WidgetTree->FindWidget(TEXT("B_DetailSelected")));
		for (const FName Name :
			{ FName(TEXT("TXT_FleetRowID")), FName(TEXT("TXT_FleetRowMode")),
			  FName(TEXT("TXT_FleetRowGPS")), FName(TEXT("TXT_FleetRowBattery")),
			  FName(TEXT("TXT_FleetRowHeight")), FName(TEXT("TXT_FleetRowRSSI")),
			  FName(TEXT("TXT_FleetRowStatus")) })
		{
			TestNotNull(
				*FString::Printf(TEXT("Fleet row exposes text %s"), *Name.ToString()),
				Cast<UTextBlock>(Entry->WidgetTree->FindWidget(Name)));
		}
	}

	GameInstance->Shutdown();
	return true;
}

#endif
