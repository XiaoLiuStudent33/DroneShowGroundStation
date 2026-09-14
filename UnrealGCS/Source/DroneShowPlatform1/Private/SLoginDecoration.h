#pragma once
#include "Widgets/SLeafWidget.h"
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
class SLoginDecoration:public SLeafWidget
{
public:
 SLATE_BEGIN_ARGS(SLoginDecoration):_Kind(0){} SLATE_ARGUMENT(int32,Kind) SLATE_END_ARGS()
 void Construct(const FArguments& A){Kind=A._Kind;}
 FVector2D ComputeDesiredSize(float)const override{return Kind==0?FVector2D(58,36):FVector2D(26,26);}
 int32 OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& O,int32 L,const FWidgetStyle&,bool)const override
 {
  FVector2D S=G.GetLocalSize();
  if(Kind==6)
  {
   auto Color=[](uint32 H){return FLinearColor::FromSRGBColor(FColor(H>>16,(H>>8)&255,H&255));};
   TArray<FSlateGradientStop> Stops;
   Stops.Add({FVector2D(0,0),Color(0x14253f)});Stops.Add({FVector2D(0,S.Y*.55f),Color(0x0d1c30)});Stops.Add({FVector2D(0,S.Y),Color(0x091425)});
   FSlateDrawElement::MakeGradient(O,L,G.ToPaintGeometry(),Stops,Orient_Vertical);
   TArray<FSlateVertex> V;TArray<SlateIndex> I;
   auto Halo=[&](FVector2f C,FVector2f R,FColor Ink)
   {
    SlateIndex B=V.Num();V.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),C,FVector2f(.5f),Ink));Ink.A=0;
    for(int N=0;N<64;++N){float A=N*PI/32;V.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(G.GetAccumulatedRenderTransform(),C+FVector2f(FMath::Cos(A)*R.X,FMath::Sin(A)*R.Y),FVector2f(.5f),Ink));}
    for(int N=0;N<64;++N)I.Append({B,SlateIndex(B+1+N),SlateIndex(B+1+(N+1)%64)});
   };
   Halo(FVector2f(S.X*.31f,S.Y*.42f),FVector2f(S.X*.40f,S.Y*.72f),FColor(46,85,141,100));
   Halo(FVector2f(S.X*.67f,S.Y*.15f),FVector2f(S.X*.49f,S.Y*.40f),FColor(39,72,121,65));
   FSlateDrawElement::MakeCustomVerts(O,L+1,FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush("WhiteBrush")),V,I,nullptr,0,0);return L+1;
  }
  if(Kind==4){TArray<FSlateGradientStop> Stops;Stops.Add(FSlateGradientStop(FVector2D(0,0),FLinearColor(.018f,.038f,.085f)));Stops.Add(FSlateGradientStop(S*.5f,FLinearColor(.029f,.061f,.13f)));Stops.Add(FSlateGradientStop(S,FLinearColor(.012f,.026f,.065f)));FSlateDrawElement::MakeGradient(O,L,G.ToPaintGeometry(),Stops,Orient_Vertical);return L;}
  auto Line=[&](TArray<FVector2D> P,float W=1.5f,FLinearColor C=FLinearColor(.45f,.65f,.9f)){for(auto& V:P)V*=S;FSlateDrawElement::MakeLines(O,L,G.ToPaintGeometry(),P,ESlateDrawEffect::None,C,true,W);};
  auto Circle=[&](float X,float Y,float RX,float RY){TArray<FVector2D>P;for(int N=0;N<=40;++N){float T=N*2*PI/40;P.Add(FVector2D(X+RX*FMath::Cos(T),Y+RY*FMath::Sin(T)));}Line(P);};
  if(Kind==0){Line({{.06f,.90f},{.06f,.14f},{.38f,.62f},{.68f,.14f},{.94f,.5f},{.68f,.86f}},7,FLinearColor(.25f,.60f,1));Line({{.10f,.14f},{.38f,.56f},{.68f,.14f},{.91f,.48f}},7,FLinearColor(.8f,.92f,1));}
  if(Kind==1){Circle(.5f,.28f,.16f,.16f);Line({{.18f,.88f},{.22f,.69f},{.34f,.58f},{.66f,.58f},{.78f,.69f},{.82f,.88f}});}
  if(Kind==2){Line({{.20f,.43f},{.80f,.43f},{.80f,.9f},{.2f,.9f},{.2f,.43f}});Line({{.32f,.43f},{.32f,.25f},{.40f,.13f},{.60f,.13f},{.68f,.25f},{.68f,.43f}});Line({{.5f,.60f},{.5f,.76f}});}
  if(Kind==3){Circle(.5f,.5f,.4f,.4f);Circle(.5f,.5f,.18f,.4f);Line({{.12f,.36f},{.88f,.36f}});Line({{.12f,.64f},{.88f,.64f}});}
  if(Kind==5){Line({{.08f,.5f},{.26f,.29f},{.5f,.2f},{.74f,.29f},{.92f,.5f},{.74f,.71f},{.5f,.8f},{.26f,.71f},{.08f,.5f}});Circle(.5f,.5f,.13f,.13f);}
  if(Kind==7){Line({{.52f,.13f},{.20f,.13f},{.14f,.19f},{.14f,.81f},{.20f,.87f},{.52f,.87f}});Line({{.40f,.5f},{.91f,.5f}});Line({{.70f,.29f},{.91f,.5f},{.70f,.71f}});}
  return L;
 }
private:int32 Kind=0;
};
