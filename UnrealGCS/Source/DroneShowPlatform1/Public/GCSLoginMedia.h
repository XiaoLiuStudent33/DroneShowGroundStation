#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Styling/SlateBrush.h"
#include "GCSLoginMedia.generated.h"
class UMediaPlayer;
class UMediaTexture;
class UMaterialInstanceDynamic;
class UTexture2D;
struct FGCSLoginPresentation;

// Owns exactly one decoder. Posters cover loading, unavailable codecs and static mode.
UCLASS()
class DRONESHOWPLATFORM1_API UGCSLoginMedia final:public UObject
{
 GENERATED_BODY()
public:
 void Initialize();
 bool Update(const FGCSLoginPresentation& State,float Delta);
 void Shutdown();
 void SetStatic(bool Static);
 bool IsStatic()const{return bStatic;}
 bool IsVideoVisible()const{return VideoBlend>0 && bFrameReady;}
 bool IsStopped()const;
 bool IsDecoderClosed()const;
#if WITH_DEV_AUTOMATION_TESTS
 bool bMeasureFrames=false;
 TArray<float> SteadyFrameMs,TransitionFrameMs;
#endif
 int32 GetMediaTheme()const{return MediaTheme;}
 double GetPlaybackSeconds()const;
 float GetVideoBlend(int32 Theme)const{return Theme==MediaTheme && bFrameReady?VideoBlend:0.f;}
 const FSlateBrush* GetPoster(int32 Theme)const{return Posters.IsValidIndex(Theme)?&Posters[Theme]:nullptr;}
 const FSlateBrush* GetVideo()const{return &VideoBrush;}
protected:
 virtual void BeginDestroy()override;
private:
 UPROPERTY(Transient) TObjectPtr<UMediaPlayer> Player;
 UPROPERTY(Transient) TObjectPtr<UMediaTexture> Texture;
 UPROPERTY(Transient) TObjectPtr<UMaterialInstanceDynamic> VideoMaterial;
 UPROPERTY(Transient) TArray<TObjectPtr<UMaterialInstanceDynamic>> PosterMaterials;
 UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> PosterTextures;
 TArray<FSlateBrush> Posters;
 FSlateBrush VideoBrush;
 int32 MediaTheme=INDEX_NONE;
 bool bStatic=false,bStopped=false,bFrameReady=false,bFailed=false;
 double OpenStarted=0;
 float VideoBlend=0;
 void OpenTheme(int32 Theme);
};
