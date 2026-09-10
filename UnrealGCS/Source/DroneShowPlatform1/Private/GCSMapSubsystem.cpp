#include "GCSMapSubsystem.h"
#include "GCSInvisiblePawn.h"
#include "GCSUIBridgeSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Slider.h"
#include "Components/SpinBox.h"
#include "Components/TextBlock.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Framework/Application/SlateApplication.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShObjIdl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif
namespace {
bool PickMap(FString& Path)
{
#if PLATFORM_WINDOWS
 const HRESULT Init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
 if (FAILED(Init) && Init != RPC_E_CHANGED_MODE) return false;
 IFileOpenDialog* Dialog = nullptr;
 if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))))
 {
  const COMDLG_FILTERSPEC Filter[] = {{L"Map images (*.png;*.jpg;*.jpeg)", L"*.png;*.jpg;*.jpeg"}};
  Dialog->SetFileTypes(1, Filter);
  Dialog->SetOptions(FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
  HWND Parent = FSlateApplication::IsInitialized() ? static_cast<HWND>(const_cast<void*>(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr))) : nullptr;
  if (SUCCEEDED(Dialog->Show(Parent)))
  {
   IShellItem* Item = nullptr;
   if (SUCCEEDED(Dialog->GetResult(&Item)))
   {
    PWSTR Name = nullptr;
    if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Name))) { Path = Name; CoTaskMemFree(Name); }
    Item->Release();
   }
  }
  Dialog->Release();
 }
 if (SUCCEEDED(Init)) CoUninitialize();
#endif
 return !Path.IsEmpty();
}
}
bool UGCSMapSubsystem::EnsureMap()
{
 if (Plane.IsValid()) return true;
 if (!GetWorld()) return false;
 for (TActorIterator<AActor> It(GetWorld()); It; ++It)
 {
  if (!It->ActorHasTag(TEXT("GCS.SceneMap")) && !It->GetClass()->GetName().StartsWith(TEXT("BP_GCS_2DMapGround"))) continue;
  TInlineComponentArray<UStaticMeshComponent*> Meshes(*It);
  for (auto* Mesh : Meshes)
  {
   if (Mesh->GetFName() != TEXT("MapPlane") || !Mesh->GetStaticMesh()) continue;
   Plane = Mesh;
   SceneFrame = Mesh->GetComponentTransform();
   MeshSize = Mesh->GetStaticMesh()->GetBoundingBox().GetSize();
   if (MeshSize.X <= 0 || MeshSize.Y <= 0) { Plane.Reset(); return false; }
   DefaultWidth = Width = FMath::Abs(MeshSize.X * SceneFrame.GetScale3D().X) / 100.0;
   DefaultAspect = Aspect = FMath::Abs(MeshSize.Y * SceneFrame.GetScale3D().Y) / FMath::Max(1.0, Width * 100.0);
   East = North = Angle = 0; Opacity = 1; ImagePath.Reset();
   Texture = nullptr;
   if (auto* Old = Mesh->GetMaterial(0)) { UTexture* Original = nullptr; Old->GetTextureParameterValue(FMaterialParameterInfo(TEXT("MapTexture")), Original); Texture = Original; }
   auto* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Map/Materials/M_GCS_RuntimeMap.M_GCS_RuntimeMap"));
   if (Parent)
   {
    Material = UMaterialInstanceDynamic::Create(Parent, this);
    Mesh->SetMaterial(0, Material);
   }
   else { Material = nullptr; Status = TEXT("地图材质缺失：不透明度不可用"); }
   if (Status.IsEmpty()) Status = TEXT("当前地图：默认场地图");
   Restore();
   Apply();
   return true;
  }
 }
 return false;
}
bool UGCSMapSubsystem::GetSceneFrame(FTransform& Out)
{
 if (!EnsureMap()) return false;
 Out = SceneFrame;
 return true;
}
FString UGCSMapSubsystem::SettingsPath() const
{
 if (!TestSettingsPath.IsEmpty()) return TestSettingsPath;
 FString Name = GetWorld() ? GetWorld()->GetMapName() : TEXT("Scene");
 if (GetWorld()) Name.RemoveFromStart(GetWorld()->StreamingLevelsPrefix);
 return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MapSettings"), FPaths::MakeValidFileName(Name) + TEXT(".ini"));
}
void UGCSMapSubsystem::Restore()
{
 if (!bPersistent) return;
 FConfigFile C; C.Read(SettingsPath());
 float V;
 if (C.GetFloat(TEXT("Map"), TEXT("Width"), V) && FMath::IsFinite(V) && V >= 0.01 && V <= 100000) Width = V;
 if (C.GetFloat(TEXT("Map"), TEXT("East"), V) && FMath::IsFinite(V) && FMath::Abs(V) <= 100000) East = V;
 if (C.GetFloat(TEXT("Map"), TEXT("North"), V) && FMath::IsFinite(V) && FMath::Abs(V) <= 100000) North = V;
 if (C.GetFloat(TEXT("Map"), TEXT("Angle"), V) && FMath::IsFinite(V)) Angle = FMath::UnwindDegrees(V);
 if (C.GetFloat(TEXT("Map"), TEXT("Opacity"), V) && FMath::IsFinite(V)) Opacity = FMath::Clamp(V,0.f,1.f);
 FString Path;
 if (C.GetString(TEXT("Map"), TEXT("Image"), Path) && !Path.IsEmpty()) ImportImage(Path);
}
void UGCSMapSubsystem::Save()
{
 if (!bPersistent || !Plane.IsValid()) return;
 FConfigFile C;
 C.SetFloat(TEXT("Map"), TEXT("Width"), Width);
 C.SetFloat(TEXT("Map"), TEXT("East"), East);
 C.SetFloat(TEXT("Map"), TEXT("North"), North);
 C.SetFloat(TEXT("Map"), TEXT("Angle"), Angle);
 C.SetFloat(TEXT("Map"), TEXT("Opacity"), Opacity);
 C.SetString(TEXT("Map"), TEXT("Image"), *ImagePath);
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(SettingsPath()), true);
 if (!C.Write(SettingsPath())) { Status = TEXT("地图设置保存失败"); SyncUI(); }
}
void UGCSMapSubsystem::ScheduleSave()
{
 if (bPersistent && GetWorld()) GetWorld()->GetTimerManager().SetTimer(SaveTimer, this, &UGCSMapSubsystem::Save, 0.5f, false);
}
void UGCSMapSubsystem::Apply()
{
 if (!Plane.IsValid()) return;
 const FVector E = SceneFrame.TransformVectorNoScale(FVector::XAxisVector).GetSafeNormal();
 const FVector N = -SceneFrame.TransformVectorNoScale(FVector::YAxisVector).GetSafeNormal();
 const FVector U = SceneFrame.TransformVectorNoScale(FVector::ZAxisVector).GetSafeNormal();
 FTransform T = SceneFrame;
 T.SetLocation(SceneFrame.GetLocation() + (E * East + N * North) * 100.0);
 T.SetRotation(FQuat(U, FMath::DegreesToRadians(Angle)) * SceneFrame.GetRotation());
 FVector S = SceneFrame.GetScale3D();
 S.X = FMath::Sign(S.X) * Width * 100.0 / MeshSize.X;
 S.Y = FMath::Sign(S.Y) * Width * Aspect * 100.0 / MeshSize.Y;
 T.SetScale3D(S);
 Plane->SetWorldTransform(T);
 if (Material)
 {
  if (Texture) Material->SetTextureParameterValue(TEXT("MapTexture"), Texture);
  Material->SetScalarParameterValue(TEXT("MapOpacity"), Opacity);
 }
 SyncUI();
}
bool UGCSMapSubsystem::SetLayout(double W, double E, double N, double A)
{
 if (!FMath::IsFinite(W) || !FMath::IsFinite(E) || !FMath::IsFinite(N) || !FMath::IsFinite(A) || W < 0.01 || W > 100000 || FMath::Abs(E)>100000 || FMath::Abs(N)>100000 || !EnsureMap()) return false;
 Width=W; East=E; North=N; Angle=FMath::UnwindDegrees(A);
 Apply(); ScheduleSave(); return true;
}
void UGCSMapSubsystem::ImportImage(const FString& Path)
{
 if (bLoading || !EnsureMap() || !Material) return;
 bLoading=true; const int32 Serial=++ImportSerial;
 Status=TEXT("正在加载地图…"); SyncUI();
 auto* Module=&FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
 TWeakObjectPtr<UGCSMapSubsystem> Weak(this);
 const FString FullPath=FPaths::ConvertRelativePathToFull(Path);
 Async(EAsyncExecution::ThreadPool, [Weak, Serial, FullPath, Module]()
 {
  FString Error; TArray<uint8> Bytes; TArray64<uint8> Pixels; int32 W=0,H=0;
  const int64 Size=IFileManager::Get().FileSize(*FullPath);
  if (Size<=0 || Size>32*1024*1024 || !FFileHelper::LoadFileToArray(Bytes,*FullPath)) Error=TEXT("读取失败：图片不存在或超过 32 MB");
  else
  {
   const EImageFormat Format=Module->DetectImageFormat(Bytes.GetData(),Bytes.Num());
   auto Decoder=(Format==EImageFormat::PNG || Format==EImageFormat::JPEG) ? Module->CreateImageWrapper(Format) : nullptr;
   if (!Decoder || !Decoder->SetCompressed(Bytes.GetData(),Bytes.Num())) Error=TEXT("仅支持有效的 PNG/JPG 图片");
   else
   {
    W=Decoder->GetWidth(); H=Decoder->GetHeight();
    if (W<1 || H<1 || W>4096 || H>4096) Error=TEXT("图片尺寸须在 1～4096 像素之间");
    else if (!Decoder->GetRaw(ERGBFormat::BGRA,8,Pixels) || Pixels.Num()!=int64(W)*H*4) Error=TEXT("地图图片解码失败");
   }
  }
  AsyncTask(ENamedThreads::GameThread,[Weak,Serial,FullPath,Error,W,H,Pixels=MoveTemp(Pixels)]() mutable
  {
   auto* Self=Weak.Get(); if (!Self || Self->ImportSerial!=Serial) return;
   Self->bLoading=false;
   if (!Error.IsEmpty()) { Self->Status=Error; Self->SyncUI(); return; }
   auto* T=UTexture2D::CreateTransient(W,H,PF_B8G8R8A8);
   if (!T) { Self->Status=TEXT("地图纹理创建失败"); Self->SyncUI(); return; }
   auto& Mip=T->GetPlatformData()->Mips[0];
   void* Data=Mip.BulkData.Lock(LOCK_READ_WRITE); FMemory::Memcpy(Data,Pixels.GetData(),Pixels.Num()); Mip.BulkData.Unlock();
   T->SRGB=true; T->UpdateResource();
   Self->Texture=T; Self->Aspect=double(H)/W; Self->ImagePath=FullPath;
   Self->Status=FString::Printf(TEXT("%s · %d×%d"),*FPaths::GetCleanFilename(FullPath),W,H);
   Self->Apply(); Self->ScheduleSave();
  });
 });
}
void UGCSMapSubsystem::OnImport() { FString P; if (PickMap(P)) ImportImage(P); }
void UGCSMapSubsystem::OnFit()
{
 if (!EnsureMap()) return;
 auto* PC=Panel.IsValid()?Panel->GetOwningPlayer():nullptr;
 if (auto* Pawn=PC?Cast<AGCSInvisiblePawn>(PC->GetPawn()):nullptr)
 {
  if (auto* Bridge=GetGameInstance()->GetSubsystem<UGCSUIBridgeSubsystem>()) Bridge->EndTopDownLocateLock(PC);
  PC->SetViewTarget(Pawn);
  Pawn->FitMapBounds(Plane->Bounds.GetBox(), SceneFrame.GetRotation().GetAxisX(), SceneFrame.GetRotation().GetAxisZ());
 }
}
void UGCSMapSubsystem::OnReset() { SetLayout(DefaultWidth,0,0,0); }
void UGCSMapSubsystem::OnOpacity(float V) { if (!bSync && FMath::IsFinite(V) && EnsureMap()) { Opacity=FMath::Clamp(V,0.f,1.f); Apply(); ScheduleSave(); } }
void UGCSMapSubsystem::OnWidth(float V) { if (!bSync && !SetLayout(V,East,North,Angle)) SyncUI(); }
void UGCSMapSubsystem::OnEast(float V) { if (!bSync && !SetLayout(Width,V,North,Angle)) SyncUI(); }
void UGCSMapSubsystem::OnNorth(float V) { if (!bSync && !SetLayout(Width,East,V,Angle)) SyncUI(); }
void UGCSMapSubsystem::OnAngle(float V) { if (!bSync && !SetLayout(Width,East,North,V)) SyncUI(); }
void UGCSMapSubsystem::SyncUI()
{
 TGuardValue<bool> Guard(bSync,true);
 if (auto* C=WidthInput.Get()) C->SetValue(Width);
 if (auto* C=EastInput.Get()) C->SetValue(East);
 if (auto* C=NorthInput.Get()) C->SetValue(North);
 if (auto* C=AngleInput.Get()) C->SetValue(Angle);
 if (auto* C=OpacitySlider.Get()) { C->SetValue(Opacity); C->SetIsEnabled(Material!=nullptr); }
 if (auto* C=OpacityText.Get()) C->SetText(FText::FromString(FString::Printf(TEXT("%d%%"),FMath::RoundToInt(Opacity*100))));
 if (auto* C=StatusText.Get()) C->SetText(FText::FromString(Status));
 if (auto* C=ImportButton.Get()) C->SetIsEnabled(!bLoading && Material!=nullptr);
}
void UGCSMapSubsystem::BindUI(UUserWidget* Widget)
{
 UnbindUI(); Panel=Widget;
 if (!Widget || !Widget->WidgetTree) return;
 auto Find=[Widget](const TCHAR* Name) { return Widget->WidgetTree->FindWidget(FName(Name)); };
 ImportButton=Cast<UButton>(Find(TEXT("BTN_ImportMapTexture")));
 FitButton=Cast<UButton>(Find(TEXT("BTN_FitMapView")));
 ResetButton=Cast<UButton>(Find(TEXT("BTN_ResetMapTransform")));
 OpacitySlider=Cast<USlider>(Find(TEXT("SLD_MapOpacity")));
 WidthInput=Cast<USpinBox>(Find(TEXT("SPN_MapWidthMeters")));
 EastInput=Cast<USpinBox>(Find(TEXT("SPN_MapOffsetX")));
 NorthInput=Cast<USpinBox>(Find(TEXT("SPN_MapOffsetY")));
 AngleInput=Cast<USpinBox>(Find(TEXT("SPN_MapRotationDegrees")));
 StatusText=Cast<UTextBlock>(Find(TEXT("TXT_MapStatus")));
 OpacityText=Cast<UTextBlock>(Find(TEXT("TXT_MapOpacityValue")));
 if (auto* C=ImportButton.Get()) C->OnClicked.AddUniqueDynamic(this,&UGCSMapSubsystem::OnImport);
 if (auto* C=FitButton.Get()) C->OnClicked.AddUniqueDynamic(this,&UGCSMapSubsystem::OnFit);
 if (auto* C=ResetButton.Get()) C->OnClicked.AddUniqueDynamic(this,&UGCSMapSubsystem::OnReset);
 if (auto* C=OpacitySlider.Get()) { C->SetMinValue(0); C->SetMaxValue(1); C->OnValueChanged.AddUniqueDynamic(this,&UGCSMapSubsystem::OnOpacity); }
 TGuardValue<bool> Guard(bSync,true);
 if (auto* C=WidthInput.Get()) { C->SetMinValue(0.01); C->SetMaxValue(100000); C->OnValueChanged.AddUniqueDynamic(this,&UGCSMapSubsystem::OnWidth); }
 if (auto* C=EastInput.Get()) { C->SetMinValue(-100000); C->SetMaxValue(100000); C->OnValueChanged.AddUniqueDynamic(this,&UGCSMapSubsystem::OnEast); }
 if (auto* C=NorthInput.Get()) { C->SetMinValue(-100000); C->SetMaxValue(100000); C->OnValueChanged.AddUniqueDynamic(this,&UGCSMapSubsystem::OnNorth); }
 if (auto* C=AngleInput.Get()) { C->SetMinValue(-360); C->SetMaxValue(360); C->OnValueChanged.AddUniqueDynamic(this,&UGCSMapSubsystem::OnAngle); }
 if (!EnsureMap()) Status=TEXT("未找到场地地图");
 SyncUI();
}
void UGCSMapSubsystem::UnbindUI()
{
 if (auto* C=ImportButton.Get()) C->OnClicked.RemoveDynamic(this,&UGCSMapSubsystem::OnImport);
 if (auto* C=FitButton.Get()) C->OnClicked.RemoveDynamic(this,&UGCSMapSubsystem::OnFit);
 if (auto* C=ResetButton.Get()) C->OnClicked.RemoveDynamic(this,&UGCSMapSubsystem::OnReset);
 if (auto* C=OpacitySlider.Get()) C->OnValueChanged.RemoveDynamic(this,&UGCSMapSubsystem::OnOpacity);
 if (auto* C=WidthInput.Get()) C->OnValueChanged.RemoveDynamic(this,&UGCSMapSubsystem::OnWidth);
 if (auto* C=EastInput.Get()) C->OnValueChanged.RemoveDynamic(this,&UGCSMapSubsystem::OnEast);
 if (auto* C=NorthInput.Get()) C->OnValueChanged.RemoveDynamic(this,&UGCSMapSubsystem::OnNorth);
 if (auto* C=AngleInput.Get()) C->OnValueChanged.RemoveDynamic(this,&UGCSMapSubsystem::OnAngle);
 ImportButton.Reset(); FitButton.Reset(); ResetButton.Reset(); OpacitySlider.Reset(); WidthInput.Reset(); EastInput.Reset(); NorthInput.Reset(); AngleInput.Reset(); StatusText.Reset(); OpacityText.Reset(); Panel.Reset();
}
void UGCSMapSubsystem::Deinitialize()
{
 if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(SaveTimer);
 Save(); ++ImportSerial; UnbindUI(); Material=nullptr; Texture=nullptr; Plane.Reset(); Super::Deinitialize();
}
