#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "GCSFleetRowCheckBinding.generated.h"

class UCheckBox;
class UButton;
class UGCSUIBridgeSubsystem;
class UUserWidget;

/**
 * Owns the dynamic checkbox delegate for one virtualized Fleet ListView entry.
 * The binding is released when Unreal recycles that entry for another item.
 */
UCLASS()
class UGCSFleetRowCheckBinding final : public UObject
{
	GENERATED_BODY()

public:
	void Initialize(
		UGCSUIBridgeSubsystem* InBridge,
		UObject* InRowObject,
		UCheckBox* InCheckBox,
		UButton* InDetailButton,
		UUserWidget* InEntryWidget);
	void RefreshFromRowObject(const FString& CurrentSelectedDroneID);
	UObject* GetRowObject() const { return RowObject; }
	void Shutdown();

private:
	UFUNCTION()
	void HandleCheckStateChanged(bool bIsChecked);

	UFUNCTION()
	void HandleDetailClicked();

	TWeakObjectPtr<UGCSUIBridgeSubsystem> Bridge;

	UPROPERTY(Transient)
	TObjectPtr<UObject> RowObject;

	UPROPERTY(Transient)
	TObjectPtr<UCheckBox> CheckBox;

	UPROPERTY(Transient)
	TObjectPtr<UButton> DetailButton;

	UPROPERTY(Transient)
	TObjectPtr<UUserWidget> EntryWidget;

	bool bApplyingState = false;
};
