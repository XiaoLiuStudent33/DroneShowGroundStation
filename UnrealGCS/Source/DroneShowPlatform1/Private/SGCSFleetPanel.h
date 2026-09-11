#pragma once
#include "Widgets/SCompoundWidget.h"
#include "GCSWorkbenchModel.h"
#include "Widgets/Views/SListView.h"

class SGCSFleetPanel final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SGCSFleetPanel) {} SLATE_ARGUMENT(TSharedPtr<FGCSWorkbenchModel>, Model) SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    virtual void Tick(const FGeometry&,double,float) override;
    void SetSearchForTest(const FString& Search);
private:
    TSharedRef<ITableRow> GenerateRow(TSharedPtr<FDroneData> Row,const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<FGCSWorkbenchModel> Model;
    TSharedPtr<SListView<TSharedPtr<FDroneData>>> List;
    TSharedPtr<class SEditableTextBox> SearchBox;
    FButtonStyle RowStyle;
    FSlateBrush CardBrush;
    FEditableTextBoxStyle SearchStyle;
    int32 Generation=-1;
    FString LastDetailID;
};
