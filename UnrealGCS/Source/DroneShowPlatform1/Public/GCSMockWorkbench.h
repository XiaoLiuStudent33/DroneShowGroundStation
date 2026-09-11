#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GCSMockWorkbench.generated.h"

// V3 workbench: live fleet/view adapter; flight-command controls remain placeholders.
UCLASS()
class DRONESHOWPLATFORM1_API UGCSMockWorkbench : public UUserWidget
{
    GENERATED_BODY()
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
};
