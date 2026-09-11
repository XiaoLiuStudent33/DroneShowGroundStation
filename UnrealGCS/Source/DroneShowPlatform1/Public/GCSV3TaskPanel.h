#pragma once
#include "Blueprint/UserWidget.h"
#include "GCSV3TaskPanel.generated.h"

// Presentation-only tabs. Existing controls keep their UI Bridge bindings.
UCLASS()
class DRONESHOWPLATFORM1_API UGCSV3TaskPanel : public UUserWidget
{
    GENERATED_BODY()
protected:
    virtual void NativeConstruct() override;
private:
    friend struct FGCSLoginSmoke;
    void SelectPage(int32 Index);
};
