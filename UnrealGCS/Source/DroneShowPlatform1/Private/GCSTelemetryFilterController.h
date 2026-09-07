#pragma once

#include "CoreMinimal.h"
#include "GCSTelemetryFilterModel.h"
#include "GCSTelemetryFilterPresetSaveGame.h"
#include "Components/ComboBoxString.h"
#include "GCSTelemetryFilterController.generated.h"

class UUserWidget;
class UListView;
class UCheckBox;
class UEditableTextBox;
class UTextBlock;
class UButton;
class UWidget;
class UGCSTelemetryFilterController;
class UGCSUIBridgeSubsystem;

UCLASS()
class UGCSTelemetryFilterItem : public UObject
{
	GENERATED_BODY()
public:
	GCSTelemetryFilter::FRule Rule;
	FString DroneID;
	FString DisplayValue;
	int32 MatchCount = 0;
	bool bSelected = false;
	TWeakObjectPtr<UGCSTelemetryFilterController> Controller;
	UFUNCTION() void OnEnabledChanged(bool bEnabled);
	UFUNCTION() void OnSelectedChanged(bool bChecked);
};

// Binds the user's existing widget assets, without reparenting or modifying them.
// Selection uses the bridge's guarded state API only; never submits commands.
UCLASS()
class UGCSTelemetryFilterController : public UObject
{
	GENERATED_BODY()
public:
	bool Bind(UUserWidget* InWidget, UGCSUIBridgeSubsystem* InSelectionBridge = nullptr);
	void Shutdown();
	void UpdateTelemetry(const TArray<FMDHKTelemetryTableRow>& InRows,
		const TMap<FString, bool>& InOnline, bool bConnected, int64 NowUtcMs, int32 TimeoutMs);
	void RuleEnabledChanged();
	void ResultSelectionChanged(UGCSTelemetryFilterItem* Item, bool bChecked);
	UUserWidget* GetWidget() const { return Widget; }

private:
	friend class FGCSTelemetryFilterWidgetTest;
	friend class FGCSTelemetryFilterSelectionTest;
	friend class FGCSTelemetryFilterModesTest;
	friend class FGCSTelemetryFilterPresetTest;
	void AppendCondition(const GCSTelemetryFilter::FRule& Rule);
	UFUNCTION() void SelectAllChanged(bool bChecked);
	void RefreshSelection();
	UFUNCTION() void AddCondition();
	UFUNCTION() void RemoveCondition();
	UFUNCTION() void ApplyCondition();
	UFUNCTION() void ClearConditions();
	UFUNCTION() void MatchModeChanged(FString Value, ESelectInfo::Type Info);
	UFUNCTION() void FieldChanged(FString Value, ESelectInfo::Type Info);
	UFUNCTION() void PresetChanged(FString SelectedPreset, ESelectInfo::Type Info);
	UFUNCTION() void NewPreset();
	UFUNCTION() void SavePreset();
	UFUNCTION() void DeletePreset();
	UFUNCTION() void ConfirmPreset();
	UFUNCTION() void CancelPreset();
	UFUNCTION() void PresetNameCommitted(const FText& Text, ETextCommit::Type CommitMethod);
	void SelectionChanged(UObject* Object);
	void ConditionGenerated(UUserWidget& Entry);
	void ResultGenerated(UUserWidget& Entry);
	void ConditionReleased(UUserWidget& Entry);
	void ResultReleased(UUserWidget& Entry);
	void PaintCondition(UUserWidget& Entry);
	void PaintResult(UUserWidget& Entry);
	void RefreshResults();
	void RefreshConditions();
	void SetError(const FString& Error);
	void SetPresetError(const FString& Error);
	void SetPresetEditorVisible(bool bVisible);
	void LoadPresets();
	bool PersistPresets();
	void RebuildPresetOptions(const FString& Selection);
	void UpdatePresetButtons();
	void ApplyPreset(const FGCSTelemetryFilterPresetRecord& Preset);
	FGCSTelemetryFilterPresetRecord CapturePreset(const FString& Name) const;
	int32 FindPreset(const FString& Name) const;
	void MarkPresetDirty();
	GCSTelemetryFilter::FRule Draft() const;
	FName EditorField() const;

	UPROPERTY(Transient) TObjectPtr<UUserWidget> Widget;
	UPROPERTY(Transient) TObjectPtr<UListView> Conditions;
	UPROPERTY(Transient) TObjectPtr<UListView> Results;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> MatchMode;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> Field;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> Operator;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> Value;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ErrorText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> CountText;
	UPROPERTY(Transient) TObjectPtr<UCheckBox> SelectAll;
	UPROPERTY(Transient) TObjectPtr<UComboBoxString> PresetCombo;
	UPROPERTY(Transient) TObjectPtr<UEditableTextBox> PresetName;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> PresetErrorText;
	UPROPERTY(Transient) TObjectPtr<UButton> NewPresetButton;
	UPROPERTY(Transient) TObjectPtr<UButton> SavePresetButton;
	UPROPERTY(Transient) TObjectPtr<UButton> DeletePresetButton;
	UPROPERTY(Transient) TObjectPtr<UButton> ConfirmPresetButton;
	UPROPERTY(Transient) TObjectPtr<UButton> CancelPresetButton;
	UPROPERTY(Transient) TObjectPtr<UWidget> PresetNameEditor;
	TWeakObjectPtr<UGCSUIBridgeSubsystem> SelectionBridge;
	UPROPERTY(Transient) TArray<TObjectPtr<UGCSTelemetryFilterItem>> Rules;
	UPROPERTY(Transient) TMap<FString, TObjectPtr<UGCSTelemetryFilterItem>> ResultItems;
	TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UGCSTelemetryFilterItem>> EntryBindings;
	TMap<TWeakObjectPtr<UUserWidget>, TWeakObjectPtr<UGCSTelemetryFilterItem>> ResultEntryBindings;
	TArray<FMDHKTelemetryTableRow> Rows;
	TMap<FString, bool> Online;
	TSet<FString> FreshIDs;
	TArray<FGCSTelemetryFilterPresetRecord> Presets;
	FString ActivePresetName;
	FString PresetSlotName = TEXT("GCSTelemetryFilterPresets");
	bool bUpdating = false;
	bool bApplyingPreset = false;
	bool bPresetDirty = false;
	bool bLinkConnected = false;
	bool bPaintingSelection = false;
};
