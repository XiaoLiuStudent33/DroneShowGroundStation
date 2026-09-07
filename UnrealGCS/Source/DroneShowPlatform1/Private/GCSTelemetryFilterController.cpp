#include "GCSTelemetryFilterController.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/EditableTextBox.h"
#include "Components/ListView.h"
#include "Components/TextBlock.h"
#include "Kismet/GameplayStatics.h"
#include "GCSUIBridgeSubsystem.h"

using namespace GCSTelemetryFilter;
namespace
{
constexpr int32 MaxPresetCount = 64;
constexpr int32 MaxPresetNameLength = 32;
const FString TemporaryPresetName = TEXT("临时筛选");

FRule BlankCondition()
{
	FRule Rule;
	Rule.Field = NAME_None;
	Rule.Operator = TEXT("--");
	Rule.Input.Empty();
	return Rule;
}
bool IsBlankCondition(const FRule& Rule)
{
	return Rule.Field.IsNone() && (Rule.Operator.IsEmpty() || Rule.Operator == TEXT("--"))
		&& (Rule.Input.IsEmpty() || Rule.Input == TEXT("--"));
}
template<typename T> T* FindFilterWidget(UUserWidget* Widget, const TCHAR* Name)
{
	return Widget && Widget->WidgetTree ? Cast<T>(Widget->WidgetTree->FindWidget(Name)) : nullptr;
}
void FilterText(UUserWidget& Entry, const TCHAR* Name, const FString& Text)
{
	if (UTextBlock* Block = FindFilterWidget<UTextBlock>(&Entry, Name))
	{
		Block->SetText(FText::FromString(Text));
		Block->SetToolTipText(FText::FromString(Text));
	}
}
}

void UGCSTelemetryFilterItem::OnEnabledChanged(bool bEnabled)
{
	if (Rule.bEnabled == bEnabled) return;
	Rule.bEnabled = bEnabled;
	if (Controller.IsValid()) Controller->RuleEnabledChanged();
}

void UGCSTelemetryFilterItem::OnSelectedChanged(bool bChecked)
{
	if (Controller.IsValid()) Controller->ResultSelectionChanged(this, bChecked);
}

bool UGCSTelemetryFilterController::Bind(UUserWidget* InWidget, UGCSUIBridgeSubsystem* InSelectionBridge)
{
	Shutdown();
	Widget = InWidget;
	Conditions = FindFilterWidget<UListView>(Widget, TEXT("LV_FilterConditions"));
	Results = FindFilterWidget<UListView>(Widget, TEXT("LV_FilterResults"));
	MatchMode = FindFilterWidget<UComboBoxString>(Widget, TEXT("CMB_MatchMode"));
	Field = FindFilterWidget<UComboBoxString>(Widget, TEXT("CMB_FilterField"));
	Operator = FindFilterWidget<UComboBoxString>(Widget, TEXT("CMB_Operator"));
	Value = FindFilterWidget<UEditableTextBox>(Widget, TEXT("EDT_FilterValue"));
	ErrorText = FindFilterWidget<UTextBlock>(Widget, TEXT("TXT_FilterError"));
	CountText = FindFilterWidget<UTextBlock>(Widget, TEXT("TXT_ResultCount"));
	SelectAll = FindFilterWidget<UCheckBox>(Widget, TEXT("CHK_SelectAllResults"));
	PresetCombo = FindFilterWidget<UComboBoxString>(Widget, TEXT("CMB_FilterPreset"));
	PresetName = FindFilterWidget<UEditableTextBox>(Widget, TEXT("EDT_PresetName"));
	PresetErrorText = FindFilterWidget<UTextBlock>(Widget, TEXT("TXT_PresetError"));
	PresetNameEditor = FindFilterWidget<UWidget>(Widget, TEXT("HBOX_PresetNameEditor"));
	NewPresetButton = FindFilterWidget<UButton>(Widget, TEXT("BTN_NewPreset"));
	SavePresetButton = FindFilterWidget<UButton>(Widget, TEXT("BTN_SavePreset"));
	DeletePresetButton = FindFilterWidget<UButton>(Widget, TEXT("BTN_DeletePreset"));
	ConfirmPresetButton = FindFilterWidget<UButton>(Widget, TEXT("BTN_ConfirmPreset"));
	CancelPresetButton = FindFilterWidget<UButton>(Widget, TEXT("BTN_CancelPreset"));
	UButton* Add = FindFilterWidget<UButton>(Widget, TEXT("BTN_AddCondition"));
	UButton* Remove = FindFilterWidget<UButton>(Widget, TEXT("BTN_RemoveCondition"));
	UButton* Apply = FindFilterWidget<UButton>(Widget, TEXT("BTN_ApplyCondition"));
	UButton* Clear = FindFilterWidget<UButton>(Widget, TEXT("BTN_ClearConditions"));
	if (!Conditions || !Results || !MatchMode || !Field || !Operator || !Value || !ErrorText || !CountText
		|| !Add || !Remove || !Apply || !Clear || !SelectAll || !PresetCombo || !PresetName
		|| !PresetErrorText || !PresetNameEditor || !NewPresetButton || !SavePresetButton
		|| !DeletePresetButton || !ConfirmPresetButton || !CancelPresetButton)
	{
		UE_LOG(LogTemp, Error, TEXT("TelemetryFilter: widget names/types do not match the filter contract."));
		Shutdown();
		return false;
	}
	Conditions->SetSelectionMode(ESelectionMode::Single);
	Results->SetSelectionMode(ESelectionMode::None);
	Conditions->ClearListItems();
	Results->ClearListItems();
	Conditions->OnEntryWidgetGenerated().AddUObject(this, &ThisClass::ConditionGenerated);
	Conditions->OnEntryWidgetReleased().AddUObject(this, &ThisClass::ConditionReleased);
	Conditions->OnItemSelectionChanged().AddUObject(this, &ThisClass::SelectionChanged);
	Results->OnEntryWidgetGenerated().AddUObject(this, &ThisClass::ResultGenerated);
	Results->OnEntryWidgetReleased().AddUObject(this, &ThisClass::ResultReleased);
	SelectionBridge = InSelectionBridge;
	if (SelectionBridge.IsValid())
		SelectionBridge->OnCommandSelectionChanged.AddUObject(this, &ThisClass::RefreshSelection);
	SelectAll->OnCheckStateChanged.AddUniqueDynamic(this, &ThisClass::SelectAllChanged);
	Add->OnClicked.AddDynamic(this, &ThisClass::AddCondition);
	Remove->OnClicked.AddDynamic(this, &ThisClass::RemoveCondition);
	Apply->OnClicked.AddDynamic(this, &ThisClass::ApplyCondition);
	Clear->OnClicked.AddDynamic(this, &ThisClass::ClearConditions);
	MatchMode->OnSelectionChanged.AddDynamic(this, &ThisClass::MatchModeChanged);
	Field->OnSelectionChanged.AddDynamic(this, &ThisClass::FieldChanged);
	PresetCombo->OnSelectionChanged.AddDynamic(this, &ThisClass::PresetChanged);
	NewPresetButton->OnClicked.AddDynamic(this, &ThisClass::NewPreset);
	SavePresetButton->OnClicked.AddDynamic(this, &ThisClass::SavePreset);
	DeletePresetButton->OnClicked.AddDynamic(this, &ThisClass::DeletePreset);
	ConfirmPresetButton->OnClicked.AddDynamic(this, &ThisClass::ConfirmPreset);
	CancelPresetButton->OnClicked.AddDynamic(this, &ThisClass::CancelPreset);
	PresetName->OnTextCommitted.AddDynamic(this, &ThisClass::PresetNameCommitted);
	bUpdating = true;
	MatchMode->ClearOptions();
	MatchMode->AddOption(TEXT("当前条件"));
	MatchMode->AddOption(TEXT("任一满足"));
	MatchMode->AddOption(TEXT("全部满足"));
	MatchMode->SetSelectedIndex(0);
	Field->ClearOptions();
	Field->AddOption(TEXT("--"));
	for (const GCSTelemetryFilter::FField& F : Fields()) Field->AddOption(F.Label);
	Field->SetSelectedIndex(0);
	bUpdating = false;
	LoadPresets();
	ActivePresetName.Empty();
	RebuildPresetOptions(TemporaryPresetName);
	SetPresetEditorVisible(false);
	SetPresetError(TEXT(""));
	UpdatePresetButtons();
	FieldChanged(Field->GetSelectedOption(), ESelectInfo::Direct);
	SetError(TEXT(""));
	RefreshResults();
	return true;
}

void UGCSTelemetryFilterController::Shutdown()
{
	if (SelectionBridge.IsValid()) SelectionBridge->OnCommandSelectionChanged.RemoveAll(this);
	SelectionBridge.Reset();
	if (SelectAll) SelectAll->OnCheckStateChanged.RemoveDynamic(this, &ThisClass::SelectAllChanged);
	for (const auto& Binding : ResultEntryBindings)
	{
		if (UCheckBox* Check = FindFilterWidget<UCheckBox>(Binding.Key.Get(), TEXT("CHK_ResultSelected")))
			if (Binding.Value.IsValid()) Check->OnCheckStateChanged.RemoveDynamic(Binding.Value.Get(), &UGCSTelemetryFilterItem::OnSelectedChanged);
	}
	ResultEntryBindings.Reset();
	for (const auto& Binding : EntryBindings)
	{
		if (UCheckBox* Check = FindFilterWidget<UCheckBox>(Binding.Key.Get(), TEXT("CHK_ConditionEnabled")))
			if (Binding.Value.IsValid()) Check->OnCheckStateChanged.RemoveDynamic(Binding.Value.Get(), &UGCSTelemetryFilterItem::OnEnabledChanged);
	}
	EntryBindings.Reset();
	if (Conditions)
	{
		Conditions->OnEntryWidgetGenerated().RemoveAll(this);
		Conditions->OnEntryWidgetReleased().RemoveAll(this);
		Conditions->OnItemSelectionChanged().RemoveAll(this);
		Conditions->ClearListItems();
	}
	if (Results)
	{
		Results->OnEntryWidgetGenerated().RemoveAll(this);
		Results->OnEntryWidgetReleased().RemoveAll(this);
		Results->ClearListItems();
	}
	if (MatchMode) MatchMode->OnSelectionChanged.RemoveDynamic(this, &ThisClass::MatchModeChanged);
	if (Field) Field->OnSelectionChanged.RemoveDynamic(this, &ThisClass::FieldChanged);
	if (PresetCombo) PresetCombo->OnSelectionChanged.RemoveDynamic(this, &ThisClass::PresetChanged);
	if (PresetName) PresetName->OnTextCommitted.RemoveDynamic(this, &ThisClass::PresetNameCommitted);
	if (NewPresetButton) NewPresetButton->OnClicked.RemoveDynamic(this, &ThisClass::NewPreset);
	if (SavePresetButton) SavePresetButton->OnClicked.RemoveDynamic(this, &ThisClass::SavePreset);
	if (DeletePresetButton) DeletePresetButton->OnClicked.RemoveDynamic(this, &ThisClass::DeletePreset);
	if (ConfirmPresetButton) ConfirmPresetButton->OnClicked.RemoveDynamic(this, &ThisClass::ConfirmPreset);
	if (CancelPresetButton) CancelPresetButton->OnClicked.RemoveDynamic(this, &ThisClass::CancelPreset);
	if (UButton* B = FindFilterWidget<UButton>(Widget, TEXT("BTN_AddCondition"))) B->OnClicked.RemoveDynamic(this, &ThisClass::AddCondition);
	if (UButton* B = FindFilterWidget<UButton>(Widget, TEXT("BTN_RemoveCondition"))) B->OnClicked.RemoveDynamic(this, &ThisClass::RemoveCondition);
	if (UButton* B = FindFilterWidget<UButton>(Widget, TEXT("BTN_ApplyCondition"))) B->OnClicked.RemoveDynamic(this, &ThisClass::ApplyCondition);
	if (UButton* B = FindFilterWidget<UButton>(Widget, TEXT("BTN_ClearConditions"))) B->OnClicked.RemoveDynamic(this, &ThisClass::ClearConditions);
	Rules.Reset(); ResultItems.Reset(); Rows.Reset(); Online.Reset(); FreshIDs.Reset();
	Widget = nullptr; Conditions = nullptr; Results = nullptr; MatchMode = nullptr;
	Field = nullptr; Operator = nullptr; Value = nullptr; ErrorText = nullptr; CountText = nullptr;
	SelectAll = nullptr; PresetCombo = nullptr; PresetName = nullptr; PresetErrorText = nullptr;
	NewPresetButton = nullptr; SavePresetButton = nullptr; DeletePresetButton = nullptr;
	ConfirmPresetButton = nullptr; CancelPresetButton = nullptr; PresetNameEditor = nullptr;
	Presets.Reset(); ActivePresetName.Empty();
	bPaintingSelection = false;
	bUpdating = false; bApplyingPreset = false; bPresetDirty = false; bLinkConnected = false;
}

FName UGCSTelemetryFilterController::EditorField() const
{
	if (Field)
		for (const GCSTelemetryFilter::FField& F : Fields()) if (F.Label == Field->GetSelectedOption()) return F.Key;
	return NAME_None;
}
FRule UGCSTelemetryFilterController::Draft() const
{
	FRule Rule;
	Rule.Field = EditorField();
	Rule.Operator = Operator->GetSelectedOption();
	Rule.Input = Value->GetText().ToString().TrimStartAndEnd();
	return Rule;
}
void UGCSTelemetryFilterController::SetError(const FString& Error)
{
	if (!ErrorText) return;
	ErrorText->SetText(FText::FromString(Error));
	ErrorText->SetVisibility(Error.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}

void UGCSTelemetryFilterController::SetPresetError(const FString& Error)
{
	if (!PresetErrorText) return;
	PresetErrorText->SetText(FText::FromString(Error));
	PresetErrorText->SetVisibility(Error.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}

void UGCSTelemetryFilterController::SetPresetEditorVisible(bool bVisible)
{
	if (PresetNameEditor)
		PresetNameEditor->SetVisibility(bVisible ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
}

int32 UGCSTelemetryFilterController::FindPreset(const FString& Name) const
{
	return Presets.IndexOfByPredicate([&Name](const FGCSTelemetryFilterPresetRecord& Preset)
	{
		return Preset.Name.Equals(Name, ESearchCase::IgnoreCase);
	});
}

void UGCSTelemetryFilterController::LoadPresets()
{
	Presets.Reset();
	if (!UGameplayStatics::DoesSaveGameExist(PresetSlotName, 0)) return;
	const auto* Save = Cast<UGCSTelemetryFilterPresetSaveGame>(UGameplayStatics::LoadGameFromSlot(PresetSlotName, 0));
	if (!Save)
	{
		UE_LOG(LogTemp, Warning, TEXT("TelemetryFilter: preset save could not be loaded."));
		return;
	}
	for (const FGCSTelemetryFilterPresetRecord& Stored : Save->Presets)
	{
		if (Presets.Num() >= MaxPresetCount) break;
		FGCSTelemetryFilterPresetRecord Preset;
		Preset.Name = Stored.Name.TrimStartAndEnd();
		if (Preset.Name.IsEmpty() || Preset.Name.Len() > MaxPresetNameLength
			|| Preset.Name.Equals(TemporaryPresetName, ESearchCase::IgnoreCase)
			|| FindPreset(Preset.Name) != INDEX_NONE)
			continue;
		Preset.MatchModeIndex = FMath::Clamp(Stored.MatchModeIndex, 0, 2);
		for (const FGCSTelemetryFilterPresetRule& StoredRule : Stored.Rules)
		{
			if (Preset.Rules.Num() >= 128) break;
			FRule Rule;
			Rule.Field = StoredRule.Field;
			Rule.Operator = StoredRule.Operator;
			Rule.Input = StoredRule.Input;
			Rule.bEnabled = StoredRule.bEnabled;
			FString Error;
			if (!IsBlankCondition(Rule) && !Validate(Rule, Error)) Rule = BlankCondition();
			FGCSTelemetryFilterPresetRule SavedRule;
			SavedRule.Field = Rule.Field;
			SavedRule.Operator = Rule.Operator;
			SavedRule.Input = Rule.Input;
			SavedRule.bEnabled = Rule.bEnabled;
			Preset.Rules.Add(MoveTemp(SavedRule));
		}
		Preset.SelectedConditionIndex = Stored.SelectedConditionIndex >= 0
			&& Stored.SelectedConditionIndex < Preset.Rules.Num() ? Stored.SelectedConditionIndex : INDEX_NONE;
		Presets.Add(MoveTemp(Preset));
	}
}

bool UGCSTelemetryFilterController::PersistPresets()
{
	auto* Save = Cast<UGCSTelemetryFilterPresetSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UGCSTelemetryFilterPresetSaveGame::StaticClass()));
	if (!Save) return false;
	Save->Presets = Presets;
	return UGameplayStatics::SaveGameToSlot(Save, PresetSlotName, 0);
}

void UGCSTelemetryFilterController::RebuildPresetOptions(const FString& Selection)
{
	if (!PresetCombo) return;
	TGuardValue<bool> Updating(bUpdating, true);
	PresetCombo->ClearOptions();
	PresetCombo->AddOption(TemporaryPresetName);
	for (const FGCSTelemetryFilterPresetRecord& Preset : Presets) PresetCombo->AddOption(Preset.Name);
	PresetCombo->SetSelectedOption(FindPreset(Selection) != INDEX_NONE ? Selection : TemporaryPresetName);
}

void UGCSTelemetryFilterController::UpdatePresetButtons()
{
	const bool bHasActivePreset = FindPreset(ActivePresetName) != INDEX_NONE;
	if (SavePresetButton)
	{
		SavePresetButton->SetIsEnabled(bHasActivePreset);
		SavePresetButton->SetToolTipText(FText::FromString(bHasActivePreset
			? FString::Printf(TEXT("保存当前筛选配置到“%s”%s"), *ActivePresetName, bPresetDirty ? TEXT("（有未保存修改）") : TEXT(""))
			: TEXT("请先选择或新建一个预设")));
	}
	if (DeletePresetButton)
	{
		DeletePresetButton->SetIsEnabled(bHasActivePreset);
		DeletePresetButton->SetToolTipText(FText::FromString(bHasActivePreset
			? FString::Printf(TEXT("删除本地预设“%s”"), *ActivePresetName)
			: TEXT("临时筛选不能删除")));
	}
	if (NewPresetButton) NewPresetButton->SetToolTipText(FText::FromString(TEXT("将当前筛选配置保存为新预设")));
}

FGCSTelemetryFilterPresetRecord UGCSTelemetryFilterController::CapturePreset(const FString& Name) const
{
	FGCSTelemetryFilterPresetRecord Preset;
	Preset.Name = Name;
	Preset.MatchModeIndex = MatchMode ? FMath::Clamp(MatchMode->GetSelectedIndex(), 0, 2) : 0;
	const auto* Selected = Conditions ? Cast<UGCSTelemetryFilterItem>(Conditions->GetSelectedItem()) : nullptr;
	for (int32 Index = 0; Index < Rules.Num(); ++Index)
	{
		const UGCSTelemetryFilterItem* Item = Rules[Index];
		if (!Item) continue;
		if (Item == Selected) Preset.SelectedConditionIndex = Preset.Rules.Num();
		FGCSTelemetryFilterPresetRule Rule;
		Rule.Field = Item->Rule.Field;
		Rule.Operator = Item->Rule.Operator;
		Rule.Input = Item->Rule.Input;
		Rule.bEnabled = Item->Rule.bEnabled;
		Preset.Rules.Add(MoveTemp(Rule));
	}
	return Preset;
}

void UGCSTelemetryFilterController::ApplyPreset(const FGCSTelemetryFilterPresetRecord& Preset)
{
	UGCSTelemetryFilterItem* Selected = nullptr;
	{
		TGuardValue<bool> Updating(bUpdating, true);
		Conditions->ClearSelection();
		Conditions->ClearListItems();
		Rules.Reset();
		MatchMode->SetSelectedIndex(FMath::Clamp(Preset.MatchModeIndex, 0, 2));
		for (int32 Index = 0; Index < Preset.Rules.Num() && Index < 128; ++Index)
		{
			const FGCSTelemetryFilterPresetRule& SavedRule = Preset.Rules[Index];
			auto* Item = NewObject<UGCSTelemetryFilterItem>(this);
			Item->Rule.Field = SavedRule.Field;
			Item->Rule.Operator = SavedRule.Operator;
			Item->Rule.Input = SavedRule.Input;
			Item->Rule.bEnabled = SavedRule.bEnabled;
			Item->Controller = this;
			Rules.Add(Item);
			Conditions->AddItem(Item);
			if (Index == Preset.SelectedConditionIndex) Selected = Item;
		}
		if (Selected) Conditions->SetSelectedItem(Selected);
	}
	TGuardValue<bool> Applying(bApplyingPreset, true);
	SelectionChanged(Selected);
	RefreshConditions();
}

void UGCSTelemetryFilterController::MarkPresetDirty()
{
	if (bUpdating || bApplyingPreset || FindPreset(ActivePresetName) == INDEX_NONE) return;
	bPresetDirty = true;
	UpdatePresetButtons();
}

void UGCSTelemetryFilterController::PresetChanged(FString SelectedPreset, ESelectInfo::Type)
{
	if (bUpdating) return;
	SetPresetError(TEXT(""));
	const int32 Index = FindPreset(SelectedPreset);
	if (Index == INDEX_NONE)
	{
		ActivePresetName.Empty();
		bPresetDirty = false;
		UpdatePresetButtons();
		return;
	}
	ActivePresetName = Presets[Index].Name;
	bPresetDirty = false;
	ApplyPreset(Presets[Index]);
	UpdatePresetButtons();
}

void UGCSTelemetryFilterController::NewPreset()
{
	SetPresetError(TEXT(""));
	PresetName->SetText(FText::GetEmpty());
	SetPresetEditorVisible(true);
	PresetName->SetKeyboardFocus();
}

void UGCSTelemetryFilterController::ConfirmPreset()
{
	FString Name = PresetName->GetText().ToString().TrimStartAndEnd();
	if (Name.IsEmpty()) { SetPresetError(TEXT("请输入预设名称")); return; }
	if (Name.Len() > MaxPresetNameLength) { SetPresetError(TEXT("预设名称最多 32 个字符")); return; }
	if (Name.Equals(TemporaryPresetName, ESearchCase::IgnoreCase))
	{
		SetPresetError(TEXT("“临时筛选”是保留名称"));
		return;
	}
	if (FindPreset(Name) != INDEX_NONE) { SetPresetError(TEXT("该预设名称已经存在")); return; }
	if (Presets.Num() >= MaxPresetCount) { SetPresetError(TEXT("最多保存 64 个筛选预设")); return; }
	Presets.Add(CapturePreset(Name));
	if (!PersistPresets())
	{
		Presets.Pop();
		SetPresetError(TEXT("预设保存失败，请检查本地存储权限"));
		return;
	}
	ActivePresetName = Name;
	bPresetDirty = false;
	RebuildPresetOptions(Name);
	PresetName->SetText(FText::GetEmpty());
	SetPresetEditorVisible(false);
	SetPresetError(TEXT(""));
	UpdatePresetButtons();
}

void UGCSTelemetryFilterController::CancelPreset()
{
	PresetName->SetText(FText::GetEmpty());
	SetPresetEditorVisible(false);
	SetPresetError(TEXT(""));
}

void UGCSTelemetryFilterController::PresetNameCommitted(const FText&, ETextCommit::Type CommitMethod)
{
	if (CommitMethod == ETextCommit::OnEnter) ConfirmPreset();
}

void UGCSTelemetryFilterController::SavePreset()
{
	const int32 Index = FindPreset(ActivePresetName);
	if (Index == INDEX_NONE) { SetPresetError(TEXT("请先选择或新建一个预设")); return; }
	const FGCSTelemetryFilterPresetRecord Previous = Presets[Index];
	Presets[Index] = CapturePreset(Previous.Name);
	if (!PersistPresets())
	{
		Presets[Index] = Previous;
		SetPresetError(TEXT("预设保存失败，请检查本地存储权限"));
		return;
	}
	bPresetDirty = false;
	SetPresetError(TEXT(""));
	UpdatePresetButtons();
}

void UGCSTelemetryFilterController::DeletePreset()
{
	const int32 Index = FindPreset(ActivePresetName);
	if (Index == INDEX_NONE) return;
	const FGCSTelemetryFilterPresetRecord Removed = Presets[Index];
	Presets.RemoveAt(Index);
	if (!PersistPresets())
	{
		Presets.Insert(Removed, Index);
		SetPresetError(TEXT("预设删除失败，请检查本地存储权限"));
		return;
	}
	ActivePresetName.Empty();
	bPresetDirty = false;
	RebuildPresetOptions(TemporaryPresetName);
	SetPresetEditorVisible(false);
	SetPresetError(TEXT(""));
	UpdatePresetButtons();
}
void UGCSTelemetryFilterController::AddCondition()
{
	// New always creates a separate blank row, never a copy of the editor.
	AppendCondition(BlankCondition());
}
void UGCSTelemetryFilterController::AppendCondition(const FRule& Rule)
{
	if (Rules.Num() >= 128) { SetError(TEXT("最多 128 条条件")); return; }
	UGCSTelemetryFilterItem* Item = NewObject<UGCSTelemetryFilterItem>(this);
	Item->Rule = Rule;
	Item->Controller = this;
	Rules.Add(Item);
	Conditions->AddItem(Item);
	Conditions->SetSelectedItem(Item);
	SetError(TEXT(""));
	MarkPresetDirty();
	RefreshResults();
}
void UGCSTelemetryFilterController::RemoveCondition()
{
	UGCSTelemetryFilterItem* Item = Cast<UGCSTelemetryFilterItem>(Conditions->GetSelectedItem());
	if (!Item || !Rules.Contains(Item)) { SetError(TEXT("请先选中上方的一条条件")); return; }
	{
		TGuardValue<bool> Updating(bUpdating, true);
		Conditions->ClearSelection();
		Rules.Remove(Item);
		Conditions->RemoveItem(Item);
	}
	SelectionChanged(nullptr);
	SetError(TEXT(""));
	MarkPresetDirty();
	RefreshResults();
}
void UGCSTelemetryFilterController::ApplyCondition()
{
	UGCSTelemetryFilterItem* Item = Cast<UGCSTelemetryFilterItem>(Conditions->GetSelectedItem());
	if (Item && !Rules.Contains(Item)) Item = nullptr;
	FRule Rule = Draft();
	FString Error;
	if (IsBlankCondition(Rule)) Rule = BlankCondition();
	else if (!Validate(Rule, Error)) { SetError(Error); return; }
	if (!Item) { AppendCondition(Rule); return; }
	Rule.bEnabled = Item->Rule.bEnabled;
	Item->Rule = Rule;
	SetError(TEXT(""));
	MarkPresetDirty();
	RefreshConditions();
	RefreshResults();
}
void UGCSTelemetryFilterController::ClearConditions()
{
	{
		TGuardValue<bool> Updating(bUpdating, true);
		// ClearListItems only requests a deferred Slate refresh; selection must
		// be cleared now so Apply cannot write into a removed, invisible item.
		Conditions->ClearSelection();
		Conditions->ClearListItems();
		Rules.Reset();
	}
	SelectionChanged(nullptr);
	SetError(TEXT(""));
	MarkPresetDirty();
	RefreshResults();
}
void UGCSTelemetryFilterController::MatchModeChanged(FString, ESelectInfo::Type)
{
	if (!bUpdating)
	{
		MarkPresetDirty();
		RefreshResults();
	}
}
void UGCSTelemetryFilterController::FieldChanged(FString, ESelectInfo::Type)
{
	if (bUpdating) return;
	const GCSTelemetryFilter::FField* Selected = FindField(EditorField());
	Operator->SetIsEnabled(Selected != nullptr);
	Value->SetIsEnabled(Selected != nullptr);
	if (!Selected)
	{
		Operator->ClearOptions();
		Operator->AddOption(TEXT("--"));
		Operator->SetSelectedIndex(0);
		Value->SetText(FText::GetEmpty());
		Value->SetHintText(FText::FromString(TEXT("--")));
		SetError(TEXT(""));
		return;
	}
	const FString Previous = Operator->GetSelectedOption();
	Operator->ClearOptions();
	const TArray<FString> Allowed = Operators(Selected->Kind);
	for (const FString& Op : Allowed) Operator->AddOption(Op);
	Operator->SetSelectedOption(Allowed.Contains(Previous) ? Previous : Allowed[0]);
	Value->SetHintText(FText::FromString(Selected->Kind == EKind::Online ? TEXT("在线 / 离线")
		: Selected->Kind == EKind::Text ? TEXT("输入文本") : TEXT("输入数字，不含单位")));
	SetError(TEXT(""));
}
void UGCSTelemetryFilterController::SelectionChanged(UObject* Object)
{
	if (bUpdating) return;
	const auto* Item = Cast<UGCSTelemetryFilterItem>(Object);
	if (Item && !Rules.Contains(Item)) Item = nullptr;
	const GCSTelemetryFilter::FField* Selected = Item ? FindField(Item->Rule.Field) : nullptr;
	bUpdating = true;
	Field->SetSelectedOption(Selected ? Selected->Label : TEXT("--"));
	bUpdating = false;
	FieldChanged(Field->GetSelectedOption(), ESelectInfo::Direct);
	if (Item && Selected)
	{
		Operator->SetSelectedOption(Item->Rule.Operator);
		Value->SetText(FText::FromString(Item->Rule.Input));
	}
	SetError(TEXT(""));
	MarkPresetDirty();
	RefreshResults();
}
void UGCSTelemetryFilterController::ConditionGenerated(UUserWidget& Entry)
{
	ConditionReleased(Entry);
	if (auto* Item = Cast<UGCSTelemetryFilterItem>(Conditions->GetListObjectFromEntry(Entry)))
	{
		EntryBindings.Add(&Entry, Item);
		if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ConditionEnabled")))
			Check->OnCheckStateChanged.AddUniqueDynamic(Item, &UGCSTelemetryFilterItem::OnEnabledChanged);
	}
	PaintCondition(Entry);
}
void UGCSTelemetryFilterController::ConditionReleased(UUserWidget& Entry)
{
	if (auto* Binding = EntryBindings.Find(&Entry))
	{
		if (Binding->IsValid())
			if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ConditionEnabled")))
				Check->OnCheckStateChanged.RemoveDynamic(Binding->Get(), &UGCSTelemetryFilterItem::OnEnabledChanged);
		EntryBindings.Remove(&Entry);
	}
}
void UGCSTelemetryFilterController::PaintCondition(UUserWidget& Entry)
{
	const auto* Item = Cast<UGCSTelemetryFilterItem>(Conditions->GetListObjectFromEntry(Entry));
	if (!Item) return;
	const GCSTelemetryFilter::FField* F = FindField(Item->Rule.Field);
	FilterText(Entry, TEXT("TXT_ConditionField"), F ? F->Label : TEXT("--"));
	FilterText(Entry, TEXT("TXT_ConditionOperator"), Item->Rule.Operator);
	FilterText(Entry, TEXT("TXT_ConditionValue"), IsBlankCondition(Item->Rule) ? TEXT("--") : Item->Rule.Input);
	FilterText(Entry, TEXT("TXT_ConditionMatchCount"), FString::FromInt(Item->MatchCount));
	if (UTextBlock* Count = FindFilterWidget<UTextBlock>(&Entry, TEXT("TXT_ConditionMatchCount")))
		Count->SetToolTipText(FText::FromString(TEXT("单独满足此条件的无人机数量；停用条件仍显示独立计数，缺失或过期值不参与比较。")));
	if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ConditionEnabled")))
	{
		Check->SetIsChecked(Item->Rule.bEnabled);
		Check->SetToolTipText(FText::FromString(TEXT("是否参与任一满足 / 全部满足；不影响当前条件单独查看。")));
	}
	const bool bSelected = Conditions->GetSelectedItem() == Item;
	if (UBorder* Border = FindFilterWidget<UBorder>(&Entry, TEXT("B_ConditionRow")))
	{
		// Use a neutral solid brush so an authored dark tint cannot darken the highlight.
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.TintColor = FLinearColor::White;
		Border->SetBrush(Brush);
		Border->SetBrushColor(FLinearColor::FromSRGBColor(bSelected ? FColor(46, 125, 91) : FColor(38, 58, 65)));
	}
	for (const TCHAR* Name : {TEXT("TXT_ConditionMatchCount"), TEXT("TXT_ConditionField"),
		TEXT("TXT_ConditionOperator"), TEXT("TXT_ConditionValue")})
		if (UTextBlock* Text = FindFilterWidget<UTextBlock>(&Entry, Name))
			Text->SetColorAndOpacity(FSlateColor(FLinearColor::White));
}
void UGCSTelemetryFilterController::ResultGenerated(UUserWidget& Entry)
{
	ResultReleased(Entry);
	if (auto* Item = Cast<UGCSTelemetryFilterItem>(Results->GetListObjectFromEntry(Entry)))
	{
		ResultEntryBindings.Add(&Entry, Item);
		if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ResultSelected")))
			Check->OnCheckStateChanged.AddUniqueDynamic(Item, &UGCSTelemetryFilterItem::OnSelectedChanged);
	}
	PaintResult(Entry);
}
void UGCSTelemetryFilterController::ResultReleased(UUserWidget& Entry)
{
	if (auto* Binding = ResultEntryBindings.Find(&Entry))
	{
		if (Binding->IsValid())
			if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ResultSelected")))
				Check->OnCheckStateChanged.RemoveDynamic(Binding->Get(), &UGCSTelemetryFilterItem::OnSelectedChanged);
		ResultEntryBindings.Remove(&Entry);
	}
}
void UGCSTelemetryFilterController::PaintResult(UUserWidget& Entry)
{
	const auto* Item = Cast<UGCSTelemetryFilterItem>(Results->GetListObjectFromEntry(Entry));
	if (!Item) return;
	FilterText(Entry, TEXT("TXT_ResultDroneID"), Item->DroneID);
	FilterText(Entry, TEXT("TXT_ResultValue"), Item->DisplayValue);
	if (UCheckBox* Check = FindFilterWidget<UCheckBox>(&Entry, TEXT("CHK_ResultSelected")))
	{
		TGuardValue<bool> Painting(bPaintingSelection, true);
		Check->SetIsChecked(Item->bSelected);
		Check->SetIsEnabled(SelectionBridge.IsValid() && !SelectionBridge->IsCommandSelectionMutationBlocked());
	}
}

void UGCSTelemetryFilterController::ResultSelectionChanged(UGCSTelemetryFilterItem* Item, bool bChecked)
{
	if (bPaintingSelection || !Item || !SelectionBridge.IsValid()) return;
	// Ignore delayed events from entries that have left the filtered result set.
	const auto* Current = ResultItems.Find(Item->DroneID);
	if (!Current || Current->Get() != Item) return;
	const FString ID = Item->DroneID;
	SelectionBridge->SetCommandDroneSelected(ID, bChecked);
	RefreshSelection(); // Also restores the checkbox if the workflow rejected mutation.
}

void UGCSTelemetryFilterController::SelectAllChanged(bool bChecked)
{
	if (bPaintingSelection || !SelectionBridge.IsValid() || ResultItems.IsEmpty()) return;
	TSet<FString> Selected;
	Selected.Append(SelectionBridge->GetSelectedCommandDroneIDs());
	int32 AlreadySelected = 0;
	for (const auto& Pair : ResultItems) if (Selected.Contains(Pair.Key)) ++AlreadySelected;
	// Slate toggles Undetermined to Unchecked; a partial-result click should
	// complete the selection instead. Use authoritative IDs, not widget state.
	if (AlreadySelected > 0 && AlreadySelected < ResultItems.Num()) bChecked = true;
	for (const auto& Pair : ResultItems)
	{
		if (bChecked) Selected.Add(Pair.Key);
		else Selected.Remove(Pair.Key);
	}
	// One atomic update, including offscreen results, preserving unrelated targets.
	SelectionBridge->ReplaceCommandDroneSelection(Selected.Array());
	RefreshSelection();
}

void UGCSTelemetryFilterController::RefreshSelection()
{
	if (!Results || !SelectAll) return;
	const TArray<FString> Selected = SelectionBridge.IsValid()
		? SelectionBridge->GetSelectedCommandDroneIDs() : TArray<FString>();
	TSet<FString> SelectedIDs;
	SelectedIDs.Append(Selected);
	int32 SelectedCount = 0;
	for (auto& Pair : ResultItems)
	{
		Pair.Value->bSelected = SelectedIDs.Contains(Pair.Key);
		if (Pair.Value->bSelected) ++SelectedCount;
	}
	for (UUserWidget* Entry : Results->GetDisplayedEntryWidgets()) if (Entry) PaintResult(*Entry);
	TGuardValue<bool> Painting(bPaintingSelection, true);
	const bool bEditable = SelectionBridge.IsValid() && !SelectionBridge->IsCommandSelectionMutationBlocked();
	SelectAll->SetIsEnabled(bEditable && !ResultItems.IsEmpty());
	SelectAll->SetCheckedState(SelectedCount == 0 ? ECheckBoxState::Unchecked
		: SelectedCount == ResultItems.Num() ? ECheckBoxState::Checked : ECheckBoxState::Undetermined);
	SelectAll->SetToolTipText(FText::FromString(FString::Printf(
		TEXT("当前结果已选 %d / %d；仅改变无人机选择，不发送命令。%s"), SelectedCount, ResultItems.Num(),
		bEditable ? TEXT("") : TEXT("当前不可修改选择。"))));
}
void UGCSTelemetryFilterController::RefreshConditions()
{
	for (UUserWidget* Entry : Conditions->GetDisplayedEntryWidgets()) if (Entry) PaintCondition(*Entry);
}
void UGCSTelemetryFilterController::RuleEnabledChanged()
{
	MarkPresetDirty();
	RefreshResults();
}

void UGCSTelemetryFilterController::UpdateTelemetry(const TArray<FMDHKTelemetryTableRow>& InRows,
	const TMap<FString, bool>& InOnline, bool bConnected, int64 NowUtcMs, int32 TimeoutMs)
{
	Rows = InRows;
	Online = InOnline;
	bLinkConnected = bConnected;
	FreshIDs.Reset();
	for (const auto& Row : Rows)
	{
		const bool* State = Online.Find(Row.DroneID);
		if (bConnected && State && *State && Row.bReceived && Row.SampleTimestampUtcMs > 0
			&& Row.SampleTimestampUtcMs <= NowUtcMs && NowUtcMs - Row.SampleTimestampUtcMs <= TimeoutMs)
			FreshIDs.Add(Row.DroneID);
	}
	Rows.Sort([](const auto& A, const auto& B)
	{
		const FValue AI = Read(A, TEXT("UavID"), {}, false), BI = Read(B, TEXT("UavID"), {}, false);
		return AI.bValid && BI.bValid && AI.Integer != BI.Integer ? AI.Integer < BI.Integer : A.DroneID < B.DroneID;
	});
	RefreshResults();
}
void UGCSTelemetryFilterController::RefreshResults()
{
	if (!Results) return;
	TArray<FRule> Active;
	const bool bCurrent = MatchMode->GetSelectedIndex() == 0;
	const auto* SelectedItem = Cast<UGCSTelemetryFilterItem>(Conditions->GetSelectedItem());
	if (SelectedItem && !Rules.Contains(SelectedItem)) SelectedItem = nullptr;
	FString ValidationError;
	if (bCurrent)
	{
		if (SelectedItem && Validate(SelectedItem->Rule, ValidationError))
		{
			FRule Rule = SelectedItem->Rule;
			Rule.bEnabled = true; // Checkboxes control combination modes only.
			Active.Add(Rule);
		}
	}
	else
	{
		for (const auto& Item : Rules)
			if (Item->Rule.bEnabled && Validate(Item->Rule, ValidationError)) Active.Add(Item->Rule);
	}
	// Each count is independent of AND/OR and of whether its rule is enabled.
	for (const auto& Item : Rules)
	{
		Item->MatchCount = 0;
		if (!Validate(Item->Rule, ValidationError)) continue;
		FRule SingleRule = Item->Rule;
		SingleRule.bEnabled = true;
		const TArray<FRule> Single{SingleRule};
		TSet<FString> MatchedIDs;
		for (const auto& Row : Rows)
		{
			TOptional<bool> State;
			if (bLinkConnected) if (const bool* B = Online.Find(Row.DroneID)) State = *B;
			if (!Row.DroneID.IsEmpty() && Matches(Row, State, FreshIDs.Contains(Row.DroneID), Single, false))
				MatchedIDs.Add(Row.DroneID);
		}
		Item->MatchCount = MatchedIDs.Num();
	}
	RefreshConditions();
	const bool bAny = MatchMode->GetSelectedIndex() == 1;
	TArray<UObject*> NewItems;
	TSet<FString> Keep;
	int32 MissingCount = 0;
	// Result values follow the saved selected row, never an unapplied draft.
	const FName DisplayKey = SelectedItem ? SelectedItem->Rule.Field : NAME_None;
	const GCSTelemetryFilter::FField* DisplayField = FindField(DisplayKey);
	for (const auto& Row : Rows)
	{
		if (Row.DroneID.IsEmpty() || Keep.Contains(Row.DroneID)) continue;
		TOptional<bool> State;
		if (bLinkConnected) if (const bool* B = Online.Find(Row.DroneID)) State = *B;
		const bool bFresh = FreshIDs.Contains(Row.DroneID);
		if (!bFresh) ++MissingCount;
		if (Active.IsEmpty() || !Matches(Row, State, bFresh, Active, bAny)) continue;
		Keep.Add(Row.DroneID);
		auto& Item = ResultItems.FindOrAdd(Row.DroneID);
		if (!Item) Item = NewObject<UGCSTelemetryFilterItem>(this);
		Item->DroneID = Row.DroneID;
		Item->Controller = this;
		const FValue V = Read(Row, DisplayKey, State, bFresh);
		Item->DisplayValue = !DisplayField ? TEXT("--") : V.bValid ? V.Text : TEXT("--（无有效数据）");
		NewItems.Add(Item);
	}
	for (auto It = ResultItems.CreateIterator(); It; ++It) if (!Keep.Contains(It.Key())) It.RemoveCurrent();
	// Preserve pooled entry/item identity, scroll offset and condition editor drafts.
	if (Results->GetListItems() != NewItems) Results->SetListItems(NewItems);
	RefreshSelection();
	CountText->SetText(FText::FromString(FString::Printf(TEXT("匹配 %d / 总计 %d"), NewItems.Num(), Rows.Num())));
	CountText->SetToolTipText(FText::FromString(FString::Printf(
		TEXT("模式：%s。结果值：%s。空白条件不参与筛选；无有效条件时结果为空。无新鲜样本 %d 台；缺失值不参与比较。%s"),
		*MatchMode->GetSelectedOption(), DisplayField ? *DisplayField->Label : TEXT("--"), MissingCount,
		bLinkConnected ? TEXT("") : TEXT("遥测链路未连接，在线状态未知。"))));
}
