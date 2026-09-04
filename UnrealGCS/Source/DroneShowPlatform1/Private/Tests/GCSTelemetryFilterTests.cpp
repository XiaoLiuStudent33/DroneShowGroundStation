#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "GCSTelemetryFilterController.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SOverlay.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"
#include "DSPGCSProductionCommandProviderSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSTelemetryFilterModelTest,
	"DroneGCS.UI.TelemetryFilter.Model", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSTelemetryFilterModelTest::RunTest(const FString&)
{
	using namespace GCSTelemetryFilter;
	TestEqual(TEXT("32 table fields plus online"), Fields().Num(), 33);
	TSet<FName> Keys;
	for (const auto& Field : Fields()) Keys.Add(Field.Key);
	TestEqual(TEXT("Unique field keys"), Keys.Num(), 33);
	FMDHKTelemetryTableRow Row;
	Row.DroneID = TEXT("Drone009"); Row.bReceived = true;
	Row.BatteryRemainingPercent = 25; Row.bBatteryRemainingValid = true;
	FRule Low;
	FRule ID; ID.Field = TEXT("UavID"); ID.Operator = TEXT("="); ID.Input = TEXT("Drone009");
	TestTrue(TEXT("Low battery and ID"), Matches(Row, true, true, {Low, ID}, false));
	ID.Input = TEXT("10");
	TestFalse(TEXT("AND requires both"), Matches(Row, true, true, {Low, ID}, false));
	TestTrue(TEXT("OR accepts one"), Matches(Row, true, true, {Low, ID}, true));
	TestFalse(TEXT("Stale samples cannot match low battery"), Matches(Row, true, false, {Low}, false));
	Row.bBatteryRemainingValid = false;
	TestFalse(TEXT("Invalid samples are not zero"), Matches(Row, true, true, {Low}, false));
	Low.Operator = TEXT("!=");
	TestFalse(TEXT("Invalid samples do not match inequality"), Matches(Row, true, true, {Low}, false));
	Low.bEnabled = false;
	TestTrue(TEXT("Disabled filters ignored"), Matches(Row, {}, false, {Low}, true));
	TestTrue(TEXT("Empty OR shows all"), Matches(Row, {}, false, {}, true));
	FString Error;
	Low.bEnabled = true;
	for (const FString& Bad : {TEXT(""), TEXT("30oops"), TEXT("nan"), TEXT("inf"), TEXT("1e999"), TEXT("30%")})
	{
		Low.Input = Bad;
		TestFalse(*FString::Printf(TEXT("Reject malformed %s"), *Bad), Validate(Low, Error));
	}
	int64 N;
	TestTrue(TEXT("Decimal leading zeros"), ParseInteger(TEXT("009"), N)); TestEqual(TEXT("Decimal not octal"), N, int64(9));
	TestTrue(TEXT("Max int64 exact"), ParseInteger(TEXT("9223372036854775807"), N)); TestEqual(TEXT("Max"), N, MAX_int64);
	TestTrue(TEXT("Min int64 exact"), ParseInteger(TEXT("-9223372036854775808"), N)); TestEqual(TEXT("Min"), N, MIN_int64);
	TestFalse(TEXT("Reject integer overflow"), ParseInteger(TEXT("9223372036854775808"), N));
	TestFalse(TEXT("Reject hex suffix garbage"), ParseInteger(TEXT("0x10Z"), N));
	TestTrue(TEXT("Parse hex"), ParseInteger(TEXT("0xA5A50001"), N)); TestEqual(TEXT("Hex flags"), N, int64(0xA5A50001));
	Row.ValidFlags = 0xA5A50001;
	FRule Flags; Flags.Field = TEXT("ValidFlags"); Flags.Operator = TEXT("="); Flags.Input = TEXT("0xA5A50001");
	TestTrue(TEXT("Hex numeric equality"), Matches(Row, true, true, {Flags}, false));
	Row.FreeMemoryBytes = 9007199254740993ll;
	Flags.Field = TEXT("FreeMemoryBytes"); Flags.Input = TEXT("9007199254740992"); Flags.Operator = TEXT(">");
	TestTrue(TEXT("Int64 comparison does not round through double"), Matches(Row, true, true, {Flags}, false));
	FRule State; State.Field = TEXT("Online"); State.Operator = TEXT("="); State.Input = TEXT("离线");
	TestTrue(TEXT("Known offline"), Matches(Row, false, false, {State}, false));
	TestFalse(TEXT("Disconnected channel means unknown, not offline"), Matches(Row, {}, false, {State}, false));
	Row.HardwareID = TEXT("ABC123");
	FRule TextRule; TextRule.Field = TEXT("HardwareID"); TextRule.Operator = TEXT("包含"); TextRule.Input = TEXT("abc");
	TestTrue(TEXT("Text contains case-insensitive"), Matches(Row, true, true, {TextRule}, false));
	Row.GPSUtc = TEXT("-1");
	TestFalse(TEXT("GPS UTC sentinel invalid"), Read(Row, TEXT("GPSUtc"), true, true).bValid);
	TestFalse(TEXT("Cell voltage invalid"), Read(Row, TEXT("Cell1"), true, true).bValid);
	TestFalse(TEXT("Satellite sentinel invalid"), Read(Row, TEXT("SatellitesVisible"), true, true).bValid);
	TestFalse(TEXT("Battery current sentinel invalid"), Read(Row, TEXT("BatteryCurrent"), true, true).bValid);
	// All fields must be implemented, not just present in the picker.
	Row.bBatteryRemainingValid = Row.bBatteryTemperatureValid = true;
	Row.bCell1Valid = Row.bCell2Valid = Row.bCell3Valid = Row.bCell4Valid = true;
	Row.bGPSValid = Row.bAltitudeValid = true;
	Row.GPSUtc = TEXT("2026-09-04 12:00:00"); Row.ValidFlagsText = TEXT("0xA5A50001");
	Row.FlightSoftwareVersionText = TEXT("1.2.3"); Row.EKFFlagsText = TEXT("0x033F");
	Row.BatteryCycleCount = Row.CellVoltageDeltaMv = Row.SatellitesVisible = 1;
	Row.BatteryCurrentAmps = 1.2; Row.RGBText = TEXT("R=1 G=2 B=3"); Row.FlightCustomVersionText = TEXT("abcd");
	for (const auto& Field : Fields()) TestTrue(*Field.Key.ToString(), Read(Row, Field.Key, true, true).bValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSTelemetryFilterWidgetTest,
	"DroneGCS.UI.TelemetryFilter.AuthoredWidgets", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSTelemetryFilterWidgetTest::RunTest(const FString&)
{
	using namespace GCSTelemetryFilter;
	auto* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone(TEXT("TelemetryFilterTestWorld"));
	UClass* Class = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_TelemetryFilter.WBP_GCS_TelemetryFilter_C"));
	auto* Widget = Class ? CreateWidget<UUserWidget>(GI, Class) : nullptr;
	auto* C = NewObject<UGCSTelemetryFilterController>(GI);
	if (!Widget || !TestTrue(TEXT("Authored controls bind"), C->Bind(Widget))) { GI->Shutdown(); return false; }
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	const auto Click = [Widget](const TCHAR* Name) { CastChecked<UButton>(Widget->WidgetTree->FindWidget(Name))->OnClicked.Broadcast(); };
	const auto SetDraft = [C](const TCHAR* Key, const TCHAR* Op, const TCHAR* Input)
	{
		C->Field->SetSelectedOption(FindField(Key)->Label);
		C->Operator->SetSelectedOption(Op);
		C->Value->SetText(FText::FromString(Input));
	};
	const auto PumpList = [](UListView* List)
	{
		const auto Slate = List->TakeWidget();
		Slate->SlatePrepass();
		Slate->Tick(FGeometry::MakeRoot(FVector2D(284, 250), FSlateLayoutTransform()), FPlatformTime::Seconds(), 0.016f);
		for (auto* Entry : List->GetDisplayedEntryWidgets()) List->OnEntryWidgetGenerated().Broadcast(*Entry);
	};
	TestEqual(TEXT("33 fields plus placeholder"), C->Field->GetOptionCount(), 34);
	TestEqual(TEXT("Three modes"), C->MatchMode->GetOptionCount(), 3);
	TestEqual(TEXT("Default current mode"), C->MatchMode->GetSelectedOption(), FString(TEXT("当前条件")));
	TestEqual(TEXT("Initial field blank"), C->Field->GetSelectedOption(), FString(TEXT("--")));
	TestTrue(TEXT("Initial value empty"), C->Value->GetText().IsEmpty());
	TestFalse(TEXT("Operator disabled until field chosen"), C->Operator->GetIsEnabled());
	TestTrue(TEXT("Conditions single"), C->Conditions->GetSelectionMode() == ESelectionMode::Single);
	TestTrue(TEXT("Results checkbox-driven"), C->Results->GetSelectionMode() == ESelectionMode::None);
	for (UListView* List : {C->Conditions.Get(), C->Results.Get()})
	{
		UClass* EntryClass = List->GetEntryWidgetClass();
		if (!TestNotNull(TEXT("Entry class exists"), EntryClass)) continue;
		TestTrue(TEXT("Entry implements object list entry"), EntryClass->ImplementsInterface(UUserObjectListEntry::StaticClass()));
	}
	FMDHKTelemetryTableRow A; A.DroneID = TEXT("Drone002"); A.bReceived = true;
	A.SampleTimestampUtcMs = 10000; A.bBatteryRemainingValid = true; A.BatteryRemainingPercent = 25;
	FMDHKTelemetryTableRow B = A; B.DroneID = TEXT("Drone010"); B.BatteryRemainingPercent = 80;
	TMap<FString, bool> Online{{A.DroneID, true}, {B.DroneID, true}};
	C->UpdateTelemetry({B, A}, Online, true, 10000, 2000);
	TestEqual(TEXT("No condition means no results"), C->Results->GetNumItems(), 0);
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Apply with no selection creates blank"), C->Rules.Num(), 1);
	if (C->Rules.Num() != 1) { C->Shutdown(); GI->Shutdown(); return false; }
	auto* First = C->Rules[0].Get();
	TestTrue(TEXT("Blank has no field"), First->Rule.Field.IsNone());
	TestEqual(TEXT("Blank selected automatically"), C->Conditions->GetSelectedItem(), static_cast<UObject*>(First));
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Apply selected blank does not duplicate"), C->Rules.Num(), 1);
	SetDraft(TEXT("Battery"), TEXT("<"), TEXT(""));
	Click(TEXT("BTN_ApplyCondition"));
	TestTrue(TEXT("Partial invalid draft preserves blank"), First->Rule.Field.IsNone());
	TestFalse(TEXT("Partial draft error shown"), C->ErrorText->GetText().IsEmpty());
	SetDraft(TEXT("Battery"), TEXT("<"), TEXT("30"));
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Apply fills same row"), C->Rules.Num(), 1);
	TestEqual(TEXT("Low battery matches one"), C->Results->GetNumItems(), 1);
	TestEqual(TEXT("Independent count"), First->MatchCount, 1);
	PumpList(C->Conditions); PumpList(C->Results);
	auto* FirstEntry = C->Conditions->GetEntryWidgetFromItem(First);
	if (TestNotNull(TEXT("Actual condition entry generated"), FirstEntry))
	{
		auto* Count = CastChecked<UTextBlock>(FirstEntry->WidgetTree->FindWidget(TEXT("TXT_ConditionMatchCount")));
		TestEqual(TEXT("Actual count painted"), Count->GetText().ToString(), FString(TEXT("1")));
		auto* Border = CastChecked<UBorder>(FirstEntry->WidgetTree->FindWidget(TEXT("B_ConditionRow")));
		TestTrue(TEXT("Selected muted green color"), Border->GetBrushColor().Equals(FLinearColor::FromSRGBColor(FColor(46,125,91))));
		for (const TCHAR* Name : {TEXT("TXT_ConditionMatchCount"), TEXT("TXT_ConditionField"), TEXT("TXT_ConditionOperator"), TEXT("TXT_ConditionValue")})
			TestTrue(Name, CastChecked<UTextBlock>(FirstEntry->WidgetTree->FindWidget(Name))->GetColorAndOpacity().GetSpecifiedColor().Equals(FLinearColor::White));
		auto* Check = CastChecked<UCheckBox>(FirstEntry->WidgetTree->FindWidget(TEXT("CHK_ConditionEnabled")));
		Check->OnCheckStateChanged.Broadcast(false);
		TestEqual(TEXT("Disabled still viewable in current mode"), C->Results->GetNumItems(), 1);
		Check->OnCheckStateChanged.Broadcast(true);
	}
	if (auto* Entry = C->Results->GetEntryWidgetFromItem(C->Results->GetItemAt(0)))
	{
		TestEqual(TEXT("Actual result ID"), CastChecked<UTextBlock>(Entry->WidgetTree->FindWidget(TEXT("TXT_ResultDroneID")))->GetText().ToString(), A.DroneID);
		TestNotNull(TEXT("Actual result checkbox"), Entry->WidgetTree->FindWidget(TEXT("CHK_ResultSelected")));
	}
	// An unapplied draft must not change saved-field values or result membership.
	UObject* Stable = C->Results->GetItemAt(0);
	SetDraft(TEXT("UavID"), TEXT(">"), TEXT("unfinished"));
	C->UpdateTelemetry({A, B}, Online, true, 10001, 2000);
	TestEqual(TEXT("Result identity stable"), C->Results->GetItemAt(0), Stable);
	TestEqual(TEXT("Result still uses saved battery field"), CastChecked<UGCSTelemetryFilterItem>(Stable)->DisplayValue, Read(A, TEXT("Battery"), true, true).Text);
	TestEqual(TEXT("Draft survives telemetry refresh"), C->Value->GetText().ToString(), FString(TEXT("unfinished")));
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Invalid apply preserves saved field"), First->Rule.Field, FName(TEXT("Battery")));
	Click(TEXT("BTN_AddCondition"));
	TestEqual(TEXT("New always adds blank"), C->Rules.Num(), 2);
	auto* Second = C->Rules.Last().Get();
	TestTrue(TEXT("New never copies previous draft"), Second->Rule.Field.IsNone());
	TestEqual(TEXT("Blank selected clears results immediately"), C->Results->GetNumItems(), 0);
	TestEqual(TEXT("Blank selected resets editor"), C->Field->GetSelectedOption(), FString(TEXT("--")));
	C->Conditions->SetSelectedItem(First);
	TestEqual(TEXT("Selecting first immediately restores results"), C->Results->GetNumItems(), 1);
	TestEqual(TEXT("Selecting first restores saved value"), C->Value->GetText().ToString(), FString(TEXT("30")));
	C->Conditions->SetSelectedItem(Second);
	SetDraft(TEXT("UavID"), TEXT(">"), TEXT("5"));
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Second saved in place"), C->Rules.Num(), 2);
	TestEqual(TEXT("Second matches other drone"), CastChecked<UGCSTelemetryFilterItem>(C->Results->GetItemAt(0))->DroneID, B.DroneID);
	C->Conditions->SetSelectedItem(First);
	TestEqual(TEXT("Switch back changes result ID immediately"), CastChecked<UGCSTelemetryFilterItem>(C->Results->GetItemAt(0))->DroneID, A.DroneID);
	C->Field->SetSelectedOption(TEXT("--"));
	TestEqual(TEXT("Blank draft alone does not clear saved result"), C->Results->GetNumItems(), 1);
	Click(TEXT("BTN_ApplyCondition"));
	TestTrue(TEXT("Apply blank clears saved row"), First->Rule.Field.IsNone());
	TestEqual(TEXT("Applied blank clears results"), C->Results->GetNumItems(), 0);
	PumpList(C->Conditions);
	if (auto* Entry = C->Conditions->GetEntryWidgetFromItem(First))
	{
		for (const TCHAR* Name : {TEXT("TXT_ConditionField"), TEXT("TXT_ConditionOperator"), TEXT("TXT_ConditionValue")})
			TestEqual(Name, CastChecked<UTextBlock>(Entry->WidgetTree->FindWidget(Name))->GetText().ToString(), FString(TEXT("--")));
	}
	C->Conditions->ClearSelection();
	TestEqual(TEXT("Deselection clears current results"), C->Results->GetNumItems(), 0);
	SetDraft(TEXT("Battery"), TEXT("<"), TEXT("90"));
	Click(TEXT("BTN_ApplyCondition"));
	TestEqual(TEXT("Apply valid draft without selection creates row"), C->Rules.Num(), 3);
	TestEqual(TEXT("New valid row matches both"), C->Results->GetNumItems(), 2);
	C->UpdateTelemetry({A, B}, Online, true, 14000, 2000);
	TestEqual(TEXT("Stale values do not match"), C->Results->GetNumItems(), 0);
	TestEqual(TEXT("Stale independent count zero"), C->Rules.Last()->MatchCount, 0);
	Click(TEXT("BTN_RemoveCondition"));
	TestEqual(TEXT("Remove current row"), C->Rules.Num(), 2);
	Click(TEXT("BTN_ClearConditions"));
	TestEqual(TEXT("Clear all rows"), C->Rules.Num(), 0);
	TestEqual(TEXT("Clear leaves no results"), C->Results->GetNumItems(), 0);
	TestEqual(TEXT("Clear resets editor"), C->Field->GetSelectedOption(), FString(TEXT("--")));
	C->Shutdown();
	TestTrue(TEXT("Rebind successful"), C->Bind(Widget));
	SlateWidget = Widget->TakeWidget();
	Click(TEXT("BTN_AddCondition"));
	TestEqual(TEXT("One click one blank after rebind"), C->Rules.Num(), 1);
	TestEqual(TEXT("Rebind defaults current"), C->MatchMode->GetSelectedIndex(), 0);
	C->Shutdown(); SlateWidget.Reset(); Widget->ReleaseSlateResources(true); GI->Shutdown();
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSTelemetryFilterModesTest,
	"DroneGCS.UI.TelemetryFilter.CurrentAndCombinedModes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSTelemetryFilterModesTest::RunTest(const FString&)
{
	using namespace GCSTelemetryFilter;
	auto* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone(TEXT("TelemetryFilterModesWorld"));
	UClass* Class = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_TelemetryFilter.WBP_GCS_TelemetryFilter_C"));
	auto* Widget = Class ? CreateWidget<UUserWidget>(GI, Class) : nullptr;
	auto* C = NewObject<UGCSTelemetryFilterController>(GI);
	if (!Widget || !TestTrue(TEXT("Modes bind"), C->Bind(Widget))) { GI->Shutdown(); return false; }
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	TArray<FMDHKTelemetryTableRow> Rows;
	TMap<FString, bool> Online;
	for (int32 I = 0; I < 4; ++I)
	{
		FMDHKTelemetryTableRow R;
		R.DroneID = FString::Printf(TEXT("Drone%03d"), I);
		R.bReceived = true; R.SampleTimestampUtcMs = 10000;
		Rows.Add(R); Online.Add(R.DroneID, true);
	}
	C->UpdateTelemetry(Rows, Online, true, 10000, 2000);
	const auto CreateIDRule = [C](const TCHAR* Op, const TCHAR* Value)
	{
		C->AddCondition();
		C->Field->SetSelectedOption(FindField(TEXT("UavID"))->Label);
		C->Operator->SetSelectedOption(Op); C->Value->SetText(FText::FromString(Value));
		C->ApplyCondition();
		return C->Rules.Last().Get();
	};
	auto* GreaterTwo = CreateIDRule(TEXT(">"), TEXT("2"));
	auto* GreaterEqualZero = CreateIDRule(TEXT(">="), TEXT("0"));
	TestEqual(TEXT("Current ID >= 0 shows four, not previous intersection"), C->Results->GetNumItems(), 4);
	C->Conditions->SetSelectedItem(GreaterTwo);
	TestEqual(TEXT("Current ID > 2 immediately shows one"), C->Results->GetNumItems(), 1);
	TestEqual(TEXT("Current mode correct drone"), CastChecked<UGCSTelemetryFilterItem>(C->Results->GetItemAt(0))->DroneID, FString(TEXT("Drone003")));
	auto* LessOne = CreateIDRule(TEXT("<"), TEXT("1"));
	TestEqual(TEXT("Current ID < 1 shows Drone000 despite conflicting other rules"),
		CastChecked<UGCSTelemetryFilterItem>(C->Results->GetItemAt(0))->DroneID, FString(TEXT("Drone000")));
	LessOne->OnEnabledChanged(false);
	TestEqual(TEXT("Current ignores combination enable flag"), C->Results->GetNumItems(), 1);
	C->MatchMode->SetSelectedIndex(2);
	TestEqual(TEXT("AND skips disabled rule"), C->Results->GetNumItems(), 1);
	LessOne->OnEnabledChanged(true);
	TestEqual(TEXT("AND of conflicting rules empty"), C->Results->GetNumItems(), 0);
	C->Conditions->SetSelectedItem(GreaterEqualZero);
	TestEqual(TEXT("AND unaffected by selected row"), C->Results->GetNumItems(), 0);
	C->MatchMode->SetSelectedIndex(1);
	TestEqual(TEXT("OR union of rules shows four"), C->Results->GetNumItems(), 4);
	GreaterEqualZero->OnEnabledChanged(false);
	TestEqual(TEXT("OR union of disjoint rules shows two"), C->Results->GetNumItems(), 2);
	TestEqual(TEXT("Individual broad count remains four"), GreaterEqualZero->MatchCount, 4);
	C->AddCondition();
	auto* Blank = C->Rules.Last().Get();
	TestEqual(TEXT("OR ignores newly added blank"), C->Results->GetNumItems(), 2);
	TestEqual(TEXT("Blank independent count zero"), Blank->MatchCount, 0);
	C->MatchMode->SetSelectedIndex(0);
	TestEqual(TEXT("Current blank clears results"), C->Results->GetNumItems(), 0);
	C->Conditions->SetSelectedItem(GreaterTwo);
	LessOne->OnEnabledChanged(false);
	C->MatchMode->SetSelectedIndex(2);
	TestEqual(TEXT("AND ignores enabled blank rather than rejecting all"), C->Results->GetNumItems(), 1);
	GreaterTwo->OnEnabledChanged(false);
	for (int32 Mode : {1, 2})
	{
		C->MatchMode->SetSelectedIndex(Mode);
		TestEqual(TEXT("No enabled valid rules means empty in combined mode"), C->Results->GetNumItems(), 0);
	}
	C->MatchMode->SetSelectedIndex(0);
	TestEqual(TEXT("Disabled selected rule still viewable"), C->Results->GetNumItems(), 1);
	C->Conditions->ClearSelection();
	TestEqual(TEXT("No selected rule clears current results"), C->Results->GetNumItems(), 0);
	C->ClearConditions();
	for (int32 Mode : {0, 1, 2})
	{
		C->MatchMode->SetSelectedIndex(Mode);
		TestEqual(TEXT("Cleared list empty in every mode"), C->Results->GetNumItems(), 0);
	}
	C->Shutdown(); SlateWidget.Reset(); Widget->ReleaseSlateResources(true); GI->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSTelemetryFilterSelectionTest,
	"DroneGCS.UI.TelemetryFilter.CountsAndSelection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSTelemetryFilterSelectionTest::RunTest(const FString&)
{
	using namespace GCSTelemetryFilter;
	UGameInstance* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone(TEXT("TelemetryFilterSelectionWorld"));
	auto* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
	auto* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
	auto* Commands = GI->GetSubsystem<UDSPGCSProductionCommandProviderSubsystem>();
	const auto BeforeCommand = Commands->GetLastResult();
	UClass* Class = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_TelemetryFilter.WBP_GCS_TelemetryFilter_C"));
	UUserWidget* Widget = Class ? CreateWidget<UUserWidget>(GI, Class) : nullptr;
	auto* C = NewObject<UGCSTelemetryFilterController>(GI);
	if (!Widget || !TestTrue(TEXT("Selection controls bind"), C->Bind(Widget, Bridge))) { GI->Shutdown(); return false; }
	TSharedPtr<SWidget> SlateWidget = Widget->TakeWidget();
	const auto PumpList = [](UListView* List)
	{
		const auto SlateList = List->TakeWidget();
		SlateList->SlatePrepass();
		SlateList->Tick(FGeometry::MakeRoot(FVector2D(284, 96), FSlateLayoutTransform()), FPlatformTime::Seconds(), 0.016f);
		for (UUserWidget* Entry : List->GetDisplayedEntryWidgets()) List->OnEntryWidgetGenerated().Broadcast(*Entry);
	};
	TArray<FMDHKTelemetryTableRow> Rows;
	TMap<FString, bool> Online;
	for (int32 Index = 0; Index < 80; ++Index)
	{
		FMDHKTelemetryTableRow Row;
		Row.DroneID = FString::Printf(TEXT("Drone%03d"), Index);
		Row.bReceived = Row.bBatteryRemainingValid = true;
		Row.BatteryRemainingPercent = Index < 40 ? 25 : 80;
		Row.SampleTimestampUtcMs = 10000;
		Rows.Add(Row); Online.Add(Row.DroneID, true);
		FDroneData Drone; Drone.DroneID = Row.DroneID; Drone.bOnline = true;
		Fleet->AddDrone(Drone);
	}
	Fleet->SelectDrone(TEXT("Drone079"));
	const FString DetailBefore = Fleet->GetCurrentSelectedDroneID();
	Bridge->SetCommandDroneSelected(TEXT("OutsideResults"), true);
	C->UpdateTelemetry(Rows, Online, true, 10000, 2000);
	C->MatchMode->SetSelectedIndex(2);
	C->Field->SetSelectedOption(FindField(TEXT("Battery"))->Label);
	C->Operator->SetSelectedOption(TEXT("<")); C->Value->SetText(FText::FromString(TEXT("30")));
	C->ApplyCondition();
	TestEqual(TEXT("Low battery independent count 40"), C->Rules[0]->MatchCount, 40);
	C->Conditions->ClearSelection();
	C->Field->SetSelectedOption(FindField(TEXT("UavID"))->Label);
	C->Operator->SetSelectedOption(TEXT("<")); C->Value->SetText(FText::FromString(TEXT("10")));
	C->ApplyCondition();
	TestEqual(TEXT("Second condition independent count 10"), C->Rules[1]->MatchCount, 10);
	TestEqual(TEXT("AND combined count 10"), C->Results->GetNumItems(), 10);
	C->MatchMode->SetSelectedIndex(1);
	TestEqual(TEXT("OR combined count 40"), C->Results->GetNumItems(), 40);
	TestEqual(TEXT("OR does not change independent count"), C->Rules[1]->MatchCount, 10);
	C->ClearConditions();
	C->Field->SetSelectedOption(FindField(TEXT("UavID"))->Label);
	C->Operator->SetSelectedOption(TEXT(">=")); C->Value->SetText(FText::FromString(TEXT("0")));
	C->ApplyCondition();
	TestEqual(TEXT("Explicit ID rule after clear matches 80"), C->Results->GetNumItems(), 80);
	PumpList(C->Results);
	TestTrue(TEXT("Results are virtualized with offscreen rows"), C->Results->GetDisplayedEntryWidgets().Num() < 80);
	UUserWidget* Entry = C->Results->GetEntryWidgetFromItem(C->Results->GetItemAt(0));
	if (TestNotNull(TEXT("First actual row generated"), Entry))
	{
		auto* Check = Cast<UCheckBox>(Entry->WidgetTree->FindWidget(TEXT("CHK_ResultSelected")));
		if (TestNotNull(TEXT("Authored result checkbox"), Check))
		{
			Check->OnCheckStateChanged.Broadcast(true);
			TestTrue(TEXT("Row checks bridge target"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("Drone000")));
			TestTrue(TEXT("Partial select-all state"), C->SelectAll->GetCheckedState() == ECheckBoxState::Undetermined);
			Bridge->SetCommandDroneSelected(TEXT("Drone000"), false);
			TestFalse(TEXT("External fleet deselection paints checkbox"), Check->IsChecked());
			C->ResultReleased(*Entry);
			Check->OnCheckStateChanged.Broadcast(true);
			TestFalse(TEXT("Released entry has no stale checkbox delegate"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("Drone000")));
			C->ResultGenerated(*Entry);
			C->ResultGenerated(*Entry);
		}
	}
	int32 Changes = 0;
	const FDelegateHandle Handle = Bridge->OnCommandSelectionChanged.AddLambda([&Changes]() { ++Changes; });
	C->SelectAll->OnCheckStateChanged.Broadcast(true);
	TestEqual(TEXT("Bulk selection is one atomic mutation"), Changes, 1);
	TestEqual(TEXT("All 80 results plus unrelated selection"), Bridge->GetSelectedCommandDroneIDs().Num(), 81);
	TestTrue(TEXT("Offscreen last result selected"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("Drone079")));
	TestTrue(TEXT("All checkbox checked"), C->SelectAll->GetCheckedState() == ECheckBoxState::Checked);
	// Narrowing filters does not remove selection; unchecking only affects the current subset.
	C->Field->SetSelectedOption(FindField(TEXT("Battery"))->Label);
	C->Operator->SetSelectedOption(TEXT("<")); C->Value->SetText(FText::FromString(TEXT("30")));
	C->ApplyCondition();
	C->SelectAll->OnCheckStateChanged.Broadcast(false);
	TestEqual(TEXT("Deselect subset preserves 40 outside plus unrelated"), Bridge->GetSelectedCommandDroneIDs().Num(), 41);
	TestTrue(TEXT("Unrelated choice retained"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("OutsideResults")));
	TestFalse(TEXT("Current subset cleared"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("Drone000")));
	C->SelectAll->OnCheckStateChanged.Broadcast(true);
	Bridge->SetCommandDroneSelected(TEXT("Drone070"), false);
	Rows[70].BatteryRemainingPercent = 20;
	C->UpdateTelemetry(Rows, Online, true, 10001, 2000);
	if (!TestTrue(TEXT("Updated result contains Drone070"), C->ResultItems.Contains(TEXT("Drone070"))))
	{
		AddInfo(FString::Printf(TEXT("Rules=%d Results=%d Editor=%s"), C->Rules.Num(), C->Results->GetNumItems(), *C->Field->GetSelectedOption()));
		for (const auto& Item : C->Rules)
			AddInfo(FString::Printf(TEXT("Rule %s %s %s enabled=%d"), *Item->Rule.Field.ToString(), *Item->Rule.Operator, *Item->Rule.Input, Item->Rule.bEnabled));
		Bridge->OnCommandSelectionChanged.Remove(Handle);
		C->Shutdown(); SlateWidget.Reset(); Widget->ReleaseSlateResources(true); GI->Shutdown();
		return false;
	}
	TestFalse(TEXT("Newly matching result is not auto selected"), C->ResultItems[TEXT("Drone070")]->bSelected);
	TestTrue(TEXT("New result makes select-all partial"), C->SelectAll->GetCheckedState() == ECheckBoxState::Undetermined);
	C->SelectAll->OnCheckStateChanged.Broadcast(false); // Slate's click event for Undetermined.
	TestTrue(TEXT("Clicking partial selects remaining current results"), C->ResultItems[TEXT("Drone070")]->bSelected);
	TestTrue(TEXT("Partial click becomes fully checked"), C->SelectAll->GetCheckedState() == ECheckBoxState::Checked);
	Bridge->SetCommandDroneSelected(TEXT("Drone070"), false);
	auto* OldItem = C->ResultItems[TEXT("Drone000")].Get();
	C->UpdateTelemetry(Rows, Online, true, 14000, 2000);
	TestEqual(TEXT("Expired results empty"), C->Results->GetNumItems(), 0);
	TestFalse(TEXT("Empty select-all disabled"), C->SelectAll->GetIsEnabled());
	TestTrue(TEXT("Empty select-all unchecked"), C->SelectAll->GetCheckedState() == ECheckBoxState::Unchecked);
	OldItem->OnSelectedChanged(false);
	TestTrue(TEXT("Old removed item cannot mutate selection"), Bridge->GetSelectedCommandDroneIDs().Contains(TEXT("Drone000")));
	TestEqual(TEXT("Detail selection untouched"), Fleet->GetCurrentSelectedDroneID(), DetailBefore);
	TestEqual(TEXT("Selection does not create a command result"), Commands->GetLastResult().CommandID, BeforeCommand.CommandID);
	Bridge->OnCommandSelectionChanged.Remove(Handle);
	C->Shutdown();
	const int32 BeforeShutdownClick = Bridge->GetSelectedCommandDroneIDs().Num();
	Cast<UCheckBox>(Widget->WidgetTree->FindWidget(TEXT("CHK_SelectAllResults")))->OnCheckStateChanged.Broadcast(false);
	TestEqual(TEXT("Shutdown removes bulk handler"), Bridge->GetSelectedCommandDroneIDs().Num(), BeforeShutdownClick);
	TestTrue(TEXT("Rebind selection controller"), C->Bind(Widget, Bridge));
	C->Field->SetSelectedOption(FindField(TEXT("UavID"))->Label);
	C->Operator->SetSelectedOption(TEXT(">=")); C->Value->SetText(FText::FromString(TEXT("0")));
	C->ApplyCondition();
	C->UpdateTelemetry(Rows, Online, true, 10002, 2000);
	TestTrue(TEXT("Rebind reads persisted bridge selection"), C->SelectAll->GetCheckedState() == ECheckBoxState::Undetermined);
	C->Shutdown(); SlateWidget.Reset(); Widget->ReleaseSlateResources(true); GI->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSTelemetryFilterToggleTest,
	"DroneGCS.UI.TelemetryFilter.IndependentToolbarToggle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSTelemetryFilterToggleTest::RunTest(const FString&)
{
	UGameInstance* GI = NewObject<UGameInstance>(GEngine);
	GI->InitializeStandalone(TEXT("TelemetryFilterToggleWorld"));
	// Retained offscreen viewport: exercise the production AddToViewport path
	// without opening an editor/game window or enabling any production channel.
	UGameViewportClient* Viewport = NewObject<UGameViewportClient>(GEngine);
	GI->GetWorldContext()->GameViewport = Viewport;
	TSharedRef<SOverlay> Overlay = SNew(SOverlay);
	Viewport->SetViewportOverlayWidget(nullptr, Overlay);
	UGCSUIBridgeSubsystem* Bridge = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
	UClass* MainClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
	UUserWidget* Main = MainClass ? CreateWidget<UUserWidget>(GI, MainClass) : nullptr;
	if (!TestNotNull(TEXT("Main asset loads"), Main)) { GI->Shutdown(); return false; }
	Bridge->MainWidgetRef = Main;
	Bridge->RightToolbarWidget = Cast<UUserWidget>(Main->WidgetTree->FindWidget(TEXT("CHILD_RightToolbar")));
	Bridge->BindTelemetryFilterToggle();
	UButton* Button = Bridge->TelemetryFilterToggleButton.Get();
	if (!TestNotNull(TEXT("Saved BTN_ShaiXuan binds"), Button)) { GI->Shutdown(); return false; }
	TestFalse(TEXT("Filter not created at startup"), Bridge->TelemetryFilterWidget.IsValid());
	Bridge->ShowMainPage(EGCSMainPage::Logs);
	TestFalse(TEXT("Opening logs does not open filter"), Bridge->TelemetryFilterWidget.IsValid());
	Button->OnClicked.Broadcast();
	UUserWidget* Filter = Bridge->TelemetryFilterWidget.Get();
	if (!TestNotNull(TEXT("Toolbar creates standalone filter"), Filter)) { GI->Shutdown(); return false; }
	TestTrue(TEXT("First click shows filter"), Filter->IsVisible());
	TestNull(TEXT("Filter not parented to logs"), Filter->GetParent());
	TestTrue(TEXT("Filter in viewport"), Filter->IsInViewport());
	TestTrue(TEXT("Fixed right anchor"), Filter->GetAnchorsInViewport().Minimum.Equals(FVector2D(1, 0)));
	TestTrue(TEXT("Right aligned"), Filter->GetAlignmentInViewport().Equals(FVector2D(1, 0)));
	TSharedPtr<SWidget> SlateWidget = Filter->TakeWidget();
	UListView* Conditions = Cast<UListView>(Filter->WidgetTree->FindWidget(TEXT("LV_FilterConditions")));
	auto* Field = Cast<UComboBoxString>(Filter->WidgetTree->FindWidget(TEXT("CMB_FilterField")));
	TestEqual(TEXT("Toolbar first open starts neutral"), Field->GetSelectedOption(), FString(TEXT("--")));
	Field->SetSelectedOption(GCSTelemetryFilter::FindField(TEXT("Battery"))->Label);
	Cast<UComboBoxString>(Filter->WidgetTree->FindWidget(TEXT("CMB_Operator")))->SetSelectedOption(TEXT("<"));
	Cast<UEditableTextBox>(Filter->WidgetTree->FindWidget(TEXT("EDT_FilterValue")))->SetText(FText::FromString(TEXT("30")));
	Cast<UButton>(Filter->WidgetTree->FindWidget(TEXT("BTN_ApplyCondition")))->OnClicked.Broadcast();
	TestEqual(TEXT("Condition added"), Conditions->GetNumItems(), 1);
	UObject* Rule = Conditions->GetItemAt(0);
	Bridge->ShowMainPage(EGCSMainPage::Logs);
	TestTrue(TEXT("Closing logs does not hide filter"), Filter->IsVisible());
	Bridge->ShowMainPage(EGCSMainPage::Map);
	TestTrue(TEXT("Changing pages does not hide filter"), Filter->IsVisible());
	Bridge->BindTelemetryFilterToggle();
	Button->OnClicked.Broadcast();
	TestFalse(TEXT("One click hides after rebind (no duplicate handler)"), Filter->IsVisible());
	Bridge->ShowMainPage(EGCSMainPage::Logs);
	TestFalse(TEXT("Opening logs does not reopen hidden filter"), Filter->IsVisible());
	Button->OnClicked.Broadcast();
	TestTrue(TEXT("Next click reopens same widget"), Bridge->TelemetryFilterWidget.Get() == Filter && Filter->IsVisible());
	TestEqual(TEXT("Condition retained across hide/show"), Conditions->GetItemAt(0), Rule);
	TestEqual(TEXT("Hide/show retains selected editor field"), Field->GetSelectedOption(), GCSTelemetryFilter::FindField(TEXT("Battery"))->Label);
	Bridge->ResetTelemetryFilter();
	TestFalse(TEXT("Cleanup removes viewport widget"), Filter->IsInViewport());
	Button->OnClicked.Broadcast();
	TestFalse(TEXT("Cleanup unbinds button"), Bridge->TelemetryFilterWidget.IsValid());
	SlateWidget.Reset();
	GI->Shutdown();
	return true;
}
#endif
