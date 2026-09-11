// Explicit, isolated development run only. Never inject data into an enabled production provider.
bool bLivePassed=true;
FString LiveChecks;
void TickLiveAcceptance(double T,const FString& Dir)
{
    auto* GI=ViewController->GetGameInstance();
    auto* Adapter=GI->GetSubsystem<UGCSServerAdapterSubsystem>();
    auto Check=[&](bool OK,const TCHAR* Name){bLivePassed&=OK;LiveChecks+=FString::Printf(TEXT("%s %s\n"),OK?TEXT("PASS"):TEXT("FAIL"),Name);};
    if(GI->GetSubsystem<UDSPGCSProductionTelemetryProviderSubsystem>()->IsEnabled())
    { FFileHelper::SaveStringToFile(TEXT("FAIL: disable production telemetry for isolated acceptance"),*(Dir/TEXT("live-result.txt")));FPlatformMisc::RequestExit(false);return; }
    if(Stage==0 && T>2)
    {
        FGCSFleetSnapshot S; S.Header.SourceID=TEXT("workbench-acceptance"); S.Header.SessionID=TEXT("acceptance-1"); S.FleetRevision=1;
        for(int32 I=0;I<1000;++I)
        { FDroneData D;D.DroneID=FString::Printf(TEXT("Drone%03d"),I);D.EndpointIdentity=FString::Printf(TEXT("127.0.0.1:%d"),20000+I);D.bOnline=I!=8 && I!=9;D.Mode=TEXT("GUIDED");D.Battery=98;D.bAltitudeValid=true;D.Altitude=5;
          D.bGPSValid=true;D.GPSFixType=6;D.GPSLat=31.932471+(I/32)*.00001;D.GPSLon=118.638531+(I%32)*.00001;D.AltitudeAMSL=30;S.Drones.Add(D); }
        Check(Adapter->PushFleetSnapshot(S),TEXT("adapter accepts snapshot")); ++Stage;
    }
    else if(Stage==1 && T>4)
    {
        Model->Refresh();Check(Model->Filtered.Num()==1000 && Model->OnlineCount==998,TEXT("1000 rows / 998 online"));
        FleetPanel->SetSearchForTest(TEXT("Drone00"));Check(Model->Filtered.Num()==10,TEXT("real search input filters ten rows"));
        Model->SelectFiltered(true);Check(Model->SelectedCount()==8,TEXT("filtered select-all excludes offline"));
        Model->Inspect(TEXT("Drone020"));Check(Model->SelectedCount()==8 && Model->DetailID()==TEXT("Drone020"),TEXT("detail does not change command selection"));
        FleetPanel->SetSearchForTest(TEXT(""));Check(Model->Filtered.Num()==1000 && Model->SelectedCount()==8,TEXT("clear search preserves selection"));
        Model->Locate(); ++Stage;
    }
    else if(Stage==2 && T>6)
    { FScreenshotRequest::RequestScreenshot(Dir/TEXT("live-01-fleet.png"),true,false);
      FGCSTelemetryBatch B;B.Header.SourceID=TEXT("workbench-acceptance");B.Header.SessionID=TEXT("acceptance-1");B.FleetRevision=2;
      FDroneDataDelta D;D.DroneID=TEXT("Drone000");D.ChangedFields=int32(EGCSDroneDataField::Battery)|int32(EGCSDroneDataField::Online);D.Data.Battery=42;D.Data.bOnline=false;B.Deltas.Add(D);
      Check(Adapter->PushTelemetryBatch(B),TEXT("adapter accepts delta"));++Stage; }
    else if(Stage==3 && T>8)
    {
        Model->Refresh();Check(Model->OnlineCount==997 && Model->SelectedCount()==7 && Model->Filtered[0]->Battery==42,TEXT("delta refresh and offline target pruning"));
        RightTab=0;Activate(1550+Layout.ExtraWidth,240);Activate(1440+Layout.ExtraWidth,370);Activate(1440+Layout.ExtraWidth,426);
        Check(Cast<AGCSInvisiblePawn>(ViewController->GetPawn())->IsOrthographicView() && bGrid && bWorldAxes,TEXT("view controls switch projection and overlays"));
        Model->Locate();++Stage;
    }
    else if(Stage==4 && T>10)
    {
        auto* V=Model->Visualization();Check(V && V->GetVisualizationDroneCount()==1000,TEXT("shared visualization contains fleet"));
        if(V)
        {const auto IDs=V->GetParticleDroneIDs();const auto Positions=V->GetDronePositions();const int32 I=IDs.IndexOfByKey(TEXT("Drone020"));FVector2D Pixel;
         if(Positions.IsValidIndex(I) && ViewController->ProjectWorldLocationToScreen(Positions[I],Pixel,true))Check(Model->Pick(Pixel,4) && Model->DetailID()==TEXT("Drone020"),TEXT("scene picking uses stable drone identity"));else Check(false,TEXT("scene projection"));}
        FScreenshotRequest::RequestScreenshot(Dir/TEXT("live-02-view.png"),true,false);++Stage;
    }
    else if(Stage==5 && T>12)
    { bLeft=bRight=false;bPlayback=true;++Stage; }
    else if(Stage==6 && T>14)
    { FScreenshotRequest::RequestScreenshot(Dir/TEXT("live-03-drawers.png"),true,false);++Stage; }
    else if(Stage==7 && T>16)
    { FGCSFleetSnapshot S;S.Header.SourceID=TEXT("workbench-acceptance");S.Header.SessionID=TEXT("acceptance-1");S.FleetRevision=3;Adapter->PushFleetSnapshot(S);bLeft=bRight=true;bPlayback=false; ++Stage; }
    else if(Stage==8 && T>18)
    {
        Model->Refresh();Check(Model->Filtered.IsEmpty() && Model->SelectedCount()==0 && Model->DetailID().IsEmpty(),TEXT("empty snapshot clears list selection and detail"));
        Check(Model->Visualization() && Model->Visualization()->GetVisualizationDroneCount()==0,TEXT("empty snapshot clears scene"));
        Activate(1370+Layout.ExtraWidth,240);Check(!Cast<AGCSInvisiblePawn>(ViewController->GetPawn())->IsOrthographicView(),TEXT("restore perspective"));
        Model->Locate();FScreenshotRequest::RequestScreenshot(Dir/TEXT("live-04-cleared.png"),true,false);++Stage;
    }
    else if(Stage==9 && T>20)
    { FFileHelper::SaveStringToFile((bLivePassed?TEXT("PASS\n"):TEXT("FAIL\n"))+LiveChecks,*(Dir/TEXT("live-result.txt")));FPlatformMisc::RequestExit(false);++Stage; }
}
