#pragma once
#include "Widgets/SLeafWidget.h"
#include "GCSLoginPresentation.h"
#include "Brushes/SlateRoundedBoxBrush.h"

class UGCSLoginMedia;
class SGCSLoginShow final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SGCSLoginShow) {} 
        SLATE_ARGUMENT(TSharedPtr<FGCSLoginPresentation>, State)
        SLATE_ARGUMENT(UGCSLoginMedia*, Media)
        SLATE_ARGUMENT(const FSlateBrush*, Background)
        SLATE_ARGUMENT(const TArray<FSlateBrush>*, Themes)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args) { State=Args._State; Media=Args._Media; Background=Args._Background; Themes=Args._Themes; SetClipping(EWidgetClipping::ClipToBoundsAlways); }
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(960, 840); }
    virtual void Tick(const FGeometry& G, double T, float D) override;
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&, int32, const FWidgetStyle&, bool) const override;
    virtual FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override;
    virtual FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E) override;
    virtual void OnMouseCaptureLost(const FCaptureLostEvent&) override { Dragging=false; }
private:
    TSharedPtr<FGCSLoginPresentation> State;
    TWeakObjectPtr<UGCSLoginMedia> Media;
    const FSlateBrush* Background = nullptr;
    const TArray<FSlateBrush>* Themes=nullptr;
    FSlateRoundedBoxBrush Dot{FLinearColor::White};
    FVector2D DragStart;
    bool Dragging=false;
    double LastPaintTime=-1;
    int32 LastLanguage=-1;
    bool LastPaused=false;
};
