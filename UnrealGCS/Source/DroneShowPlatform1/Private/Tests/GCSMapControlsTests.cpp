#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "GCSMapSubsystem.h"
#include "GCSInvisiblePawn.h"
#include "Blueprint/UserWidget.h"
#include "Components/StaticMeshComponent.h"
#include "Components/Button.h"
#include "Components/SpinBox.h"
#include "Components/Slider.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "UObject/Script.h"
class FMapWait final : public IAutomationLatentCommand
{
 TFunction<bool()> Work;
public:
 explicit FMapWait(TFunction<bool()> In):Work(MoveTemp(In)){}
 virtual bool Update() override { return Work(); }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGCSMapControlsTest,"DroneGCS.UI.Map.ControlsAndPersistence",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FGCSMapControlsTest::RunTest(const FString&)
{
 auto* GI=NewObject<UGameInstance>(GEngine); GI->AddToRoot(); GI->InitializeStandalone(TEXT("MapControlsTest"));
 auto* Map=GI->GetSubsystem<UGCSMapSubsystem>(); Map->bPersistent=false;
 FEditorScriptExecutionGuard Guard;
 auto* Class=LoadClass<AActor>(nullptr,TEXT("/Game/Blueprints/BP_GCS_2DMapGround.BP_GCS_2DMapGround_C"));
 if (!TestNotNull(TEXT("Actual map class"),Class)) { GI->Shutdown(); GI->RemoveFromRoot(); return false; }
 auto* Actor=GI->GetWorld()->SpawnActor<AActor>(Class);
 Actor->SetActorScale3D(FVector(1000));
 Actor->SetActorRotation(FRotator(10,25,0));
 auto* UIClass=LoadClass<UUserWidget>(nullptr,TEXT("/Game/UI/WBP_GCS_RightPanel.WBP_GCS_RightPanel_C"));
 auto* Panel=CreateWidget<UUserWidget>(GI,UIClass); Panel->AddToRoot();
 Map->BindUI(Panel);
 TestTrue(TEXT("Import control named correctly"),Map->ImportButton.IsValid());
 TestTrue(TEXT("Fit control named correctly"),Map->FitButton.IsValid());
 TestTrue(TEXT("Reset control named correctly"),Map->ResetButton.IsValid());
 TestTrue(TEXT("Opacity value named correctly"),Map->OpacityText.IsValid());
 TestTrue(TEXT("Status named correctly"),Map->StatusText.IsValid());
 TestTrue(TEXT("All inputs named correctly"),Map->WidthInput.IsValid()&&Map->EastInput.IsValid()&&Map->NorthInput.IsValid()&&Map->AngleInput.IsValid()&&Map->OpacitySlider.IsValid());
 FTransform Frame; TestTrue(TEXT("Scene frame captured"),Map->GetSceneFrame(Frame));
 TestTrue(TEXT("Runtime map material loaded"),Map->Material!=nullptr);
 TestTrue(TEXT("Set artwork layout"),Map->SetLayout(1500,20,30,90));
 FTransform After; Map->GetSceneFrame(After);
 TestTrue(TEXT("Flight frame unaffected by artwork"),Frame.Equals(After));
 const FVector E=Frame.GetRotation().GetAxisX(),N=-Frame.GetRotation().GetAxisY();
 TestTrue(TEXT("Offsets are meters in fixed frame"),Map->Plane->GetComponentLocation().Equals(Frame.GetLocation()+E*2000+N*3000,0.01));
 TestTrue(TEXT("Width is in meters"),FMath::IsNearlyEqual(Map->Plane->GetComponentScale().X*Map->MeshSize.X,150000.0,0.01));
 Map->OnOpacity(0.4f);
 float Value=0; Map->Material->GetScalarParameterValue(FMaterialParameterInfo(TEXT("MapOpacity")),Value);
 TestTrue(TEXT("Opacity reaches material"),FMath::IsNearlyEqual(Value,0.4f));
 Map->BindUI(Panel); TestTrue(TEXT("Rebind preserves layout"),FMath::IsNearlyEqual(Map->Width,1500.0));
 if(auto* Input=Map->EastInput.Get()) Input->OnValueChanged.Broadcast(42);
 TestTrue(TEXT("Input changes artwork"),FMath::IsNearlyEqual(Map->East,42.0));
 auto* OldInput=Map->EastInput.Get(); Map->UnbindUI();
 if(OldInput) OldInput->OnValueChanged.Broadcast(99);
 TestTrue(TEXT("Old UI detached"),FMath::IsNearlyEqual(Map->East,42.0));
 TestFalse(TEXT("Zero width rejected"),Map->SetLayout(0,0,0,0));
 const FString Dir=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("Automation"),FGuid::NewGuid().ToString());
 IFileManager::Get().MakeDirectory(*Dir,true);
 Map->TestSettingsPath=FPaths::Combine(Dir,TEXT("map.ini")); Map->bPersistent=true; Map->Save();
 Map->OnReset(); Map->Restore();
 TestTrue(TEXT("Stored layout restored"),FMath::IsNearlyEqual(Map->Width,1500.0)&&FMath::IsNearlyEqual(Map->East,42.0));
 Map->bPersistent=false;
 auto Decoder=FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper")).CreateImageWrapper(EImageFormat::PNG);
 const uint8 Raw[]={255,0,0,255,0,255,0,255}; Decoder->SetRaw(Raw,8,2,1,ERGBFormat::BGRA,8);
 const auto& Compressed=Decoder->GetCompressed();
 const FString Image=FPaths::Combine(Dir,TEXT("map.png"));
 FFileHelper::SaveArrayToFile(TArrayView<const uint8>(Compressed.GetData(),Compressed.Num()),*Image);
 Map->ImportImage(Image);
 const double Start=FPlatformTime::Seconds();
 ADD_LATENT_AUTOMATION_COMMAND(FMapWait([this,GI,Map,Panel,Image,Start,Frame]()
 {
  if(Map->bLoading && FPlatformTime::Seconds()-Start<15) return false;
  TestFalse(TEXT("Async image import completes"),Map->bLoading);
  TestEqual(TEXT("Imported file accepted"),Map->ImagePath,Image);
  TestTrue(TEXT("Image aspect ratio applied"),FMath::IsNearlyEqual(Map->Aspect,0.5));
  TestTrue(TEXT("Imported texture dimensions"),Map->Texture && Cast<UTexture2D>(Map->Texture)->GetSizeX()==2);
  FTransform Current; Map->GetSceneFrame(Current); TestTrue(TEXT("Import preserves flight frame"),Current.Equals(Frame));
  Map->OnReset(); TestTrue(TEXT("Reset uses original map width"),FMath::IsNearlyEqual(Map->Width,Map->DefaultWidth));
  TestEqual(TEXT("Transform reset keeps imported image"),Map->ImagePath,Image);
  GI->Shutdown(); GI->RemoveFromRoot(); Panel->RemoveFromRoot();
  return true;
 }));
 return true;
}
#endif
