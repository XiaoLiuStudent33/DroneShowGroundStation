#include "GCSV3EditorLibrary.h"
#if WITH_EDITOR
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanel.h"
#include "Components/Button.h"
#include "Components/Border.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/SpinBox.h"
#include "Components/ExpandableArea.h"
#include "Components/WidgetSwitcher.h"
#include "Components/VerticalBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "GCSV3TaskPanel.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#endif

bool UGCSV3EditorLibrary::BuildWorkbench()
{
#if WITH_EDITOR
    const TArray<FString> Names = { TEXT("TopBar"),TEXT("LeftToolbar"),TEXT("RightToolbar"),TEXT("DroneDetail"),TEXT("FleetPanel_V2"),TEXT("FleetRow"),TEXT("RightPanel"),TEXT("Playback"),TEXT("PageMap"),TEXT("PageLogs"),TEXT("TelemetryFilter"),TEXT("RouteImport"),TEXT("UploadDashboard"),TEXT("Main_V2") };
    const FLinearColor Background=FLinearColor::FromSRGBColor(FColor(23,29,37)),
        Surface=FLinearColor::FromSRGBColor(FColor(31,40,52)),
        Accent=FLinearColor::FromSRGBColor(FColor(108,154,236)),
        Ink=FLinearColor::FromSRGBColor(FColor(213,223,237));
    const FLinearColor Edge=FLinearColor::FromSRGBColor(FColor(51,65,82));
    for (const FString& Name : Names)
    {
        const FString Path = TEXT("/Game/UI_V3/WBP_GCS_") + Name;
        auto* BP = LoadObject<UWidgetBlueprint>(nullptr, *Path);
        if (!BP || !BP->WidgetTree) return false;
        BP->Modify();
        auto Place = [](UWidget* W, FVector2D P, FVector2D Size, FAnchors A=FAnchors(0,0), FVector2D Align=FVector2D::ZeroVector)
        {
            if (auto* S=Cast<UCanvasPanelSlot>(W ? W->Slot : nullptr)) { S->SetAnchors(A); S->SetAlignment(Align); S->SetPosition(P); S->SetSize(Size); }
        };
        if (Name==TEXT("Main_V2"))
        {
            // New instances use V3 classes; the original Widget Blueprints are never saved.
            auto* Root=Cast<UCanvasPanel>(BP->WidgetTree->RootWidget);
            if (!Root) return false;
            Root->ClearChildren();
            struct FChild { const TCHAR* Name; const TCHAR* Asset; FVector2D Pos, Size; FAnchors Anchors; FVector2D Align; };
            const FChild Children[] = {
                {TEXT("CHILD_TopBar"),TEXT("TopBar"),{0,0},{0,64},{0,0,1,0},{}},
                {TEXT("CHILD_LeftToolbar"),TEXT("LeftToolbar"),{402,76},{42,300},{0,0},{}},
                {TEXT("CHILD_RightToolbar"),TEXT("RightToolbar"),{-408,76},{42,300},{1,0},{1,0}},
                {TEXT("CHILD_FleetPanel"),TEXT("FleetPanel_V2"),{12,76},{382,38},{0,0,0,1},{}},
                {TEXT("CHILD_DroneDetail"),TEXT("DroneDetail"),{452,120},{248,500},{0,0},{}},
                {TEXT("CHILD_RightPanel"),TEXT("RightPanel"),{-12,76},{384,38},{1,0,1,1},{1,0}},
                {TEXT("CHILD_Playback"),TEXT("Playback"),{452,-132},{458,94},{0,1,1,1},{0,0}}
            };
            for (const auto& C:Children)
            {
                if (auto* Old=FindObject<UWidget>(BP->WidgetTree,C.Name)) Old->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors);
                FString ClassPath=FString::Printf(TEXT("/Game/UI_V3/WBP_GCS_%s.WBP_GCS_%s_C"),C.Asset,C.Asset);
                auto* Class=LoadClass<UUserWidget>(nullptr,*ClassPath);
                if (!Class) return false;
                auto* W=BP->WidgetTree->ConstructWidget<UUserWidget>(Class,C.Name);
                Root->AddChild(W); Place(W,C.Pos,C.Size,C.Anchors,C.Align);
            }
            if (auto* Old=FindObject<UWidget>(BP->WidgetTree,TEXT("SelectionRectangle"))) Old->Rename(nullptr,GetTransientPackage(),REN_DontCreateRedirectors);
            auto* Selection=BP->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(),TEXT("SelectionRectangle"));
            Root->AddChild(Selection); Selection->SetBrushColor(FLinearColor(.1f,.7f,.8f,.15f)); Selection->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (Name==TEXT("RightPanel") && !BP->WidgetTree->FindWidget(TEXT("V3_TaskPages")))
        {
            BP->ParentClass=UGCSV3TaskPanel::StaticClass();
            auto* Root=Cast<UCanvasPanel>(BP->WidgetTree->RootWidget);
            auto* Sections=Cast<UVerticalBox>(BP->WidgetTree->FindWidget(TEXT("BOX_RightSections")));
            if (!Root || !Sections) return false;
            TArray<UWidget*> Areas=Sections->GetAllChildren();
            Sections->ClearChildren();
            auto* Pages=BP->WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass(),TEXT("V3_TaskPages"));
            Sections->AddChild(Pages);
            UVerticalBox* Groups[3];
            for(int I=0;I<3;++I)
            {
                Groups[I]=BP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(),*FString::Printf(TEXT("V3_Group%d"),I));
                Pages->AddChild(Groups[I]);
                auto* B=BP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(),*FString::Printf(TEXT("V3_Tab%d"),I));
                auto* T=BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
                const TCHAR* Labels[]={TEXT("作业准备"),TEXT("演出控制"),TEXT("设备 / 场景")};
                T->SetText(FText::FromString(Labels[I])); auto Font=T->GetFont(); Font.Size=12; T->SetFont(Font); B->AddChild(T); Root->AddChild(B);
                Place(B,{float(12+I*118),50},{112,34});
            }
            for(auto* Area:Areas)
            {
                const FString N=Area->GetName();
                const int I=N==TEXT("AREA_GroundStation") ? 0 : N==TEXT("AREA_Console") ? 1 : 2;
                Groups[I]->AddChild(Area);
                if(auto* E=Cast<UExpandableArea>(Area)) E->SetIsExpanded(I<2);
            }
            auto* Title=BP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(),TEXT("V3_TaskTitle"));
            Title->SetText(FText::FromString(TEXT("飞行作业中心"))); auto TitleFont=Title->GetFont();TitleFont.Size=18;Title->SetFont(TitleFont); Root->AddChild(Title); Place(Title,{16,14},{320,28});
        }
        BP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            const FString N=W->GetName();
            if (auto* T=Cast<UTextBlock>(W))
            {
                T->SetColorAndOpacity(Ink);
                if(Name==TEXT("RightPanel")) { auto Font=T->GetFont();Font.Size=N.Contains(TEXT("Header"))?13:12; T->SetFont(Font); }
                if (N==TEXT("V3_TaskTitle") || (T->GetParent() && T->GetParent()->GetName().StartsWith(TEXT("V3_Tab"))))
                { auto Font=T->GetFont(); Font.Size=N==TEXT("V3_TaskTitle")?18:12; T->SetFont(Font); }
                if (N.Contains(TEXT("Header")) || N.Contains(TEXT("Title"))) T->SetColorAndOpacity(Accent);
            }
            if (auto* B=Cast<UBorder>(W)) { if (N!=TEXT("SelectionRectangle")) { B->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White,8.f,Edge,1.f)); B->SetBrushColor(Surface); B->SetPadding(FMargin(10)); } }
            if (Name==TEXT("RightPanel"))
                if(auto* Box=Cast<USizeBox>(W))
                    if(!N.Contains(TEXT("Color")) && Box->IsHeightOverride() && Box->GetHeightOverride()<30) Box->SetHeightOverride(30);
            if (auto* E=Cast<UEditableTextBox>(W))
            {
                auto Style=E->WidgetStyle;
                Style.BackgroundImageNormal.TintColor=FSlateColor(Background);
                Style.BackgroundImageHovered.TintColor=FSlateColor(Surface);
                Style.BackgroundImageFocused.TintColor=FSlateColor(Surface);
                Style.ForegroundColor=FSlateColor(Ink); E->WidgetStyle=Style;
            }
            if (auto* E=Cast<USpinBox>(W))
            {
                auto Style=E->GetWidgetStyle();
                Style.BackgroundBrush.TintColor=FSlateColor(Background);
                Style.HoveredBackgroundBrush.TintColor=FSlateColor(Surface);
                Style.ActiveFillBrush.TintColor=FSlateColor(Surface);
                Style.InactiveFillBrush.TintColor=FSlateColor(Surface);
                E->SetWidgetStyle(Style); E->SetForegroundColor(Ink);
            }
            if (auto* E=Cast<UExpandableArea>(W))
            {
                E->SetBorderColor(Edge);
            }
            if (auto* B=Cast<UButton>(W))
            {
                if (!N.StartsWith(TEXT("BTN_Color")))
                {
                    FButtonStyle Style=B->GetStyle();
                    Style.Normal.TintColor=FSlateColor(N.StartsWith(TEXT("PANEL_"))?Background:Surface);
                    Style.Hovered.TintColor=FSlateColor(FLinearColor(.055f,.16f,.21f,1));
                    Style.Pressed.TintColor=FSlateColor(FLinearColor(.035f,.30f,.34f,1));
                    Style.Normal.DrawAs=Style.Hovered.DrawAs=Style.Pressed.DrawAs=ESlateBrushDrawType::RoundedBox;
                    Style.Normal.OutlineSettings.CornerRadii=FVector4(6,6,6,6);
                    Style.Normal.OutlineSettings.Color=FSlateColor(Edge);
                    Style.Normal.OutlineSettings.Width=1;
                    Style.Hovered.OutlineSettings.Color=FSlateColor(Accent);
                    Style.Hovered.OutlineSettings.Width=1;
                    Style.Pressed.OutlineSettings.Color=FSlateColor(Accent);
                    Style.Pressed.OutlineSettings.Width=1;
                    B->SetBackgroundColor(FLinearColor::White);
                    Style.Hovered.OutlineSettings.CornerRadii=Style.Pressed.OutlineSettings.CornerRadii=Style.Normal.OutlineSettings.CornerRadii;
                    if(Name==TEXT("RightPanel")) { Style.NormalPadding=FMargin(8,6);Style.PressedPadding=FMargin(8,6); }
                    B->SetStyle(Style);
                }
                if (auto* S=Cast<UVerticalBoxSlot>(B->Slot)) S->SetPadding(FMargin(0,4));
            }
        });
        auto Find=[&](const TCHAR* N){return BP->WidgetTree->FindWidget(N);};
        if (Name==TEXT("Playback"))
        {
            Place(Find(TEXT("BORDER_Playback")),{}, {},FAnchors(0,0,1,1));
            Place(Find(TEXT("SLD_RouteTimeline")),{12,52},{12,20},FAnchors(0,0,1,0));
            Place(Find(TEXT("TXT_RouteTime")),{-12,12},{150,22},FAnchors(1,0),{1,0});
        }
        if (Name==TEXT("RightPanel"))
        {
            Place(Find(TEXT("PANEL_RightBackground")),{}, {}, FAnchors(0,0,1,1));
            if(auto* B=Cast<UBorder>(Find(TEXT("PANEL_RightBackground")))) B->SetPadding(FMargin(10,100,10,10));
        }
        if (Name==TEXT("TopBar"))
        {
            Place(Find(TEXT("PANEL_Top")),{},FVector2D(0,64),FAnchors(0,0,1,0));
            Place(Find(TEXT("Title")),{16,17},{170,26});
            Place(Find(TEXT("Subtitle")),{204,23},{260,18});
            Place(Find(TEXT("Status")),{-24,24},{440,20},FAnchors(1,0),{1,0});
            if(auto* T=Cast<UTextBlock>(Find(TEXT("Title")))) {T->SetText(FText::FromString(TEXT("DRONE GCS"))); auto F=T->GetFont();F.Size=19;T->SetFont(F);}
            if(auto* T=Cast<UTextBlock>(Find(TEXT("Subtitle")))) T->SetText(FText::FromString(TEXT("FLEET OPERATIONS")));
            // These were demonstration values, not actual show telemetry.
            if(auto* T=Cast<UTextBlock>(Find(TEXT("Status")))) T->SetText(FText::FromString(TEXT("连接状态待更新")));
            Find(TEXT("ShowName"))->SetVisibility(ESlateVisibility::Collapsed);
            Find(TEXT("Timeline"))->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (Name==TEXT("LeftToolbar"))
        {
            const TCHAR* Buttons[]={TEXT("BTN_Menu"),TEXT("BTN_Fleet"),TEXT("BTN_Control"),TEXT("BTN_Map"),TEXT("BTN_Settings"),TEXT("BTN_Logs")};
            for(int I=0;I<6;++I) Place(Find(Buttons[I]),{0,float(I*48)},{40,42});
            Find(TEXT("PANEL_Left"))->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (Name==TEXT("RightToolbar"))
        {
            const TCHAR* Buttons[]={TEXT("BTN_View"),TEXT("BTN_ZCamera"),TEXT("BTN_Device"),TEXT("BTN_ShaiXuan"),TEXT("BTN_Scene")};
            for(int I=0;I<5;++I) Place(Find(Buttons[I]),{0,float(I*52)},{40,46});
            Find(TEXT("PANEL_Right"))->SetVisibility(ESlateVisibility::Collapsed);
        }
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
        FKismetEditorUtilities::CompileBlueprint(BP);
        if (BP->Status==BS_Error) return false;
        FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
        if (!UPackage::SavePackage(BP->GetOutermost(),BP,*FPackageName::LongPackageNameToFilename(Path,FPackageName::GetAssetPackageExtension()),Args)) return false;
    }
    // The copied level still contains the legacy CreateWidget node. Route that
    // node to V3 as well so module bootstrap finds and binds the same instance.
    auto* World=LoadObject<UWorld>(nullptr,TEXT("/Game/Maps/L_GCS_Workbench_V3.L_GCS_Workbench_V3"));
    if (!World || !World->PersistentLevel) return false;
    if (auto* Script=World->PersistentLevel->GetLevelScriptBlueprint())
    {
        TArray<UEdGraph*> Graphs; Script->GetAllGraphs(Graphs);
        for(auto* Graph:Graphs) for(UEdGraphNode* Node:Graph->Nodes) for(auto* Pin:Node->Pins)
            if(Pin && Pin->DefaultObject && Pin->DefaultObject->GetPathName()==TEXT("/Game/UI/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"))
                Pin->DefaultObject=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI_V3/WBP_GCS_Main_V2.WBP_GCS_Main_V2_C"));
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Script);
        FKismetEditorUtilities::CompileBlueprint(Script);
        if(Script->Status==BS_Error) return false;
    }
    FSavePackageArgs MapArgs; MapArgs.TopLevelFlags=RF_Public|RF_Standalone; MapArgs.SaveFlags=SAVE_NoError;
    return UPackage::SavePackage(World->GetOutermost(),World,*FPackageName::LongPackageNameToFilename(TEXT("/Game/Maps/L_GCS_Workbench_V3"),FPackageName::GetMapPackageExtension()),MapArgs);
#else
    return false;
#endif
}

FString UGCSV3EditorLibrary::InspectWidgets(const FString& AssetPath)
{
    FString Result;
#if WITH_EDITOR
    if (auto* BP = LoadObject<UWidgetBlueprint>(nullptr, *AssetPath))
    {
        Result += FString::Printf(TEXT("PARENT %s\n"), *GetPathNameSafe(BP->ParentClass));
        BP->WidgetTree->ForEachWidget([&](UWidget* W)
        {
            Result += FString::Printf(TEXT("%s | %s | parent=%s"), *W->GetName(), *W->GetClass()->GetPathName(), *GetNameSafe(W->GetParent()));
            if (auto* S = Cast<UCanvasPanelSlot>(W->Slot))
                Result += FString::Printf(TEXT(" | pos=%s size=%s anchors=%s,%s"), *S->GetPosition().ToString(), *S->GetSize().ToString(), *S->GetAnchors().Minimum.ToString(), *S->GetAnchors().Maximum.ToString());
            if (auto* T = Cast<UTextBlock>(W)) Result += TEXT(" | text=") + T->GetText().ToString();
            Result += TEXT("\n");
        });
    }
#endif
    return Result;
}

