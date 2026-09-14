#pragma once
#include "Rendering/DrawElements.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

// Decorative geometry only. Bounded batches retain 16-bit index safety during slides.
inline void DrawLoginPlanet(const FGeometry& G,FSlateWindowElementList& Out,int32 Layer,FVector2D Center,float Radius,double Time,int32 Theme)
{
    struct FLight {FVector2f P;FColor C;float Size,Glow;bool Spark;};
    TArray<FLight> Lights;Lights.Reserve(5000);
    TArray<FSlateVertex> V;TArray<SlateIndex> Idx;V.Reserve(60000);Idx.Reserve(180000);
    const auto Transform=G.GetAccumulatedRenderTransform();
    const auto Handle=FSlateApplication::Get().GetRenderer()->GetResourceHandle(*FCoreStyle::Get().GetBrush("WhiteBrush"));
    auto Flush=[&]{if(V.Num()){FSlateDrawElement::MakeCustomVerts(Out,Layer,Handle,V,Idx,nullptr,0,0);V.Reset();Idx.Reset();}};
    auto Room=[&](int32 Count){if(V.Num()+Count>60000)Flush();};
    auto Vertex=[&](FVector2f P,FColor C){V.Add(FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform,P,FVector2f(.5f,.5f),C));};
    const float Scale=Radius/215.f,Spin=Time*.065f,CS=FMath::Cos(Spin),SN=FMath::Sin(Spin);
    const float Tilt=.24f,CT=FMath::Cos(Tilt),ST=FMath::Sin(Tilt);
    auto Project=[&](float X,float Y){return FVector2f(Center)+FVector2f(X*CT-Y*ST,X*ST+Y*CT)*Radius;};
    auto Fan=[&](FVector2f P,float R,FColor C,int32 Segments)
    {
        Room(Segments+1);SlateIndex Base=V.Num();Vertex(P,C);C.A=0;
        for(int J=0;J<Segments;++J){float A=J*2*PI/Segments;Vertex(P+FVector2f(FMath::Cos(A),FMath::Sin(A))*R,C);}
        for(int J=0;J<Segments;++J)Idx.Append({Base,SlateIndex(Base+1+J),SlateIndex(Base+1+(J+1)%Segments)});
    };
    auto Stroke=[&](FVector2f A,FVector2f B,float Width,FColor C)
    {
        FVector2f D=B-A;float Len=D.Size();if(Len<.001f)return;
        FVector2f N(-D.Y/Len*Width*.5f,D.X/Len*Width*.5f);
        Room(4);SlateIndex Base=V.Num();Vertex(A-N,C);Vertex(A+N,C);Vertex(B+N,C);Vertex(B-N,C);
        Idx.Append({Base,SlateIndex(Base+1),SlateIndex(Base+2),Base,SlateIndex(Base+2),SlateIndex(Base+3)});
    };
    // Local light, not an opaque blue sphere. The point cloud carries its volume.
    Fan(FVector2f(Center),Radius*1.7f,FColor(32,78,144,34),64);
    // Uneven blue-white limb: bright edge with a broad, very faint outer falloff.
    for(int J=0;J<192;++J)
    {
        float A=J*2*PI/192,B=(J+1)*2*PI/192;
        float Pulse=.64f+.23f*FMath::Sin(A*7+.6f)+.13f*FMath::Sin(A*19);
        FVector2f D(FMath::Cos(A),FMath::Sin(A)),E(FMath::Cos(B),FMath::Sin(B));
        const float Ranges[]={.975f,.998f,1.009f,1.065f};
        const uint8 Alphas[]={0,uint8(95*Pulse),uint8(46*Pulse),0};
        Room(8);SlateIndex Base=V.Num();
        for(int K=0;K<4;++K){FColor C(123,180,255,Alphas[K]);Vertex(FVector2f(Center)+D*Radius*Ranges[K],C);Vertex(FVector2f(Center)+E*Radius*Ranges[K],C);}
        for(int K=0;K<3;++K){SlateIndex Q=Base+K*2;Idx.Append({Q,SlateIndex(Q+1),SlateIndex(Q+2),SlateIndex(Q+1),SlateIndex(Q+3),SlateIndex(Q+2)});}
    }
    auto Add=[&](float X,float Y,float Z,bool Gold,float Seed,float Size,float Strength,bool Spark=false)
    {
        float Breath=.91f+.09f*FMath::Sin(Time*.65f+Seed);
        FLinearColor C=Gold?FLinearColor(1,.65f,.28f):FLinearColor(.48f,.72f,1);
        if(Theme==1 && !Gold)C=FLinearColor(.58f,.91f,1);
        if(Theme==2 && !Gold)C=FLinearColor(.73f,.77f,1);
        // Fine white cores remain crisp; luminosity varies independently from diameter.
        C=FMath::Lerp(C,FLinearColor::White,Spark?.72f:.12f);
        C.A=FMath::Clamp(Strength*Breath,.08f,1.f);
        Lights.Add({Project(X,Y),C.ToFColor(true),Size*Scale,Strength,Spark});
    };
    static const TArray<FVector3f> Sphere=[]
    {
        TArray<FVector3f> P;P.Reserve(5000);FRandomStream Random(414);
        for(int I=0;I<5000;++I){float Y=Random.FRandRange(-.999f,.999f),R=FMath::Sqrt(1-Y*Y),A=Random.FRandRange(0.f,2*PI);P.Add({R*FMath::Cos(A),Y,R*FMath::Sin(A)});}return P;
    }();
    for(int I=0;I<Sphere.Num();++I)
    {
        const auto& P=Sphere[I];float X=P.X*CS+P.Z*SN,Z=-P.X*SN+P.Z*CS;
        if(Z<0)continue;
        float Noise=FMath::Frac(I*.618034f),Limb=FMath::Pow(1-Z,3.f);
        bool Spark=I%79==0;
        float Strength=.57f+Noise*.35f+Limb*.35f;
        Add(X,P.Y,Z,I%17==0,I*.71f,Spark?1.45f:(.52f+Noise*.55f+Limb*.40f),Spark?1.f:Strength,Spark);
    }
    // Broken clusters along the silhouette give the luminous edge its point-light texture.
    for(int I=0;I<460;++I)
    {
        float A=I*2*PI/460+Spin*.09f,Jitter=FMath::Sin(I*17.13f),R=1+Jitter*.008f;
        Add(FMath::Cos(A)*R,FMath::Sin(A)*R,0,false,I,.44f+.53f*FMath::Abs(Jitter),.65f+.35f*FMath::Abs(Jitter),I%83==0);
    }
    const float Bands[]={1.32f,1.36f,1.43f,1.49f,1.57f,1.61f,1.73f};
    auto RingPoint=[&](float A,float R){return FVector3f(R*FMath::Cos(A),.27f*R*FMath::Sin(A),.963f*R*FMath::Sin(A));};
    auto Visible=[](FVector3f P){float R2=P.X*P.X+P.Y*P.Y;return R2>=1 || P.Z>FMath::Sqrt(1-R2);};
    for(int Band=0;Band<7;++Band)
    {
        // Hairline orbital traces behind scattered beads, with real front/back occlusion.
        for(int I=0;I<360;++I)
        {
            float A=I*2*PI/360;auto P=RingPoint(A,Bands[Band]),Q=RingPoint(A+2*PI/360,Bands[Band]);
            if(!Visible(P)||!Visible(Q))continue;
            Stroke(Project(P.X,P.Y),Project(Q.X,Q.Y),.85f*Scale,FColor(255,215,149,uint8(62+Band*5)));
        }
        for(int I=0;I<250;++I)
        {
            float A=I*2*PI/250+Spin*.45f+Band*.13f;
            float N=FMath::Frac(I*.754877f+Band*.56984f),R=Bands[Band]+(N-.5f)*.01f;
            auto P=RingPoint(A,R);if(!Visible(P))continue;
            bool Spark=(I+Band*13)%41==0;
            Add(P.X,P.Y,P.Z,true,A*2+Band,Spark?1.55f:.45f+N*N*.85f,Spark?1.f:.60f+N*.40f,Spark);
        }
    }
    FRandomStream Specks(735);
    for(int I=0;I<150;++I)
    {
        float X=Specks.FRandRange(-1.95f,1.95f),Y=Specks.FRandRange(-1.15f,1.15f),N=Specks.FRand();
        if(X*X+Y*Y>1.16f)Add(X,Y,0,I%4==0,I*1.5f,I%14==0?1.10f:.25f+N*.32f,I%14==0?1.f:.2f+N*.5f,I%14==0);
    }
    for(const auto& Light:Lights)
    {
        FColor Glow=Light.C;Glow.A=uint8((Light.Spark?75:46)*FMath::Min(Light.Glow,1.f));
        Fan(Light.P,Light.Size*(Light.Spark?4.8f:3.3f),Glow,8);
        // Small solid core plus alpha falloff, rather than large fuzzy uniform diamonds.
        FColor Core=Light.C;
        Fan(Light.P,Light.Size*1.75f,Core,8);
        // Tiny opaque center survives downsampling; the surrounding halo stays translucent.
        Room(5);SlateIndex Base=V.Num();Vertex(Light.P,Core);
        for(int J=0;J<4;++J){float A=J*PI/2;Vertex(Light.P+FVector2f(FMath::Cos(A),FMath::Sin(A))*Light.Size*.60f,Core);}
        for(int J=0;J<4;++J)Idx.Append({Base,SlateIndex(Base+1+J),SlateIndex(Base+1+(J+1)%4)});
        if(Light.Spark)
        {
            FColor Flare=Light.C;Flare.A=45;
            Stroke(Light.P-FVector2f(Light.Size*2.5f,0),Light.P+FVector2f(Light.Size*2.5f,0),.45f*Scale,Flare);
            Stroke(Light.P-FVector2f(0,Light.Size*2.5f),Light.P+FVector2f(0,Light.Size*2.5f),.45f*Scale,Flare);
        }
    }
    Flush();
}
