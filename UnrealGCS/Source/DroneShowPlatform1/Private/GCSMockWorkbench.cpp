#include "GCSMockWorkbench.h"
#include "GCSMockWorkbenchLayout.h"
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "UnrealClient.h"
#include "UnrealEngine.h"
#include "GameFramework/PlayerController.h"
#include "GCSInvisiblePawn.h"
#include "GCSWorkbenchModel.h"
#include "SGCSFleetPanel.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SDPIScaler.h"
#include "DroneFleetVisualizationActor.h"
#include "FleetManagerSubsystem.h"
#include "GCSServerAdapterSubsystem.h"
#include "DSPGCSProductionTelemetryProviderSubsystem.h"
#include "Engine/GameInstance.h"

namespace
{
FLinearColor C(uint32 Hex)
{ return FLinearColor::FromSRGBColor(FColor((Hex>>16)&255,(Hex>>8)&255,Hex&255)); }
const FLinearColor Ink=C(0xd6e0ec), Muted=C(0x8e9fb2), WorkbenchBlue=C(0x66adfa);
const FLinearColor ChromeColor=C(0x15202d), WorkbenchPanel=C(0x1a2736), Edge=C(0x3a526a);

class SMockWorkbench final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMockWorkbench) {}
        SLATE_ARGUMENT(TWeakObjectPtr<APlayerController>, ViewController)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        ViewController=Args._ViewController;
        Model=MakeShared<FGCSWorkbenchModel>(ViewController.Get());
        ChildSlot[SNew(SConstraintCanvas)
          +SConstraintCanvas::Slot().Alignment(FVector2D::ZeroVector).Offset_Lambda([this]{return FMargin((20+L)*Layout.Scale,136*Layout.Scale,328*Layout.Scale,(734+Layout.ExtraHeight)*Layout.Scale);})
          [SNew(SDPIScaler).DPIScale_Lambda([this]{return Layout.Scale;})
           .Visibility_Lambda([this]{return LeftTab==0 && L>-354.f?EVisibility::Visible:EVisibility::Collapsed;})
           [SAssignNew(FleetPanel,SGCSFleetPanel).Model(Model)]]];
        SetCanTick(true);
        #if WITH_DEV_AUTOMATION_TESTS
        bCapture=FParse::Param(FCommandLine::Get(),TEXT("GCSMockCapture"));
#endif
        Started=FPlatformTime::Seconds();
    }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(1672,940); }
    virtual ~SMockWorkbench() override
    {
        if(ViewController.IsValid()) if(auto* Pawn=Cast<AGCSInvisiblePawn>(ViewController->GetPawn())) Pawn->SetWorkbenchInputBlocked(false);
    }
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual void Tick(const FGeometry& G,double Time,float Delta) override
    {
        SCompoundWidget::Tick(G,Time,Delta);
        Layout=FGCSMockWorkbenchLayout::FromViewport(G.GetLocalSize());
        L=FMath::FInterpConstantTo(L,bLeft?0.f:-355.f,Delta,1775.f);
        R=FMath::FInterpConstantTo(R,bRight?0.f:360.f,Delta,1800.f);
        P=FMath::FInterpConstantTo(P,bPlayback?1.f:0.f,Delta,5.f);
        if(Time-LastDataRefresh>.1) { Model->Refresh(); LastDataRefresh=Time; }
        if(ViewController.IsValid()) if(auto* Pawn=Cast<AGCSInvisiblePawn>(ViewController->GetPawn()))
        {
            const FVector2D Point=Layout.ToDesign(G.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos()));
            Pawn->SetWorkbenchInputBlocked(bCapture || IsBlocking(Point.X,Point.Y));
        }
        if (bCapture)
        {
            const double T=FPlatformTime::Seconds()-Started;
            const FString Dir=FPaths::ProjectSavedDir()/TEXT("Automation/MockWorkbench");
            IFileManager::Get().MakeDirectory(*Dir,true);
            #if WITH_DEV_AUTOMATION_TESTS
            if(FParse::Param(FCommandLine::Get(),TEXT("GCSWorkbenchAcceptance")))
            { TickLiveAcceptance(T,Dir); Invalidate(EInvalidateWidgetReason::Paint); return; }
            #endif
            if(Stage==0 && T>8) { FScreenshotRequest::RequestScreenshot(Dir/TEXT("01-overview.png"),true,false); ++Stage; }
            else if(Stage==1 && T>10)
            {
                Activate(430,92); Activate(350,92); Activate(1380+Layout.ExtraWidth,92);
                if(FParse::Param(FCommandLine::Get(),TEXT("GCSMockAxes")) && ViewController.IsValid())
                    if(auto* Pawn=Cast<AGCSInvisiblePawn>(ViewController->GetPawn())) Pawn->RotateOrbit(65.f,20.f);
                ++Stage;
            }
            else if(Stage==2 && T>12) { FScreenshotRequest::RequestScreenshot(Dir/TEXT("02-tabs.png"),true,false); ++Stage; }
            else if(Stage==3 && T>14) { Activate(372,435+Layout.ExtraHeight*.5f); Activate(1298+Layout.ExtraWidth,435+Layout.ExtraHeight*.5f); Activate(540,92); ++Stage; }
            else if(Stage==4 && T>16) { FScreenshotRequest::RequestScreenshot(Dir/TEXT("03-collapsed-playback.png"),true,false); ++Stage; }
            else if(Stage==5 && T>18)
            {
                const bool Passed=!bLeft && !bRight && bPlayback && LeftTab==2 && RightTab==0;
                FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s: left=%d right=%d playback=%d leftTab=%d rightTab=%d"),Passed?TEXT("PASS"):TEXT("FAIL"),bLeft,bRight,bPlayback,LeftTab,RightTab),*(Dir/TEXT("result.txt")));
                if(FParse::Param(FCommandLine::Get(),TEXT("GCSMockResize")))
                {
                    bLeft=bRight=true; bPlayback=false; LeftTab=0; RightTab=1;
                    FSystemResolution::RequestResolutionChange(1600,720,EWindowMode::Windowed);
                }
                else FPlatformMisc::RequestExit(false);
                ++Stage;
            }
            else if(Stage==6 && T>22) { FScreenshotRequest::RequestScreenshot(Dir/TEXT("04-resized-wide.png"),true,false); ++Stage; }
            else if(Stage==7 && T>24)
            {
                Activate(1380+Layout.ExtraWidth,92);
                Activate(372,435+Layout.ExtraHeight*.5f);
                Activate(1298+Layout.ExtraWidth,435+Layout.ExtraHeight*.5f);
                // Use the bottom handle after resize, rather than the top shortcut.
                Activate((381+L+1288+R+Layout.ExtraWidth)*.5f,875+Layout.ExtraHeight);
                ++Stage;
            }
            else if(Stage==8 && T>26) { FScreenshotRequest::RequestScreenshot(Dir/TEXT("05-resized-drawers.png"),true,false); ++Stage; }
            else if(Stage==9 && T>28)
            {
                const bool Passed=!bLeft && !bRight && bPlayback && RightTab==0 && Layout.Width / Layout.Height > 2.f;
                FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s: resize left=%d right=%d playback=%d rightTab=%d extraWidth=%.2f extraHeight=%.2f"),Passed?TEXT("PASS"):TEXT("FAIL"),bLeft,bRight,bPlayback,RightTab,Layout.ExtraWidth,Layout.ExtraHeight),*(Dir/TEXT("resize-result.txt")));
                FPlatformMisc::RequestExit(false); ++Stage;
            }
        }
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override
    {
        if(bCapture) return FReply::Handled(); // Keep timed capture checks isolated from external clicks.
        if(E.GetEffectingButton()!=EKeys::LeftMouseButton) return FReply::Unhandled();
        Layout=FGCSMockWorkbenchLayout::FromViewport(G.GetLocalSize());
        const auto V=Layout.ToDesign(G.AbsoluteToLocal(E.GetScreenSpacePosition()));
        if(!bCapture && !IsBlocking(V.X,V.Y) && ViewController.IsValid())
        {
            int32 W,H; ViewController->GetViewportSize(W,H);
            const FVector2D Point=G.AbsoluteToLocal(E.GetScreenSpacePosition())/G.GetLocalSize()*FVector2D(W,H);
            if(Model->Pick(Point,12.f)) return FReply::Handled();
        }
        return Activate(V.X,V.Y)?FReply::Handled():FReply::Unhandled();
    }
    virtual FReply OnMouseMove(const FGeometry& G,const FPointerEvent& E) override
    { Hover=DesignPoint(G,E.GetScreenSpacePosition()); return FReply::Unhandled(); }
    virtual void OnMouseLeave(const FPointerEvent& E) override { Hover=FVector2D(-1,-1); SCompoundWidget::OnMouseLeave(E); }
    virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override
    { if(bCapture) return FReply::Handled(); if(E.GetKey()==EKeys::Escape) {bPlayback=false;return FReply::Handled();} return FReply::Unhandled(); }

    virtual int32 OnPaint(const FPaintArgs& Args,const FGeometry& G,const FSlateRect& Clip,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle& Style,bool Enabled) const override
    {
        const auto View=FGCSMockWorkbenchLayout::FromViewport(G.GetLocalSize());
        const float EW=View.ExtraWidth, EH=View.ExtraHeight;
        const FGeometry D=G.MakeChild(FVector2D(View.Width,View.Height),FSlateLayoutTransform(View.Scale));
        auto Rect=[&](float X,float Y,float W,float H,FLinearColor Fill,float Radius=7.f,FLinearColor Border=Edge)
        {
            FSlateRoundedBoxBrush Brush(Fill,Radius,Border,1.f);
            FSlateDrawElement::MakeBox(Out,Layer+1,D.ToPaintGeometry(FVector2D(W,H),FSlateLayoutTransform(FVector2D(X,Y))),&Brush,ESlateDrawEffect::None,Fill);
        };
        auto Text=[&](float X,float Y,const FString& S,int Size=14,FLinearColor Color=Ink,bool Bold=false)
        {
            FSlateDrawElement::MakeText(Out,Layer+3,D.ToPaintGeometry(FVector2D(600,40),FSlateLayoutTransform(FVector2D(X,Y))),S,FCoreStyle::GetDefaultFontStyle(Bold?"Bold":"Regular",Size),ESlateDrawEffect::None,Color);
        };
        auto Line=[&](float X,float Y,float X2,float Y2,FLinearColor Color=Edge,float Width=1.f)
        { TArray<FVector2D> Points={FVector2D(X,Y),FVector2D(X2,Y2)};FSlateDrawElement::MakeLines(Out,Layer+2,D.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Width); };
        auto Button=[&](float X,float Y,float W,float H,const FString& S,bool Active=false,bool Dim=false)
        {
            const bool Over=Hover.X>=X && Hover.X<=X+W && Hover.Y>=Y && Hover.Y<=Y+H;
            Rect(X,Y,W,H,Active?C(0x203b57):Over?C(0x2b4158):C(0x202f40),6,Active||Over?WorkbenchBlue:Edge);
            const auto Font=FCoreStyle::GetDefaultFontStyle("Regular",14);
            const auto Size=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(S,Font);
            Text(X+(W-Size.X)*.5,Y+(H-Size.Y)*.5,S,14,Dim?Muted:Active?WorkbenchBlue:Ink);
            if(Active) Line(X+6,Y+H-2,X+W-6,Y+H-2,WorkbenchBlue,2);
        };
        FLinearColor SceneTint=C(0x102338);SceneTint.A=.25f;Rect(0,54,View.Width,836+EH,SceneTint,0,SceneTint);
        if(ViewController.IsValid())
        {
            int32 W,H; ViewController->GetViewportSize(W,H);
            auto Project=[&](FVector World,FVector2D& Result)
            {
                FVector2D Pixel;
                if(W<=0 || H<=0 || !ViewController->ProjectWorldLocationToScreen(World,Pixel,true))return false;
                Result=Pixel/FVector2D(W,H)*FVector2D(View.Width,View.Height); return true;
            };
            Out.PushClip(FSlateClippingZone(D.ToPaintGeometry(FVector2D(907+R-L+EW,804+EH-P*112),FSlateLayoutTransform(FVector2D(381+L,57)))));
            FVector O,E,N,U;
            if((bGrid || bWorldAxes) && Model->SceneFrame(O,E,N,U))
            {
                auto WorldLine=[&](FVector A,FVector B,FLinearColor Color,float Thickness)
                {
                    FVector2D PA,PB;
                    if(Project(A,PA)&&Project(B,PB))
                    { TArray<FVector2D> Points={PA,PB};FSlateDrawElement::MakeLines(Out,Layer,D.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,Thickness); }
                };
                // Display-only geographic tangent grid: 10 metre spacing, 400 metre extent.
                if(bGrid) for(int32 I=-20;I<=20;++I)
                {
                    WorldLine(O+E*(I*1000.f)-N*20000.f,O+E*(I*1000.f)+N*20000.f,C(0x394d5c),1.f);
                    WorldLine(O+N*(I*1000.f)-E*20000.f,O+N*(I*1000.f)+E*20000.f,C(0x394d5c),1.f);
                }
                if(bWorldAxes)
                {
                    WorldLine(O-FVector::XAxisVector*20000,O+FVector::XAxisVector*20000,C(0xf15b64),2.f);
                    WorldLine(O-FVector::YAxisVector*20000,O+FVector::YAxisVector*20000,C(0x59c875),2.f);
                }
            }
            // Detail focus is independent of the existing multi-aircraft highlight mask.
            if(auto* V=Model->Visualization()) if(!Model->DetailID().IsEmpty() && V->GetVisualizationMode()==EGCSVisualizationMode::Live)
            {
                const auto IDs=V->GetParticleDroneIDs(); const int32 I=IDs.IndexOfByKey(Model->DetailID());
                const auto Positions=V->GetDronePositions(); FVector2D Point=FVector2D::ZeroVector; FDroneData Data;
                if(Positions.IsValidIndex(I) && Model->Fleet()->GetDroneByID(Model->DetailID(),Data) && DroneGPS::IsValid(Data) && Project(Positions[I],Point))
                {
                    Line(Point.X-12,Point.Y-12,Point.X+12,Point.Y-12,WorkbenchBlue,2);Line(Point.X-12,Point.Y+12,Point.X+12,Point.Y+12,WorkbenchBlue,2);
                    Line(Point.X-12,Point.Y-12,Point.X-12,Point.Y+12,WorkbenchBlue,2);Line(Point.X+12,Point.Y-12,Point.X+12,Point.Y+12,WorkbenchBlue,2);
                }
            }
            Out.PopClip();
        }
        // Opaque chrome surrounds the live UE viewport. No scene texture is baked into UI.
        Rect(0,0,View.Width,54,ChromeColor,0); Text(23,12,TEXT("DRONE"),20,Ink,true);Text(113,12,TEXT("GCS"),20,WorkbenchBlue,true);
        Line(186,16,186,38);Text(206,17,TEXT("无人机表演地面站"),17);Text(806+EW*.5f,17,TEXT("工作台"),18);
        Rect(1495+EW,22,12,12,Model->Connected()?C(0x59c875):C(0xf15e65),6,Model->Connected()?C(0x59c875):C(0xf15e65));Text(1514+EW,17,Model->Connected()?TEXT("已连接"):TEXT("未连接"),14);Line(1590+EW,16,1590+EW,38);Rect(1624+EW,19,18,18,ChromeColor,9,Muted);Rect(1630+EW,25,6,6,ChromeColor,3,Muted);for(int I=0;I<8;++I){float A=I*PI/4;Line(1633+EW+FMath::Cos(A)*10,28+FMath::Sin(A)*10,1633+EW+FMath::Cos(A)*14,28+FMath::Sin(A)*14,Muted,2);}
        Rect(0,890+EH,View.Width,50,ChromeColor,0);Rect(23,910+EH,12,12,Model->Connected()?C(0x59c875):C(0xf15e65),6,Model->Connected()?C(0x59c875):C(0xf15e65));Text(48,903+EH,Model->LinkText(),13,Muted);
        Text(1392+EW,903+EH,TEXT("机群 / 视图已接入 · 飞行控制待接入"),12,Muted);

        const float LX=6+L, RX=1306+R+EW;
        // Clip sliding drawer bodies; handles remain accessible at either screen edge.
        Out.PushClip(FSlateClippingZone(D.ToPaintGeometry(FVector2D(View.Width,834+EH),FSlateLayoutTransform(FVector2D(0,55)))));
        Rect(LX,57,355,830+EH,WorkbenchPanel,10);
        Rect(LX+10,69,335,46,ChromeColor,7);
        const TCHAR* LeftNames[]={TEXT("机群"),TEXT("日志"),TEXT("筛选")};
        for(int I=0;I<3;++I) Button(LX+10+I*112,69,111,46,LeftNames[I],LeftTab==I);
        if(LeftTab==0) { /* Live virtualized fleet panel is drawn by ChildSlot. */ }
        else if(LeftTab==1)
        {
            Text(LX+14,136,TEXT("运行日志"),18,Ink,true);Button(LX+14,181,104,34,TEXT("全部"),true);Button(LX+126,181,104,34,TEXT("告警"));
            Rect(LX+14,231,328,597+EH,C(0x13212e));Text(LX+28,251,TEXT("暂无日志记录"),14,Muted);Text(LX+14,850+EH,TEXT("仅布局演示，不读取运行日志"),12,Muted);
        }
        else
        {
            Text(LX+14,136,TEXT("遥测筛选"),18,Ink,true);
            const TCHAR* Names[]={TEXT("RTK 定位"),TEXT("剩余电量"),TEXT("飞行模式"),TEXT("相对高度")};
            for(int I=0;I<4;++I)Button(LX+14,183+I*53,328,42,Names[I],I==0);
            Text(LX+14,429,TEXT("筛选条件"),15,Ink,true);Rect(LX+14,465,328,47,ChromeColor);Text(LX+28,478,TEXT("RTK 定位              等于     固定解"),13,Muted);
            Text(LX+14,850+EH,TEXT("筛选控件仅作样式展示"),12,Muted);
        }
        Rect(RX,57,360,830+EH,WorkbenchPanel,10);Rect(RX+14,69,332,46,ChromeColor);
        const TCHAR* RightNames[]={TEXT("视图"),TEXT("控制"),TEXT("参数")};
        for(int I=0;I<3;++I)Button(RX+14+I*111,69,110,46,RightNames[I],RightTab==I);
        if(RightTab==1)
        {
            Text(RX+20,136,TEXT("集群控制"),19,Ink,true);Text(RX+20,174,FString::Printf(TEXT("当前已选 %d 架"),Model->SelectedCount()),14,Muted);
            Text(RX+20,219,TEXT("基础操作"),15,Ink,true);Text(RX+20,263,TEXT("悬停高度"),14,Muted);
            Rect(RX+146,249,198,43,ChromeColor);Text(RX+160,259,TEXT("5.0"),17);Text(RX+319,260,TEXT("m"),15,Muted);
            const TCHAR* Cmds[]={TEXT("解锁"),TEXT("起飞"),TEXT("悬停"),TEXT("降落"),TEXT("返航"),TEXT("加锁")};
            for(int I=0;I<6;++I)Button(RX+20+(I%2)*168,306+(I/2)*60,156,48,Cmds[I],false,true);
            Line(RX+20,503,RX+344,503);Text(RX+20,524,TEXT("灯光控制"),15,Ink,true);
            const uint32 Colors[]={0xf3f7fc,0xfb4b51,0xff9736,0xffe538,0x4fd15c,0x3f82fa,0xb34fed};
            for(int I=0;I<7;++I)Rect(RX+25+I*47,562,28,28,C(Colors[I]),14,C(Colors[I]));
            Line(RX+20,614,RX+344,614);Text(RX+20,643,bSimulation?TEXT("v  集群仿真"):TEXT(">  集群仿真"),17,Ink,true);
            if(bSimulation)
            {
                Text(RX+20,694,TEXT("无人机数量"),14,Muted);Rect(RX+154,679,190,43,ChromeColor);Text(RX+169,690,TEXT("100"),17);Text(RX+325,690,TEXT("^"),14,Muted);
                Text(RX+20,745,TEXT("编队间距"),14,Muted);Rect(RX+154,731,190,43,ChromeColor);Text(RX+169,743,TEXT("0.5"),17);Text(RX+319,743,TEXT("m"),15,Muted);
                Button(RX+20,785,324,46,TEXT("开始仿真"),false,true);
            }
        }
        else if(RightTab==0)
        {
            Text(RX+20,136,TEXT("视图设置"),19,Ink,true);Text(RX+20,181,TEXT("投影方式"),14,Muted);
            const auto* Pawn=ViewController.IsValid()?Cast<AGCSInvisiblePawn>(ViewController->GetPawn()):nullptr;
            const bool Ortho=Pawn && Pawn->IsOrthographicView();
            Button(RX+20,218,156,44,TEXT("透视"),!Ortho);Button(RX+188,218,156,44,TEXT("正交"),Ortho);
            Text(RX+20,306,TEXT("显示选项"),15,Ink,true);Button(RX+20,347,324,45,TEXT("地面坐标轴 XY"),bWorldAxes);Button(RX+20,404,324,45,TEXT("地面网格"),bGrid);
            Button(RX+20,461,324,45,TEXT("角落方向指示器"),bOrientation);
            Text(RX+20,850+EH,TEXT("中键旋转 · Shift+中键平移 · 滚轮缩放"),12,Muted);
        }
        else
        {
            Text(RX+20,136,TEXT("参数设置"),19,Ink,true);Text(RX+20,185,TEXT("连接配置"),15,Ink,true);
            Text(RX+20,234,TEXT("服务器地址"),14,Muted);Rect(RX+20,270,324,45,ChromeColor);Text(RX+34,282,TEXT("127.0.0.1"),15);
            Text(RX+20,345,TEXT("UDP 端口"),14,Muted);Rect(RX+20,382,324,45,ChromeColor);Text(RX+34,394,TEXT("10001"),15);
            Text(RX+20,850+EH,TEXT("参数仅作展示，不保存配置"),12,Muted);
        }
        Out.PopClip();
        Button(LX+355,408+EH*.5f,23,56,bLeft?TEXT("‹"):TEXT("›"));Button(RX-23,408+EH*.5f,23,56,bRight?TEXT("›"):TEXT("‹"));
        const float ToolX=381+L;
        Button(ToolX,69,98,46,TEXT("    定位"));Rect(ToolX+19,84,14,14,ChromeColor,7,WorkbenchBlue);Line(ToolX+26,80,ToolX+26,102,WorkbenchBlue);Line(ToolX+15,91,ToolX+37,91,WorkbenchBlue);Button(ToolX+113,69,100,46,TEXT("▶  播放"),bPlayback);
        const float MidLeft=381+L,MidRight=1288+R+EW,MidWidth=MidRight-MidLeft;
        // Screen-space world orientation, independent of map scale and camera distance.
        // Read the actual view rather than control rotation: the orbit camera has its own rotation.
        if(bOrientation && ViewController.IsValid())
        {
            FVector CameraLocation;
            FRotator CameraRotation;
            ViewController->GetPlayerViewPoint(CameraLocation,CameraRotation);
            const FVector2D Origin(MidRight-58.f,813.f+EH-P*112.f);
            const FVector Axes[]={FVector::XAxisVector,FVector::YAxisVector,FVector::ZAxisVector};
            const FLinearColor Colors[]={C(0xf15b64),C(0x59c875),C(0x5a9eff)};
            const TCHAR* Labels[]={TEXT("X"),TEXT("Y"),TEXT("Z")};
            FVector Local[3];
            TArray<int32> Order={0,1,2};
            for(int32 I=0;I<3;++I) Local[I]=CameraRotation.UnrotateVector(Axes[I]);
            // Camera-local X points away from the viewer. Draw farther axes first.
            Order.Sort([&](int32 A,int32 B){return Local[A].X>Local[B].X;});
            for(int32 I:Order)
            {
                const FVector2D Projected(Local[I].Y,-Local[I].Z);
                const FVector2D End=Origin+Projected*36.f;
                const FVector2D Direction=Projected.GetSafeNormal();
                const FVector2D Normal(-Direction.Y,Direction.X);
                const FLinearColor Color=Colors[I];
                Line(Origin.X,Origin.Y,End.X,End.Y,Color,2.f);
                if(Projected.Size()>.15f)
                {
                    const FVector2D A=End-Direction*7.f+Normal*3.f;
                    const FVector2D B=End-Direction*7.f-Normal*3.f;
                    Line(A.X,A.Y,End.X,End.Y,Color,2.f);
                    Line(B.X,B.Y,End.X,End.Y,Color,2.f);
                    Text(End.X+Direction.X*12.f-5.f,End.Y+Direction.Y*12.f-8.f,Labels[I],12,Color,true);
                }
                else
                {
                    // End-on axis: show a dot and offset label instead of a zero-length arrow.
                    Rect(Origin.X-3,Origin.Y-3,6,6,Color,3,Color);
                    Text(Origin.X+8,Origin.Y+6,Labels[I],12,Color,true);
                }
            }
        }
        if(P>0)
        {
            const float Y=888+EH-P*112;
            Rect(MidLeft,Y,MidWidth,112,WorkbenchPanel);Text(MidLeft+18,Y+12,TEXT("轨迹播放预览"),14,Ink,true);
            Button(MidLeft+18,Y+43,42,31,TEXT("▶"));Button(MidLeft+68,Y+43,42,31,TEXT("Ⅱ"));Text(MidLeft+128,Y+49,TEXT("00:00 / --:--"),13,Muted);
            Line(MidLeft+18,Y+91,MidRight-18,Y+91,Edge,3);Rect(MidLeft+18,Y+86,10,10,WorkbenchBlue,5,WorkbenchBlue);
        }
        Button((MidLeft+MidRight)*.5-83,862+EH-P*112,166,28,bPlayback?TEXT("v  收起播放"):TEXT("^  展开播放"));
        if(!Notice.IsEmpty()) {Rect(MidLeft+15,808+EH-P*112,FMath::Min(MidWidth-30,530.f),37,ChromeColor);Text(MidLeft+29,816+EH-P*112,Notice,12,WorkbenchBlue);}
        return SCompoundWidget::OnPaint(Args,G,Clip,Out,Layer+4,Style,Enabled);
    }
private:
#if WITH_DEV_AUTOMATION_TESTS
#include "GCSWorkbenchAcceptance.inl"
#endif
    bool IsBlocking(float X,float Y) const
    {
        const float EH=Layout.ExtraHeight,RS=R+Layout.ExtraWidth;
        return X<381+L || X>1283+RS || Y<57 || Y>862+EH-P*112
            || (Y<120 && X<594+L);
    }
    virtual FReply OnPreviewMouseButtonDown(const FGeometry&,const FPointerEvent&) override
    { return bCapture?FReply::Handled():FReply::Unhandled(); }
    static FVector2D DesignPoint(const FGeometry& G,FVector2D Screen)
    { return FGCSMockWorkbenchLayout::FromViewport(G.GetLocalSize()).ToDesign(G.AbsoluteToLocal(Screen)); }
    bool Activate(float X,float Y)
    {
        const float RS=R+Layout.ExtraWidth, EH=Layout.ExtraHeight;
        if(Y>=69 && Y<=115 && X>=381+L && X<=479+L) {Model->Locate();return true;}
        if(RightTab==0 && X>=1326+RS && X<=1650+RS)
        {
            if(Y>=218 && Y<=262)
            {
                if(ViewController.IsValid()) if(auto* Pawn=Cast<AGCSInvisiblePawn>(ViewController->GetPawn())) Pawn->SetOrthographicView(X>=1494+RS);
                return true;
            }
            if(Y>=347 && Y<=392){bWorldAxes=!bWorldAxes;return true;}
            if(Y>=404 && Y<=449){bGrid=!bGrid;return true;}
            if(Y>=461 && Y<=506){bOrientation=!bOrientation;return true;}
        }
        if(Y>=408+EH*.5f && Y<=464+EH*.5f && X>=361+L && X<=384+L) {bLeft=!bLeft;return true;}
        if(Y>=408+EH*.5f && Y<=464+EH*.5f && X>=1283+RS && X<=1306+RS) {bRight=!bRight;return true;}
        if(Y>=69 && Y<=115 && X>=16+L && X<=351+L) {LeftTab=FMath::Clamp(int((X-16-L)/112),0,2);return true;}
        if(Y>=69 && Y<=115 && X>=1320+RS && X<=1653+RS) {RightTab=FMath::Clamp(int((X-1320-RS)/111),0,2);return true;}
        if(Y>=69 && Y<=115 && X>=494+L && X<=594+L) {bPlayback=!bPlayback;return true;}
        const float Mid=(381+L+1288+RS)*.5;
        if(X>=Mid-83 && X<=Mid+83 && Y>=862+EH-P*112 && Y<=890+EH-P*112) {bPlayback=!bPlayback;return true;}
        if(X>=1326+RS && X<=1650+RS && Y>=634 && Y<=675 && RightTab==1) {bSimulation=!bSimulation;return true;}
        if((X>=1306+RS && Y>=55) || (X<=361+L && Y>=55) || (Y>=69 && Y<=115 && X>=381+L && X<=479+L))
        {Notice=TEXT("假 UI 演示：此控件尚未连接业务功能");return true;}
        return false;
    }
    bool bLeft=true,bRight=true,bPlayback=false,bSimulation=true,bCapture=false;
    int LeftTab=0,RightTab=1,Stage=0;
    float L=0,R=0,P=0;
    FGCSMockWorkbenchLayout Layout;
    double Started=0;
    FVector2D Hover=FVector2D(-1,-1);
    FString Notice;
    TSharedPtr<FGCSWorkbenchModel> Model;
    TSharedPtr<SGCSFleetPanel> FleetPanel;
    double LastDataRefresh=0;
    bool bWorldAxes=false,bGrid=false,bOrientation=true;
    TWeakObjectPtr<APlayerController> ViewController;
};
}
TSharedRef<SWidget> UGCSMockWorkbench::RebuildWidget() { return SNew(SMockWorkbench).ViewController(GetOwningPlayer()); }
