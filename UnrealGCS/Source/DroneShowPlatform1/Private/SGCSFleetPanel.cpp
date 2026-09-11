#include "SGCSFleetPanel.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Styling/CoreStyle.h"
#include "Brushes/SlateRoundedBoxBrush.h"

namespace
{
FLinearColor FC(uint32 H) { return FLinearColor::FromSRGBColor(FColor(H>>16,(H>>8)&255,H&255)); }
TSharedRef<STextBlock> Label(const FString& S,int32 Size=14)
{ return SNew(STextBlock).Text(FText::FromString(S)).Font(FCoreStyle::GetDefaultFontStyle("Regular",Size)).ColorAndOpacity(FC(0xd6e0ec)); }
}
void SGCSFleetPanel::Construct(const FArguments& Args)
{
    Model=Args._Model;
    CardBrush=FSlateRoundedBoxBrush(FC(0x1d3043),6.f,FC(0x3a526a),1.f);
    RowStyle=FCoreStyle::Get().GetWidgetStyle<FButtonStyle>("Button");
    RowStyle.SetNormal(FSlateRoundedBoxBrush(FC(0x172634),3.f));
    RowStyle.SetHovered(FSlateRoundedBoxBrush(FC(0x294057),3.f));
    RowStyle.SetPressed(FSlateRoundedBoxBrush(FC(0x345779),3.f));
    SearchStyle=FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>("NormalEditableTextBox");
    SearchStyle.SetBackgroundImageNormal(FSlateRoundedBoxBrush(FC(0x15202d),5.f,FC(0x3a526a),1.f));
    SearchStyle.SetBackgroundImageHovered(SearchStyle.BackgroundImageNormal);
    SearchStyle.SetBackgroundImageFocused(FSlateRoundedBoxBrush(FC(0x15202d),5.f,FC(0x66adfa),1.f));
    SearchStyle.SetForegroundColor(FC(0xd6e0ec));
    ChildSlot
    [ SNew(SVerticalBox)
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)[Label(TEXT("机群状态"),18)]
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,12)
      [SNew(SHorizontalBox)
       +SHorizontalBox::Slot().FillWidth(1).Padding(0,0,6,0)
       [SNew(SBorder).BorderImage(&CardBrush).Padding(12)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Label(TEXT("在线"),13)]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",24)).ColorAndOpacity(FC(0x66adfa)).Text_Lambda([this]{return FText::AsNumber(Model->OnlineCount);})]]]
       +SHorizontalBox::Slot().FillWidth(1).Padding(6,0,0,0)
       [SNew(SBorder).BorderImage(&CardBrush).Padding(12)[SNew(SVerticalBox)
        +SVerticalBox::Slot().AutoHeight()[Label(TEXT("已选"),13)]
        +SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",24)).ColorAndOpacity(FC(0xd6e0ec)).Text_Lambda([this]{return FText::AsNumber(Model->SelectedCount());})]]]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)
      [SNew(SBox).HeightOverride(36)[SAssignNew(SearchBox,SEditableTextBox).Style(&SearchStyle)
       .Font(FCoreStyle::GetDefaultFontStyle("Regular",14)).HintText(FText::FromString(TEXT("搜索无人机编号")))
       .OnTextChanged_Lambda([this](const FText& T){Model->SetSearch(T.ToString());})]]
      +SVerticalBox::Slot().AutoHeight().Padding(0,0,0,10)
      [SNew(SCheckBox).IsEnabled_Lambda([this]{return Model->CanSelect();})
       .IsChecked_Lambda([this]{int32 Eligible=0,Checked=0;for(const auto& R:Model->Filtered)if(R->bOnline){++Eligible;Checked+=Model->IsChecked(R->DroneID)?1:0;}
        return Checked==0?ECheckBoxState::Unchecked:(Checked==Eligible?ECheckBoxState::Checked:ECheckBoxState::Undetermined);})
       .OnCheckStateChanged_Lambda([this](ECheckBoxState S){Model->SelectFiltered(S==ECheckBoxState::Checked);})
       [Label(TEXT("全选当前结果中的在线飞机"),12)]]
      +SVerticalBox::Slot().AutoHeight().Padding(25,0,12,6)
      [SNew(SHorizontalBox)
       +SHorizontalBox::Slot().FillWidth(.32f)[Label(TEXT("编号"),11)]
       +SHorizontalBox::Slot().FillWidth(.25f)[Label(TEXT("模式"),11)]
       +SHorizontalBox::Slot().FillWidth(.16f)[Label(TEXT("电量"),11)]
       +SHorizontalBox::Slot().FillWidth(.18f)[Label(TEXT("高度"),11)]
       +SHorizontalBox::Slot().FillWidth(.09f)[Label(TEXT("状态"),10)]]
      +SVerticalBox::Slot().FillHeight(1.f)
      [SAssignNew(List,SListView<TSharedPtr<FDroneData>>).ListViewStyle(static_cast<const FTableViewStyle*>(nullptr)).ListItemsSource(&Model->Filtered)
       .SelectionMode(ESelectionMode::Single).OnGenerateRow(this,&SGCSFleetPanel::GenerateRow)
       .OnSelectionChanged_Lambda([this](TSharedPtr<FDroneData> Row,ESelectInfo::Type Reason){if(Row.IsValid() && Reason!=ESelectInfo::Direct) Model->Inspect(Row->DroneID);})]
      +SVerticalBox::Slot().AutoHeight().Padding(0,10,0,6)
      [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",12)).ColorAndOpacity(FC(0x8e9fb2))
       .Text_Lambda([this]{return FText::FromString(Model->Filtered.IsEmpty()?TEXT("暂无匹配飞机"):FString::Printf(TEXT("结果 %d 架 · 勾选与详情独立"),Model->Filtered.Num()));})]
      +SVerticalBox::Slot().AutoHeight()
      [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",12)).ColorAndOpacity(FC(0xd6e0ec)).AutoWrapText(true)
       .Text_Lambda([this]{return FText::FromString(Model->DetailText());})]
    ];
}
TSharedRef<ITableRow> SGCSFleetPanel::GenerateRow(TSharedPtr<FDroneData> Row,const TSharedRef<STableViewBase>& Owner)
{
    auto Cells=SNew(SHorizontalBox);
    const float Widths[]={.32f,.25f,.16f,.18f,.09f};
    for(int32 I=0;I<5;++I)
    Cells->AddSlot().FillWidth(Widths[I])
    [SNew(STextBlock).Font(FCoreStyle::GetDefaultFontStyle("Regular",11)).Clipping(EWidgetClipping::ClipToBounds)
     .ColorAndOpacity_Lambda([this,Row,I]{return I==4?(Row->bOnline?FC(0x59c875):FC(0xf15b64)):(Model->DetailID()==Row->DroneID?FC(0x66adfa):FC(0xd6e0ec));})
     .Text_Lambda([Row,I]{switch(I){case 0:return FText::FromString(Row->DroneID);case 1:return FText::FromString(Row->Mode);
       case 2:return FText::FromString(FString::Printf(TEXT("%.0f%%"),Row->Battery));case 3:return FText::FromString(Row->bAltitudeValid?FString::Printf(TEXT("%.1f"),Row->Altitude):TEXT("--"));
       default:return FText::FromString(Row->bOnline?TEXT("●"):TEXT("○"));}})];
    return SNew(STableRow<TSharedPtr<FDroneData>>,Owner).Padding(FMargin(0,2))
    [SNew(SHorizontalBox)
     +SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0,0,4,0)
     [SNew(SCheckBox).IsChecked_Lambda([this,Row]{return Model->IsChecked(Row->DroneID)?ECheckBoxState::Checked:ECheckBoxState::Unchecked;})
      .IsEnabled_Lambda([this,Row]{return Row->bOnline && Model->CanSelect();})
      .OnCheckStateChanged_Lambda([this,Row](ECheckBoxState S){Model->SetChecked(Row->DroneID,S==ECheckBoxState::Checked);})]
     +SHorizontalBox::Slot().FillWidth(1.f)
     [SNew(SButton).ButtonStyle(&RowStyle).ContentPadding(FMargin(4,3))
      .OnClicked_Lambda([this,Row]{Model->Inspect(Row->DroneID);return FReply::Handled();})
      .ToolTipText_Lambda([Row]{return FText::FromString(Row->DroneID+TEXT("\n")+Row->EndpointIdentity);})
      [Cells]]];
}
void SGCSFleetPanel::Tick(const FGeometry& G,double T,float D)
{
    SCompoundWidget::Tick(G,T,D);
    if(Generation!=Model->ListGeneration){Generation=Model->ListGeneration;List->RequestListRefresh();}
    const FString ID=Model->DetailID();
    if(ID!=LastDetailID)
    {
        LastDetailID=ID; List->ClearSelection();
        for(const auto& Row:Model->Filtered)if(Row->DroneID==ID){List->SetSelection(Row,ESelectInfo::Direct);List->RequestScrollIntoView(Row);break;}
    }
}
void SGCSFleetPanel::SetSearchForTest(const FString& S) { SearchBox->SetText(FText::FromString(S)); }
