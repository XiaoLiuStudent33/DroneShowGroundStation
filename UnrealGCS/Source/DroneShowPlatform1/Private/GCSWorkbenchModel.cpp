#include "GCSWorkbenchModel.h"
#include "FleetManagerSubsystem.h"
#include "GCSUIBridgeSubsystem.h"
#include "GCSInvisiblePawn.h"
#include "GCSMapSubsystem.h"
#include "DroneFleetVisualizationActor.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

FGCSWorkbenchModel::FGCSWorkbenchModel(APlayerController* InController):Controller(InController)
{
    // Restore per-map visual calibration before framing the new workbench.
    if(InController && InController->GetGameInstance())
    {
        if(auto* Map=InController->GetGameInstance()->GetSubsystem<UGCSMapSubsystem>()) Map->EnsureMap();
        if(auto* Pawn=Cast<AGCSInvisiblePawn>(InController->GetPawn())) Pawn->ResetOrbitView();
    }
}
UFleetManagerSubsystem* FGCSWorkbenchModel::Fleet() const
{ auto* GI=Controller.IsValid()?Controller->GetGameInstance():nullptr; return GI?GI->GetSubsystem<UFleetManagerSubsystem>():nullptr; }
UGCSUIBridgeSubsystem* FGCSWorkbenchModel::Bridge() const
{ auto* GI=Controller.IsValid()?Controller->GetGameInstance():nullptr; return GI?GI->GetSubsystem<UGCSUIBridgeSubsystem>():nullptr; }
ADroneFleetVisualizationActor* FGCSWorkbenchModel::Visualization() const
{
    if(Controller.IsValid()) for(TActorIterator<ADroneFleetVisualizationActor> It(Controller->GetWorld());It;++It) return *It;
    return nullptr;
}
bool FGCSWorkbenchModel::CanSelect() const { return Bridge() && !Bridge()->IsCommandSelectionMutationBlocked(); }
void FGCSWorkbenchModel::Refresh()
{
    auto* F=Fleet(); auto* B=Bridge(); if(!F || !B) return;
    if(Revision!=F->GetFleetRevision() || Session!=F->GetActiveFleetSessionID())
    {
        const bool NewSession=Revision!=MIN_int64 && Session!=F->GetActiveFleetSessionID();
        Session=F->GetActiveFleetSessionID(); Revision=F->GetFleetRevision();
        TMap<FString,TSharedPtr<FDroneData>> Old;
        for(const auto& Row:Rows) Old.Add(Row->DroneID,Row);
        Rows.Reset(); OnlineCount=0;
        for(const auto& Data:F->GetDronePage(0,F->GetDroneCount()))
        {
            auto Row=Old.FindRef(Data.DroneID);
            if(!Row.IsValid()) Row=MakeShared<FDroneData>();
            *Row=Data; Rows.Add(Row); OnlineCount+=Data.bOnline?1:0;
        }
        // Removed/offline aircraft must not survive as future command targets.
        TArray<FString> ValidSelection;
        if(!NewSession) for(const FString& ID:B->GetSelectedCommandDroneIDs())
        { FDroneData D; if(F->GetDroneByID(ID,D) && D.bOnline) ValidSelection.Add(ID); }
        B->ReplaceCommandDroneSelection(ValidSelection);
        Filter();
    }
    CheckedIDs.Reset();
    for(const auto& ID:B->GetSelectedCommandDroneIDs()) CheckedIDs.Add(ID);
}
void FGCSWorkbenchModel::Filter()
{
    TArray<TSharedPtr<FDroneData>> Next;
    for(const auto& Row:Rows) if(Search.IsEmpty() || Row->DroneID.Contains(Search)) Next.Add(Row);
    if(Next!=Filtered) { Filtered=MoveTemp(Next); ++ListGeneration; }
}
void FGCSWorkbenchModel::SetSearch(const FString& Value) { Search=Value.TrimStartAndEnd(); Filter(); }
void FGCSWorkbenchModel::SetChecked(const FString& ID,bool Checked)
{
    FDroneData D;
    if(CanSelect() && Fleet()->GetDroneByID(ID,D) && (!Checked || D.bOnline)) Bridge()->SetCommandDroneSelected(ID,Checked);
    Refresh();
}
void FGCSWorkbenchModel::SelectFiltered(bool Checked)
{
    if(!CanSelect()) return;
    TSet<FString> Next=CheckedIDs;
    for(const auto& Row:Filtered) if(Row->bOnline)
    { if(Checked) Next.Add(Row->DroneID); else Next.Remove(Row->DroneID); }
    Bridge()->ReplaceCommandDroneSelection(Next.Array()); Refresh();
}
void FGCSWorkbenchModel::Inspect(const FString& ID) { if(Fleet()) Fleet()->SelectDrone(ID); }
FString FGCSWorkbenchModel::DetailID() const { return Fleet()?Fleet()->GetCurrentSelectedDroneID():FString(); }
FString FGCSWorkbenchModel::DetailText() const
{
    FDroneData D;
    if(!Fleet() || !Fleet()->GetSelectedDrone(D)) return TEXT("点击列表行或场景中的飞机查看详情");
    return FString::Printf(TEXT("%s  ·  %s\n模式 %s  /  电量 %.0f%%\n相对高度 %s  /  GPS %d"),*D.DroneID,D.bOnline?TEXT("在线"):TEXT("离线"),*D.Mode,D.Battery,
        D.bAltitudeValid?*FString::Printf(TEXT("%.1f m"),D.Altitude):TEXT("--"),D.GPSFixType);
}
bool FGCSWorkbenchModel::Connected() const
{
    auto* GI=Controller.IsValid()?Controller->GetGameInstance():nullptr;
    auto* P=GI?GI->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>():nullptr;
    return P && P->IsTelemetryLinkConnected();
}
FString FGCSWorkbenchModel::LinkText() const { return Connected()?TEXT("遥测已连接"):TEXT("等待服务器遥测"); }
bool FGCSWorkbenchModel::SceneFrame(FVector& O,FVector& E,FVector& N,FVector& U) const
{
    if(auto* V=Visualization()) if(V->TryGetGeographicTangentBasis(O,E,N,U)) return true;
    if(Controller.IsValid()) for(TActorIterator<AActor> It(Controller->GetWorld());It;++It)
        if(It->ActorHasTag(TEXT("GCS.SceneMap")) || It->GetClass()->GetName().StartsWith(TEXT("BP_GCS_2DMapGround")))
        { O=It->GetActorLocation(); E=It->GetActorForwardVector(); U=It->GetActorUpVector(); N=FVector::CrossProduct(U,E).GetSafeNormal(); return true; }
    return false;
}
bool FGCSWorkbenchModel::Locate()
{
    auto* Pawn=Controller.IsValid()?Cast<AGCSInvisiblePawn>(Controller->GetPawn()):nullptr;
    if(!Pawn) return false;
    FBox Bounds(ForceInit);
    auto* V=Visualization();
    if(V && V->GetVisualizationMode()==EGCSVisualizationMode::Live)
    {
        const auto IDs=V->GetParticleDroneIDs(); const auto Positions=V->GetDronePositions();
        for(int32 I=0;I<FMath::Min(IDs.Num(),Positions.Num());++I)
        {
            FDroneData D;
            if((CheckedIDs.IsEmpty() || CheckedIDs.Contains(IDs[I])) && Fleet()->GetDroneByID(IDs[I],D) && D.bOnline && DroneGPS::IsValid(D)) Bounds+=Positions[I];
        }
    }
    FVector O,E,N,U;
    if(Bounds.IsValid && SceneFrame(O,E,N,U)) Pawn->FitMapBounds(Bounds.ExpandBy(500.f),E,U);
    else Pawn->ResetOrbitView();
    return true;
}
bool FGCSWorkbenchModel::Pick(FVector2D Point,float Radius)
{
    auto* V=Visualization(); if(!V || !Controller.IsValid() || V->GetVisualizationMode()!=EGCSVisualizationMode::Live) return false;
    const auto IDs=V->GetParticleDroneIDs(); const auto Positions=V->GetDronePositions();
    FString Hit; double Best=Radius*Radius;
    for(int32 I=0;I<FMath::Min(IDs.Num(),Positions.Num());++I)
    {
        FDroneData D; FVector2D Screen;
        if(!Fleet()->GetDroneByID(IDs[I],D) || !D.bOnline || !DroneGPS::IsValid(D) || !Controller->ProjectWorldLocationToScreen(Positions[I],Screen,true)) continue;
        const double Dist=FVector2D::DistSquared(Screen,Point);
        if(Dist<Best) { Best=Dist; Hit=IDs[I]; }
    }
    if(Hit.IsEmpty()) return false;
    Inspect(Hit); return true;
}
