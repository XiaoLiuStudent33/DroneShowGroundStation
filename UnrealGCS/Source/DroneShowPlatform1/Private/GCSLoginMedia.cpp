#include "GCSLoginMedia.h"
#include "GCSLoginPresentation.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace {const TCHAR* ThemeNames[]={TEXT("Saturn"),TEXT("Wing"),TEXT("Tide")};}
void UGCSLoginMedia::Initialize()
{
 auto* Base=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/GCS/Login/Media/M_LoginLight.M_LoginLight"));
 if(!Base){bStatic=true;UE_LOG(LogTemp,Error,TEXT("Login media: missing light material"));return;}
 Posters.SetNum(3);
 for(int32 I=0;I<3;++I)
 {
  const FString Path=FString::Printf(TEXT("/Game/GCS/Login/Media/T_%s.T_%s"),ThemeNames[I],ThemeNames[I]);
  auto* Poster=LoadObject<UTexture2D>(nullptr,*Path);PosterTextures.Add(Poster);
  auto* Mat=UMaterialInstanceDynamic::Create(Base,this);Mat->SetTextureParameterValue(TEXT("ShowTexture"),Poster);PosterMaterials.Add(Mat);
  Posters[I].SetResourceObject(Mat);Posters[I].ImageSize=FVector2D(1280,800);Posters[I].DrawAs=ESlateBrushDrawType::Image;
 }
 Player=NewObject<UMediaPlayer>(this);Player->PlayOnOpen=true;Player->SetLooping(true);
 Texture=NewObject<UMediaTexture>(this);Texture->NewStyleOutput=true;Texture->AutoClear=true;Texture->SetMediaPlayer(Player);Texture->UpdateResource();
 VideoMaterial=UMaterialInstanceDynamic::Create(Base,this);VideoMaterial->SetTextureParameterValue(TEXT("ShowTexture"),Texture);
 VideoBrush.SetResourceObject(VideoMaterial);VideoBrush.ImageSize=FVector2D(1280,800);VideoBrush.DrawAs=ESlateBrushDrawType::Image;
 GConfig->GetBool(TEXT("MD.Login.Presentation"),TEXT("StaticShow"),bStatic,GGameUserSettingsIni);
 bStatic |= FParse::Param(FCommandLine::Get(),TEXT("LoginStaticShow"));
 if(!bStatic)OpenTheme(0);
}
void UGCSLoginMedia::OpenTheme(int32 Theme)
{
 if(!Player || bStopped)return;
 Player->Close();MediaTheme=Theme;bFrameReady=false;bFailed=false;VideoBlend=0;OpenStarted=FPlatformTime::Seconds();
 const FString Path=FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(),TEXT("Movies/Login"),FString(ThemeNames[Theme])+TEXT(".mp4")));
 bFailed=!Player->OpenFile(Path);
 if(bFailed)UE_LOG(LogTemp,Warning,TEXT("Login media: poster fallback for %s"),ThemeNames[Theme]);
}
bool UGCSLoginMedia::Update(const FGCSLoginPresentation& State,float Delta)
{
 if(bStopped || bStatic || !Player)return false;
#if WITH_DEV_AUTOMATION_TESTS
 if(bMeasureFrames && Delta>0){auto& Frames=State.Transition<1?TransitionFrameMs:SteadyFrameMs;if(Frames.Num()<8000)Frames.Add(Delta*1000);}
#endif
 const float Before=VideoBlend;
 // Keep the outgoing movie until the horizontal transition has finished.
 // The incoming slide is its first-frame poster, so no second decoder is needed.
 if(State.Transition>=1 && MediaTheme!=State.Theme)OpenTheme(State.Theme);
 if(!bFailed)
 {
  if(Player->HasError() || (!bFrameReady && FPlatformTime::Seconds()-OpenStarted>5))
  {bFailed=true;Player->Close();VideoBlend=0;UE_LOG(LogTemp,Warning,TEXT("Login media: playback unavailable; keeping poster"));}
  else if(Player->IsReady() && Player->GetTime().GetTotalSeconds()>.06)
  {
   bFrameReady=true;VideoBlend=FMath::Min(1.f,VideoBlend+Delta/.30f);
   if(State.Paused && Player->IsPlaying())Player->Pause();
   else if(!State.Paused && !Player->IsPlaying())Player->Play();
  }
 }
 return Before!=VideoBlend;
}
void UGCSLoginMedia::SetStatic(bool Static)
{
 bStatic=Static;VideoBlend=0;bFrameReady=false;
 if(Player)Player->Close();MediaTheme=INDEX_NONE;
}
void UGCSLoginMedia::Shutdown()
{
 bStopped=true;bFrameReady=false;VideoBlend=0;
 if(Player)Player->Close();
 if(Texture)Texture->SetMediaPlayer(nullptr);
}
bool UGCSLoginMedia::IsDecoderClosed()const{return !Player || Player->IsClosed();}
bool UGCSLoginMedia::IsStopped()const{return bStopped && (!Player || Player->IsClosed());}
double UGCSLoginMedia::GetPlaybackSeconds()const{return Player?Player->GetTime().GetTotalSeconds():0;}
void UGCSLoginMedia::BeginDestroy(){Shutdown();Super::BeginDestroy();}
