#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateTypes.h"
#include "GCSLoginWidget.generated.h"

class SEditableTextBox;
UCLASS()
class DRONESHOWPLATFORM1_API UGCSLoginWidget : public UUserWidget
{
    GENERATED_BODY()
protected:
    virtual void NativeConstruct() override;
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
private:
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FGCSLoginSmoke;
#endif
    FButtonStyle LoginButtonStyle;
    FReply Submit();
    bool IsWorking() const;
    TSharedPtr<SEditableTextBox> UsernameBox, PasswordBox, ConfirmationBox;
    FString Status;
    bool bSetup = false;
    bool bLoading = false;
};
