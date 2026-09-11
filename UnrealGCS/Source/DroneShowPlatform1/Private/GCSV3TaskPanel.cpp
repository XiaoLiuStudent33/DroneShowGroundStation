#include "GCSV3TaskPanel.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/WidgetSwitcher.h"
#include "Widgets/Input/SButton.h"

void UGCSV3TaskPanel::NativeConstruct()
{
    Super::NativeConstruct();
    for(int32 I=0; I<3; ++I)
        if(auto* B=Cast<UButton>(WidgetTree->FindWidget(*FString::Printf(TEXT("V3_Tab%d"),I))))
        {
            TWeakObjectPtr<UGCSV3TaskPanel> WeakThis(this);
            StaticCastSharedRef<SButton>(B->TakeWidget())->SetOnClicked(FOnClicked::CreateLambda([WeakThis,I]()
            {
                if(auto* Self=WeakThis.Get()) Self->SelectPage(I);
                return FReply::Handled();
            }));
        }
    SelectPage(0);
}
void UGCSV3TaskPanel::SelectPage(int32 Index)
{
    if(auto* Pages=Cast<UWidgetSwitcher>(WidgetTree->FindWidget(TEXT("V3_TaskPages")))) Pages->SetActiveWidgetIndex(Index);
    for(int32 I=0; I<3; ++I)
        if(auto* B=Cast<UButton>(WidgetTree->FindWidget(*FString::Printf(TEXT("V3_Tab%d"),I))))
        {
            auto Style=B->GetStyle();
            Style.Normal.TintColor=FSlateColor(FLinearColor::FromSRGBColor(I==Index ? FColor(53,84,133) : FColor(34,44,57)));
            B->SetStyle(Style);
        }
}
