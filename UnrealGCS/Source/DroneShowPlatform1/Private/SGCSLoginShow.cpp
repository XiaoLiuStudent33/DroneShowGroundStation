#include "SGCSLoginShow.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "InputCoreTypes.h"
#include "GCSLoginMedia.h"
#include "Fonts/FontMeasure.h"

void SGCSLoginShow::Tick(const FGeometry& G,double T,float D)
{
    SLeafWidget::Tick(G,T,D);
    const float OldTransition=State->Transition;
    const int32 OldTheme=State->Theme;
    State->Advance(D);
    const bool MediaChanged=Media.IsValid() && Media->Update(*State,D);
    // Cap presentation repaint to 30 Hz, with continuous repaint during transitions.
    const bool Animate=!State->Paused && T-LastPaintTime>=1.0/30.0;
    if(Animate)LastPaintTime=T;
    if(MediaChanged || Animate || OldTransition!=State->Transition || OldTheme!=State->Theme || LastLanguage!=State->Language || LastPaused!=State->Paused)
        Invalidate(EInvalidateWidgetReason::Paint);
    LastLanguage=State->Language;LastPaused=State->Paused;
}
int32 SGCSLoginShow::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool) const
{
    const FVector2D Size=G.GetLocalSize();
    const float S=FMath::Clamp(FMath::Min(Size.X/960.f,Size.Y/780.f),.5f,1.6f);
    const FLinearColor Ink(.72f,.83f,.97f),Muted(.36f,.49f,.68f);
    auto Box=[&](FVector2D P,FVector2D Extent,const FSlateBrush* Brush,FLinearColor Color,int32 L)
    {FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(Extent,FSlateLayoutTransform(P)),Brush,ESlateDrawEffect::None,Color);};
    auto Text=[&](const FString& V,FVector2D P,int32 FontSize,FLinearColor Color,int32 L,bool Bold=false)
    {FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(Size,FSlateLayoutTransform(P)),V,FCoreStyle::GetDefaultFontStyle(Bold?"Bold":"Regular",FMath::Max(9,FMath::RoundToInt(FontSize*S))),ESlateDrawEffect::None,Color);};
    auto Line=[&](TArray<FVector2D> Points,FLinearColor Color,float Width=1.2f)
    {FSlateDrawElement::MakeLines(Out,Layer+6,G.ToPaintGeometry(FVector2D(Size),FSlateLayoutTransform(FVector2D(Size.X*.5f-220*S,0))),Points,ESlateDrawEffect::None,Color,true,Width*S);};
    auto Scene=[&](int32 Theme,float Offset)
    {
        // Clip the complete slide, including its caption, to prevent cross-slide overlap.
        Out.PushClip(FSlateClippingZone(G.ToPaintGeometry(Size,FSlateLayoutTransform(FVector2D(Offset,0)))));
        if(auto* Show=Media.Get())
        {
            const float Width=FMath::Min(Size.X*.98f,Size.Y*.66f*1.6f);
            const FVector2D Extent(Width,Width/1.6f),Position(Offset+(Size.X-Width)*.5f,Size.Y*.385f-Extent.Y*.5f);
            const float Blend=Show->GetVideoBlend(Theme);
            if(const auto* Poster=Show->GetPoster(Theme))Box(Position,Extent,Poster,FLinearColor(1,1,1,1-Blend),Layer+1);
            if(Blend>0)Box(Position,Extent,Show->GetVideo(),FLinearColor(1,1,1,Blend),Layer+1);
        }
        auto CenterText=[&](const FString& Value,float Y,int32 Font,FLinearColor Color,bool Bold)
        {
            const auto Info=FCoreStyle::GetDefaultFontStyle(Bold?"Bold":"Regular",FMath::Max(9,FMath::RoundToInt(Font*S)));
            const FVector2D Extent=FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Value,Info);
            Text(Value,FVector2D(Offset+(Size.X-Extent.X)*.5f,Y),Font,Color,Layer+2,Bold);
        };
        CenterText(State->Title(Theme),Size.Y*.73f,29,Ink,false);
        CenterText(State->Caption(Theme),Size.Y*.80f,15,Muted,false);
        Out.PopClip();
    };
    Out.PushClip(FSlateClippingZone(G));
    const float E=State->Transition*State->Transition*(3-2*State->Transition);
    if(State->Transition<1.f){Scene(State->Previous,-State->Direction*E*Size.X);Scene(State->Theme,State->Direction*(1-E)*Size.X);}
    else Scene(State->Theme,0);
    // Fixed control row: 32 px circles, 40 px left/bottom in design units.
    const float Y=Size.Y*.88f;
    const float ControlOffset=Size.X*.5f-220*S;
    auto Circle=[&](float X)
    {
        TArray<FVector2D> P;
        for(int32 N=0;N<=32;++N){const float A=N*2*PI/32;P.Add(FVector2D((X+16*FMath::Cos(A))*S,Y+16*S*FMath::Sin(A)));}
        Line(P,Ink);
    };
    Circle(56);Circle(196);Circle(268);
    Line({FVector2D(59*S,Y-6*S),FVector2D(53*S,Y),FVector2D(59*S,Y+6*S)},Ink,1.8f);
    Line({FVector2D(193*S,Y-6*S),FVector2D(199*S,Y),FVector2D(193*S,Y+6*S)},Ink,1.8f);
    for(int32 N=0;N<3;++N)Box(FVector2D(ControlOffset+(108+N*22-3)*S,Y-3*S),FVector2D(6*S),&Dot,N==State->Theme?FLinearColor(.15f,.72f,1):Muted,Layer+6);
    Line({FVector2D(232*S,Y-13*S),FVector2D(232*S,Y+13*S)},Muted);
    if(State->Paused)Line({FVector2D(264*S,Y-6*S),FVector2D(274*S,Y),FVector2D(264*S,Y+6*S),FVector2D(264*S,Y-6*S)},Ink,1.7f);
    else {Line({FVector2D(264*S,Y-6*S),FVector2D(264*S,Y+6*S)},Ink,1.7f);Line({FVector2D(272*S,Y-6*S),FVector2D(272*S,Y+6*S)},Ink,1.7f);}
    Text(State->Paused?State->Text(TEXT("继续演示"),TEXT("繼續演示"),TEXT("Resume"),TEXT("再開")):State->Text(TEXT("暂停演示"),TEXT("暫停演示"),TEXT("Pause show"),TEXT("一時停止")),FVector2D(ControlOffset+296*S,Y-10*S),14,Ink,Layer+6);
    Out.PopClip();
    return Layer+6;
}
FReply SGCSLoginShow::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E)
{
    if(E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    DragStart=G.AbsoluteToLocal(E.GetScreenSpacePosition());Dragging=true;
    return FReply::Handled().CaptureMouse(SharedThis(this));
}
FReply SGCSLoginShow::OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E)
{
    if(!Dragging || E.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    Dragging=false;FVector2D P=G.AbsoluteToLocal(E.GetScreenSpacePosition());
    const float S=FMath::Clamp(FMath::Min(G.GetLocalSize().X/960.f,G.GetLocalSize().Y/780.f),.5f,1.6f);
    if(FMath::Abs(P.X-DragStart.X)>50*S)State->Next(P.X<DragStart.X?1:-1);
    else if(FMath::Abs(P.Y-G.GetLocalSize().Y*.88f)<22*S)
    {
        P.X-=G.GetLocalSize().X*.5f-220*S;
        if(FMath::Abs(P.X-56*S)<22*S)State->Next(-1);
        else if(FMath::Abs(P.X-196*S)<22*S)State->Next(1);
        else if(P.X>246*S && P.X<410*S)State->Paused=!State->Paused;
        else for(int32 N=0;N<3;++N)if(FMath::Abs(P.X-(108+N*22)*S)<10*S && N!=State->Theme)State->Next(N-State->Theme);
    }
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled().ReleaseMouseCapture();
}
