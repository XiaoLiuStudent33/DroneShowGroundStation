#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateTypes.h"
#include "GCSLoginWidget.generated.h"

class SEditableTextBox;
struct FGCSLoginPresentation;
class UTexture2D;
class UGCSLoginMedia;
UCLASS()
class DRONESHOWPLATFORM1_API UGCSLoginWidget : public UUserWidget
{
    GENERATED_BODY()
protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FGCSLoginSmoke;
    friend struct FGCSLoginAccountSmoke;
    friend struct FGCSLoginVisualCheck;
    friend struct FGCSLoginMediaCheck;
#endif
    FButtonStyle LoginButtonStyle;
    FButtonStyle SecondaryStyle;
    FEditableTextBoxStyle FieldStyle;
    FCheckBoxStyle RememberStyle;
    FSlateBrush CardBrush;
    FSlateBrush BackgroundBrush;
    TArray<FSlateBrush> ThemeBrushes;
    UPROPERTY(Transient) TArray<TObjectPtr<UTexture2D>> ThemeTextures;
    UPROPERTY(Transient) TObjectPtr<UTexture2D> BackgroundTexture;
    UPROPERTY(Transient) TObjectPtr<UGCSLoginMedia> LoginMedia;
    TSharedPtr<FGCSLoginPresentation> Presentation;
    void SetLanguage(int32 Language);
    void SavePreferences();
    FReply Submit();
    void SwitchAccountMode(int32 Mode);
    int32 AccountMode=0;
    FString RecoveryCode;
    TSharedPtr<SEditableTextBox> RecoveryBox;
    bool IsWorking() const;
    TSharedPtr<SEditableTextBox> UsernameBox, PasswordBox, ConfirmationBox;
    FString Status;
    bool bSetup = false;
    bool bLoading = false;
};
