#if WITH_DEV_AUTOMATION_TESTS
#include "GCSUIBridgeSubsystem.h"
#include "FleetManagerSubsystem.h"
#include "RoutePreviewSubsystem.h"
#include "GCSUIWidgetPresenter.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "UObject/UnrealType.h"
#include "Misc/AutomationTest.h"

// Deliberately no DSP provider, transport, upload implementation or coordinator header.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSApplicationNoWidgetTest,
    "DroneGCS.Application.NoWidgetAndRebind", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSApplicationNoWidgetTest::RunTest(const FString& Parameters)
{
    UGameInstance* GI = NewObject<UGameInstance>(GEngine);
    GI->InitializeStandalone(TEXT("ApplicationBoundaryWorld"));
    auto* API = GI->GetSubsystem<UGCSUIBridgeSubsystem>();
    auto* Fleet = GI->GetSubsystem<UFleetManagerSubsystem>();
    TestFalse(TEXT("No implicit upload on initialization"), API->GetWorkflowState().bHasJobSnapshot);
    TestEqual(TEXT("Idle workflow without a view"), API->GetWorkflowState().PreparationState, EGCSRouteAutoStartState::Idle);
    API->ApplyUploadConcurrency(TEXT("17"));
    TestEqual(TEXT("Explicit intent without TextBox"), API->GetWorkflowState().AppliedConcurrency, 17);
    FDroneData A; A.DroneID = TEXT("Drone003"); A.bOnline = true;
    FDroneData B; B.DroneID = TEXT("Drone009"); B.bOnline = true;
    A.GPSLat = 31.125; A.GPSLon = 118.5; A.Battery = 84.f;
    FGCSFleetSnapshot Snapshot;
    Snapshot.Header.SourceID = TEXT("final-readiness-fixture");
    Snapshot.Header.SessionID = TEXT("final-fleet-session");
    Snapshot.FleetRevision = 7; Snapshot.Drones = {A, B};
    TestTrue(TEXT("Seed normalized fixture through existing Fleet authority"), Fleet->ApplySnapshot(Snapshot));
    FDroneData Queried;
    TestTrue(TEXT("Vehicle query without Widget"), Fleet->GetDroneByID(A.DroneID, Queried));
    TestEqual(TEXT("Normalized telemetry value without Provider dependency"), Queried.GPSLat, A.GPSLat);
    TestEqual(TEXT("Vehicle paging without Widget"), Fleet->GetDronePage(0, 10).Num(), 2);
    TestEqual(TEXT("Fleet session query without Widget"), Fleet->GetActiveFleetSessionID(), Snapshot.Header.SessionID);
    const EDSPGCSTelemetryLinkState ConnectionBefore = API->GetConnectionState();
    const FGCSProductionCommandResult CommandBefore = API->GetCommandResult();
    TestTrue(TEXT("Typed connection query"), ConnectionBefore == EDSPGCSTelemetryLinkState::Disconnected
        || ConnectionBefore == EDSPGCSTelemetryLinkState::Connected);
    const auto TableBefore = API->GetTelemetryTableRows();
    TestEqual(TEXT("Read-only MDHK query is stable without a view"), API->GetTelemetryTableRows().Num(), TableBefore.Num());
    TestEqual(TEXT("Command readiness query is stable without a view"),
        API->CanExecuteOperatorCommand(EGCSProductionCommandType::Arm), API->CanSendOperatorCommands());
    TestEqual(TEXT("Read-only Command contract query preserves state"), API->GetCommandResult().State, CommandBefore.State);
    TestFalse(TEXT("RoutePreview query without Widget"), GI->GetSubsystem<URoutePreviewSubsystem>()->IsRouteLoaded());
    Fleet->SelectDrone(B.DroneID); API->SetCommandDroneSelected(A.DroneID, true);
    TestEqual(TEXT("Detail selection stays independent"), Fleet->GetCurrentSelectedDroneID(), B.DroneID);
    TestTrue(TEXT("Direction requires paused selection"), API->ExecuteOperatorCommand(EGCSProductionCommandType::MissionForward).IsEmpty());
    TestTrue(TEXT("Shutdown requires explicit confirmation"), API->ExecuteOperatorCommand(EGCSProductionCommandType::Shutdown).IsEmpty());
    TestTrue(TEXT("Command consumer bound without Main Widget"),
        API->GetCommandResultEvent().Contains(API, FName(TEXT("HandleProductionCommandResult"))));
    const auto Before = API->GetWorkflowState();
    for (int32 I=0; I<5; ++I) API->GetWorkflowState();
    TestEqual(TEXT("Queries never start a transaction"), API->GetWorkflowState().PreparationState, Before.PreparationState);
    TestFalse(TEXT("Queries never start a scheduled mission"), API->GetMissionState().bCountdownActive);
    UClass* MainClass = LoadClass<UUserWidget>(nullptr, TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
    if (!TestNotNull(TEXT("Main class"), MainClass)) { GI->Shutdown(); return false; }
    AddExpectedError(TEXT("Unable to load GCS navigation page: /Game/UI/WBP_GCS_PageMap."), EAutomationExpectedErrorFlags::Contains, 2);
    AddExpectedError(TEXT("Unable to load GCS navigation page: /Game/UI/WBP_GCS_PageLogs."), EAutomationExpectedErrorFlags::Contains, 2);
    for (int32 I=0; I<2; ++I)
    {
        auto* Main = CreateWidget<UUserWidget>(GI, MainClass);
        API->BindMainWidget(Main);
        TestEqual(TEXT("Rebind keeps applied workflow intent"), API->GetWorkflowState().AppliedConcurrency, 17);
        TestTrue(TEXT("Rebind keeps command selection"), API->GetSelectedCommandDroneIDs().Contains(A.DroneID));
        TestEqual(TEXT("Rebind keeps detail selection"), Fleet->GetCurrentSelectedDroneID(), B.DroneID);
        const bool Ready = API->CanSendOperatorCommands();
        auto* Panel = Cast<UUserWidget>(Main->WidgetTree->FindWidget(TEXT("CHILD_RightPanel")));
        auto* Button = Panel ? Cast<UButton>(Panel->WidgetTree->FindWidget(TEXT("BTN_Shutdown"))) : nullptr;
        if (TestNotNull(TEXT("Concrete command control"), Button))
        {
            Button->SetIsEnabled(!Ready); Button->SetVisibility(ESlateVisibility::Collapsed);
            TestEqual(TEXT("Widget flags cannot change command readiness"), API->CanSendOperatorCommands(), Ready);
        }
    }
    TestEqual(TEXT("Lifecycle cannot change Fleet session"), Fleet->GetActiveFleetSessionID(), Snapshot.Header.SessionID);
    TestEqual(TEXT("Lifecycle cannot write Fleet revision"), Fleet->GetFleetRevision(), int64(7));
    TestTrue(TEXT("Vehicle remains queryable after rebind"), Fleet->GetDroneByID(A.DroneID, Queried));
    TestEqual(TEXT("Lifecycle cannot rewrite telemetry"), Queried.GPSLat, A.GPSLat);
    TestEqual(TEXT("Lifecycle produces no Command result/identity"), API->GetCommandResult().CommandID, CommandBefore.CommandID);
    TestEqual(TEXT("Lifecycle does not change connection authority"), API->GetConnectionState(), ConnectionBefore);
    TestFalse(TEXT("Rebinding never creates a Job"), API->GetWorkflowState().bHasJobSnapshot);
    GI->Shutdown(); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSApplicationTelemetryProjectionTest,
    "DroneGCS.Application.TelemetryAndCommandProjection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGCSApplicationTelemetryProjectionTest::RunTest(const FString& Parameters)
{
    FMDHKTelemetryTableRow Row;
    Row.DroneID = TEXT("Drone007"); Row.bBatteryRemainingValid = true; Row.BatteryRemainingPercent = 82;
    Row.bBatteryTemperatureValid = true; Row.BatteryTemperatureCelsius = 21.25;
    Row.RGBText = TEXT("255,128,0"); Row.GPSUtc = TEXT("2026-09-10 00:00:00");
    UClass* RowClass = LoadClass<UObject>(nullptr, TEXT("/Game/UI/BP_TelemetryRowData.BP_TelemetryRowData_C"));
    if (!TestNotNull(TEXT("Legacy table projection target"), RowClass)) return false;
    UObject* Item = NewObject<UObject>(GetTransientPackage(), RowClass);
    TestTrue(TEXT("Stable data projects into all legacy columns"), FGCSUIWidgetPresenter::ApplyTelemetryRow(Item, Row));
    const auto* Battery = FindFProperty<FIntProperty>(RowClass, TEXT("Battery"));
    const auto* ID = FindFProperty<FIntProperty>(RowClass, TEXT("UavID"));
    const auto* RGB = FindFProperty<FStrProperty>(RowClass, TEXT("RGB"));
    if (Battery && ID && RGB)
    {
        TestEqual(TEXT("Logical display number unchanged"), ID->GetPropertyValue_InContainer(Item), 7);
        TestEqual(TEXT("Battery value unchanged"), Battery->GetPropertyValue_InContainer(Item), 82);
        TestEqual(TEXT("RGB text unchanged"), RGB->GetPropertyValue_InContainer(Item), Row.RGBText);
        Row.bBatteryRemainingValid = false;
        TestTrue(TEXT("Invalid sample still projects"), FGCSUIWidgetPresenter::ApplyTelemetryRow(Item, Row));
        TestEqual(TEXT("Invalid battery placeholder unchanged"), Battery->GetPropertyValue_InContainer(Item), -1);
    }
    FGCSProductionCommandResult Result;
    Result.CommandID=TEXT("command-a"); Result.DroneID=TEXT("Drone007");
    Result.AckState=TEXT("ACCEPTED"); Result.State=EGCSCommandResultState::Accepted;
    Result.Detail=TEXT("detail");
    TestEqual(TEXT("Result text stays byte-for-byte compatible"), FGCSUIWidgetPresenter::CommandResultText(Result),
        FString(TEXT("CMD Drone007  ACCEPTED  detail  command-a")));
    Result.State=EGCSCommandResultState::Failed;
    TestEqual(TEXT("Display text does not infer business state"), FGCSUIWidgetPresenter::CommandResultText(Result),
        FString(TEXT("CMD Drone007  ACCEPTED  detail  command-a")));
    return true;
}
#endif
