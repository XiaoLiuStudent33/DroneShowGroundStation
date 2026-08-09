#include "Commands/UnrealMCPUMGCommands.h"
#include "Commands/UnrealMCPCommonUtils.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/UserWidget.h"
#include "Components/TextBlock.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
// We'll create widgets using regular Factory classes
#include "Factories/Factory.h"
// Remove problematic includes that don't exist in UE 5.5
// #include "UMGEditorSubsystem.h"
// #include "WidgetBlueprintFactory.h"
#include "WidgetBlueprintEditor.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/PanelWidget.h"
#include "JsonObjectConverter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Components/Button.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_CallFunction.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "K2Node_Event.h"
#include "ScopedTransaction.h"

namespace
{
void AddCanvasSlotFields(const TSharedPtr<FJsonObject>& WidgetObject, const UCanvasPanelSlot* CanvasSlot)
{
	const FVector2D Position = CanvasSlot->GetPosition();
	const FVector2D Size = CanvasSlot->GetSize();
	const FAnchors Anchors = CanvasSlot->GetAnchors();
	const FVector2D Alignment = CanvasSlot->GetAlignment();

	WidgetObject->SetNumberField(TEXT("position_x"), Position.X);
	WidgetObject->SetNumberField(TEXT("position_y"), Position.Y);
	WidgetObject->SetNumberField(TEXT("size_x"), Size.X);
	WidgetObject->SetNumberField(TEXT("size_y"), Size.Y);
	WidgetObject->SetNumberField(TEXT("anchor_min_x"), Anchors.Minimum.X);
	WidgetObject->SetNumberField(TEXT("anchor_min_y"), Anchors.Minimum.Y);
	WidgetObject->SetNumberField(TEXT("anchor_max_x"), Anchors.Maximum.X);
	WidgetObject->SetNumberField(TEXT("anchor_max_y"), Anchors.Maximum.Y);
	WidgetObject->SetNumberField(TEXT("alignment_x"), Alignment.X);
	WidgetObject->SetNumberField(TEXT("alignment_y"), Alignment.Y);
	WidgetObject->SetNumberField(TEXT("z_order"), CanvasSlot->GetZOrder());
	WidgetObject->SetBoolField(TEXT("auto_size"), CanvasSlot->GetAutoSize());
}

void AppendWidgetTree(UWidget* Widget, const FString& ParentName, TArray<TSharedPtr<FJsonValue>>& Widgets)
{
	if (!Widget)
	{
		return;
	}

	TSharedPtr<FJsonObject> WidgetObject = MakeShared<FJsonObject>();
	WidgetObject->SetStringField(TEXT("name"), Widget->GetName());
	WidgetObject->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
	if (ParentName.IsEmpty())
	{
		WidgetObject->SetField(TEXT("parent_name"), MakeShared<FJsonValueNull>());
	}
	else
	{
		WidgetObject->SetStringField(TEXT("parent_name"), ParentName);
	}

	if (Widget->Slot)
	{
		WidgetObject->SetStringField(TEXT("slot_class"), Widget->Slot->GetClass()->GetName());
		if (const UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot))
		{
			AddCanvasSlotFields(WidgetObject, CanvasSlot);
		}
	}
	else
	{
		WidgetObject->SetField(TEXT("slot_class"), MakeShared<FJsonValueNull>());
	}

	const UEnum* VisibilityEnum = StaticEnum<ESlateVisibility>();
	WidgetObject->SetStringField(
		TEXT("visibility"),
		VisibilityEnum
			? VisibilityEnum->GetNameStringByValue(static_cast<int64>(Widget->GetVisibility()))
			: TEXT("unsupported"));
	if (const UTextBlock* TextBlock = Cast<UTextBlock>(Widget))
	{
		WidgetObject->SetNumberField(TEXT("font_size"), TextBlock->GetFont().Size);
	}
	Widgets.Add(MakeShared<FJsonValueObject>(WidgetObject));

	if (UPanelWidget* PanelWidget = Cast<UPanelWidget>(Widget))
	{
		for (int32 ChildIndex = 0; ChildIndex < PanelWidget->GetChildrenCount(); ++ChildIndex)
		{
			AppendWidgetTree(PanelWidget->GetChildAt(ChildIndex), Widget->GetName(), Widgets);
		}
	}
}

bool ReadOptionalNumber(
	const FJsonObject& Params,
	const TCHAR* FieldName,
	TOptional<double>& OutValue,
	FString& OutError)
{
	if (!Params.HasField(FieldName))
	{
		return true;
	}

	double Value = 0.0;
	if (!Params.TryGetNumberField(FieldName, Value))
	{
		OutError = FString::Printf(TEXT("Field '%s' must be numeric"), FieldName);
		return false;
	}

	OutValue = Value;
	return true;
}

bool ReadOptionalBool(
	const FJsonObject& Params,
	const TCHAR* FieldName,
	TOptional<bool>& OutValue,
	FString& OutError)
{
	if (!Params.HasField(FieldName))
	{
		return true;
	}

	bool Value = false;
	if (!Params.TryGetBoolField(FieldName, Value))
	{
		OutError = FString::Printf(TEXT("Field '%s' must be boolean"), FieldName);
		return false;
	}

	OutValue = Value;
	return true;
}
}

FUnrealMCPUMGCommands::FUnrealMCPUMGCommands()
{
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCommand(const FString& CommandName, const TSharedPtr<FJsonObject>& Params)
{
	if (CommandName == TEXT("create_umg_widget_blueprint"))
	{
		return HandleCreateUMGWidgetBlueprint(Params);
	}
	else if (CommandName == TEXT("add_text_block_to_widget"))
	{
		return HandleAddTextBlockToWidget(Params);
	}
	else if (CommandName == TEXT("add_widget_to_viewport"))
	{
		return HandleAddWidgetToViewport(Params);
	}
	else if (CommandName == TEXT("add_button_to_widget"))
	{
		return HandleAddButtonToWidget(Params);
	}
	else if (CommandName == TEXT("bind_widget_event"))
	{
		return HandleBindWidgetEvent(Params);
	}
	else if (CommandName == TEXT("set_text_block_binding"))
	{
		return HandleSetTextBlockBinding(Params);
	}
	else if (CommandName == TEXT("get_widget_tree"))
	{
		return HandleGetWidgetTree(Params);
	}
	else if (CommandName == TEXT("set_canvas_slot_layout"))
	{
		return HandleSetCanvasSlotLayout(Params);
	}
	else if (CommandName == TEXT("save_asset"))
	{
		return HandleSaveAsset(Params);
	}
	else if (CommandName == TEXT("duplicate_asset"))
	{
		return HandleDuplicateAsset(Params);
	}
	else if (CommandName == TEXT("set_text_block_font_size"))
	{
		return HandleSetTextBlockFontSize(Params);
	}

	return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Unknown UMG command: %s"), *CommandName));
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleCreateUMGWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("name"), BlueprintName)
		&& !Params->TryGetStringField(TEXT("widget_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'name' or 'widget_name' parameter"));
	}

	// Create the full asset path
	FString PackagePath = TEXT("/Game/Widgets/");
	Params->TryGetStringField(TEXT("path"), PackagePath);
	PackagePath.RemoveFromEnd(TEXT("/"));
	PackagePath += TEXT("/");
	FString AssetName = BlueprintName;
	FString FullPath = PackagePath + AssetName;

	// Check if asset already exists
	if (UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' already exists"), *BlueprintName));
	}

	// Create package
	UPackage* Package = CreatePackage(*FullPath);
	if (!Package)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create package"));
	}

	// Create Widget Blueprint using KismetEditorUtilities
	UBlueprint* NewBlueprint = FKismetEditorUtilities::CreateBlueprint(
		UUserWidget::StaticClass(),  // Parent class
		Package,                     // Outer package
		FName(*AssetName),           // Blueprint name
		BPTYPE_Normal,               // Blueprint type
		UWidgetBlueprint::StaticClass(),   // Blueprint class
		UWidgetBlueprintGeneratedClass::StaticClass(), // Generated class
		FName("CreateUMGWidget")     // Creation method name
	);

	// Make sure the Blueprint was created successfully
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(NewBlueprint);
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Widget Blueprint"));
	}

	// Add a default Canvas Panel if one doesn't exist
	if (!WidgetBlueprint->WidgetTree->RootWidget)
	{
		UCanvasPanel* RootCanvas = WidgetBlueprint->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass());
		WidgetBlueprint->WidgetTree->RootWidget = RootCanvas;
	}

	// Mark the package dirty and notify asset registry
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(WidgetBlueprint);

	// Compile the blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(FullPath, false);

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("name"), BlueprintName);
	ResultObj->SetStringField(TEXT("path"), FullPath);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddTextBlockToWidget(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName)
		&& !Params->TryGetStringField(TEXT("widget_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing target Widget Blueprint name"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("text_block_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'text_block_name' parameter"));
	}

	// Find the Widget Blueprint
	FString FullPath = TEXT("/Game/UI/") + BlueprintName;
	if (!UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		FullPath = TEXT("/Game/UI/GCS/") + BlueprintName;
	}
	if (!UEditorAssetLibrary::DoesAssetExist(FullPath))
	{
		FullPath = TEXT("/Game/Widgets/") + BlueprintName;
	}
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(FullPath));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Get optional parameters
	FString InitialText = TEXT("New Text Block");
	Params->TryGetStringField(TEXT("text"), InitialText);

	FVector2D Position(0.0f, 0.0f);
	if (Params->HasField(TEXT("position")))
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArray;
		if (Params->TryGetArrayField(TEXT("position"), PosArray) && PosArray->Num() >= 2)
		{
			Position.X = (*PosArray)[0]->AsNumber();
			Position.Y = (*PosArray)[1]->AsNumber();
		}
	}

	// Create Text Block widget
	UTextBlock* TextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), *WidgetName);
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to create Text Block widget"));
	}

	// Set initial text
	TextBlock->SetText(FText::FromString(InitialText));
	int32 FontSize = 16;
	Params->TryGetNumberField(TEXT("font_size"), FontSize);
	FSlateFontInfo FontInfo = TextBlock->GetFont();
	FontInfo.Size = FontSize;
	TextBlock->SetFont(FontInfo);
	const TArray<TSharedPtr<FJsonValue>>* ColorArray = nullptr;
	if (Params->TryGetArrayField(TEXT("color"), ColorArray) && ColorArray->Num() >= 4)
	{
		TextBlock->SetColorAndOpacity(FSlateColor(FLinearColor(
			(*ColorArray)[0]->AsNumber(),
			(*ColorArray)[1]->AsNumber(),
			(*ColorArray)[2]->AsNumber(),
			(*ColorArray)[3]->AsNumber())));
	}

	// Add to canvas panel
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Root Canvas Panel not found"));
	}

	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(TextBlock);
	PanelSlot->SetPosition(Position);
	const TArray<TSharedPtr<FJsonValue>>* SizeArray = nullptr;
	if (Params->TryGetArrayField(TEXT("size"), SizeArray) && SizeArray->Num() >= 2)
	{
		PanelSlot->SetSize(FVector2D((*SizeArray)[0]->AsNumber(), (*SizeArray)[1]->AsNumber()));
	}

	// Mark the package dirty and compile
	WidgetBlueprint->MarkPackageDirty();
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);

	// Create success response
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("widget_name"), WidgetName);
	ResultObj->SetStringField(TEXT("text"), InitialText);
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddWidgetToViewport(const TSharedPtr<FJsonObject>& Params)
{
	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'blueprint_name' parameter"));
	}

	// Find the Widget Blueprint
	FString FullPath = TEXT("/Game/Widgets/") + BlueprintName;
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(FullPath));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(FString::Printf(TEXT("Widget Blueprint '%s' not found"), *BlueprintName));
	}

	// Get optional Z-order parameter
	int32 ZOrder = 0;
	Params->TryGetNumberField(TEXT("z_order"), ZOrder);

	// Create widget instance
	UClass* WidgetClass = WidgetBlueprint->GeneratedClass;
	if (!WidgetClass)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Failed to get widget class"));
	}

	// Note: This creates the widget but doesn't add it to viewport
	// The actual addition to viewport should be done through Blueprint nodes
	// as it requires a game context

	// Create success response with instructions
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("blueprint_name"), BlueprintName);
	ResultObj->SetStringField(TEXT("class_path"), WidgetClass->GetPathName());
	ResultObj->SetNumberField(TEXT("z_order"), ZOrder);
	ResultObj->SetStringField(TEXT("note"), TEXT("Widget class ready. Use CreateWidget and AddToViewport nodes in Blueprint to display in game."));
	return ResultObj;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleAddButtonToWidget(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName)
		&& !Params->TryGetStringField(TEXT("widget_name"), BlueprintName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing blueprint_name parameter"));
		return Response;
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("button_name"), WidgetName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing button_name parameter"));
		return Response;
	}

	FString ButtonText;
	if (!Params->TryGetStringField(TEXT("text"), ButtonText))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing text parameter"));
		return Response;
	}

	// Load the Widget Blueprint
	FString BlueprintPath = FString::Printf(TEXT("/Game/UI/%s.%s"), *BlueprintName, *BlueprintName);
	if (!UEditorAssetLibrary::DoesAssetExist(BlueprintPath))
	{
		BlueprintPath = FString::Printf(TEXT("/Game/UI/GCS/%s.%s"), *BlueprintName, *BlueprintName);
	}
	if (!UEditorAssetLibrary::DoesAssetExist(BlueprintPath))
	{
		BlueprintPath = FString::Printf(TEXT("/Game/Widgets/%s.%s"), *BlueprintName, *BlueprintName);
	}
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintPath));
	if (!WidgetBlueprint)
	{
		Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *BlueprintPath));
		return Response;
	}

	// Editor-only helper: a CHILD_* entry embeds another Widget Blueprint as a
	// normal, designer-editable child widget. The `text` parameter contains the
	// child asset name under /Game/UI.
	if (WidgetName.StartsWith(TEXT("CHILD_")))
	{
		const FString ChildClassPath = FString::Printf(
			TEXT("/Game/UI/%s.%s_C"),
			*ButtonText,
			*ButtonText);
		UClass* ChildClass = LoadClass<UUserWidget>(nullptr, *ChildClassPath);
		if (!ChildClass)
		{
			Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load child widget class: %s"), *ChildClassPath));
			return Response;
		}

		UUserWidget* ChildWidget = WidgetBlueprint->WidgetTree->ConstructWidget<UUserWidget>(
			ChildClass,
			*WidgetName);
		UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
		if (!ChildWidget || !RootCanvas)
		{
			Response->SetStringField(TEXT("error"), TEXT("Failed to create child widget or find root canvas"));
			return Response;
		}

		UCanvasPanelSlot* ChildSlot = RootCanvas->AddChildToCanvas(ChildWidget);
		const TArray<TSharedPtr<FJsonValue>>* PositionArray = nullptr;
		if (Params->TryGetArrayField(TEXT("position"), PositionArray) && PositionArray->Num() >= 2)
		{
			ChildSlot->SetPosition(FVector2D((*PositionArray)[0]->AsNumber(), (*PositionArray)[1]->AsNumber()));
		}
		const TArray<TSharedPtr<FJsonValue>>* SizeArray = nullptr;
		if (Params->TryGetArrayField(TEXT("size"), SizeArray) && SizeArray->Num() >= 2)
		{
			ChildSlot->SetSize(FVector2D((*SizeArray)[0]->AsNumber(), (*SizeArray)[1]->AsNumber()));
		}
		ChildSlot->SetZOrder(0);

		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
		UEditorAssetLibrary::SaveAsset(BlueprintPath, false);
		Response->SetBoolField(TEXT("success"), true);
		Response->SetStringField(TEXT("widget_name"), WidgetName);
		Response->SetStringField(TEXT("child_class"), ChildClassPath);
		return Response;
	}

	// Create Button widget
	UButton* Button = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), *WidgetName);
	if (!Button)
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to create Button widget"));
		return Response;
	}

	// Set button text
	UTextBlock* ButtonTextBlock = WidgetBlueprint->WidgetTree->ConstructWidget<UTextBlock>(
		UTextBlock::StaticClass(),
		*(WidgetName + TEXT("_Text")));
	if (ButtonTextBlock)
	{
		ButtonTextBlock->SetText(FText::FromString(ButtonText));
		int32 FontSize = 16;
		Params->TryGetNumberField(TEXT("font_size"), FontSize);
		FSlateFontInfo FontInfo = ButtonTextBlock->GetFont();
		FontInfo.Size = FontSize;
		ButtonTextBlock->SetFont(FontInfo);
		const TArray<TSharedPtr<FJsonValue>>* TextColorArray = nullptr;
		if (Params->TryGetArrayField(TEXT("color"), TextColorArray) && TextColorArray->Num() >= 4)
		{
			ButtonTextBlock->SetColorAndOpacity(FSlateColor(FLinearColor(
				(*TextColorArray)[0]->AsNumber(),
				(*TextColorArray)[1]->AsNumber(),
				(*TextColorArray)[2]->AsNumber(),
				(*TextColorArray)[3]->AsNumber())));
		}
		Button->AddChild(ButtonTextBlock);
	}
	const TArray<TSharedPtr<FJsonValue>>* BackgroundColorArray = nullptr;
	if (Params->TryGetArrayField(TEXT("background_color"), BackgroundColorArray) && BackgroundColorArray->Num() >= 4)
	{
		Button->SetBackgroundColor(FLinearColor(
			(*BackgroundColorArray)[0]->AsNumber(),
			(*BackgroundColorArray)[1]->AsNumber(),
			(*BackgroundColorArray)[2]->AsNumber(),
			(*BackgroundColorArray)[3]->AsNumber()));
	}

	// Get canvas panel and add button
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(WidgetBlueprint->WidgetTree->RootWidget);
	if (!RootCanvas)
	{
		Response->SetStringField(TEXT("error"), TEXT("Root widget is not a Canvas Panel"));
		return Response;
	}

	// Add to canvas and set position
	UCanvasPanelSlot* ButtonSlot = RootCanvas->AddChildToCanvas(Button);
	if (ButtonSlot)
	{
		ButtonSlot->SetZOrder(WidgetName.StartsWith(TEXT("PANEL_")) ? -10 : 10);
		const TArray<TSharedPtr<FJsonValue>>* Position;
		if (Params->TryGetArrayField(TEXT("position"), Position) && Position->Num() >= 2)
		{
			FVector2D Pos(
				(*Position)[0]->AsNumber(),
				(*Position)[1]->AsNumber()
			);
			ButtonSlot->SetPosition(Pos);
		}
		const TArray<TSharedPtr<FJsonValue>>* SizeArray = nullptr;
		if (Params->TryGetArrayField(TEXT("size"), SizeArray) && SizeArray->Num() >= 2)
		{
			ButtonSlot->SetSize(FVector2D((*SizeArray)[0]->AsNumber(), (*SizeArray)[1]->AsNumber()));
		}
	}

	// Save the Widget Blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(BlueprintPath, false);

	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleBindWidgetEvent(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing blueprint_name parameter"));
		return Response;
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing widget_name parameter"));
		return Response;
	}

	FString EventName;
	if (!Params->TryGetStringField(TEXT("event_name"), EventName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing event_name parameter"));
		return Response;
	}

	// Load the Widget Blueprint
	const FString BlueprintPath = FString::Printf(TEXT("/Game/Widgets/%s.%s"), *BlueprintName, *BlueprintName);
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintPath));
	if (!WidgetBlueprint)
	{
		Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *BlueprintPath));
		return Response;
	}

	// Create the event graph if it doesn't exist
	UEdGraph* EventGraph = FBlueprintEditorUtils::FindEventGraph(WidgetBlueprint);
	if (!EventGraph)
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to find or create event graph"));
		return Response;
	}

	// Find the widget in the blueprint
	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(*WidgetName);
	if (!Widget)
	{
		Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to find widget: %s"), *WidgetName));
		return Response;
	}

	// Create the event node (e.g., OnClicked for buttons)
	UK2Node_Event* EventNode = nullptr;
	
	// Find existing nodes first
	TArray<UK2Node_Event*> AllEventNodes;
	FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(WidgetBlueprint, AllEventNodes);
	
	for (UK2Node_Event* Node : AllEventNodes)
	{
		if (Node->CustomFunctionName == FName(*EventName) && Node->EventReference.GetMemberParentClass() == Widget->GetClass())
		{
			EventNode = Node;
			break;
		}
	}

	// If no existing node, create a new one
	if (!EventNode)
	{
		// Calculate position - place it below existing nodes
		float MaxHeight = 0.0f;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			MaxHeight = FMath::Max(MaxHeight, Node->NodePosY);
		}
		
		const FVector2D NodePos(200, MaxHeight + 200);

		// Call CreateNewBoundEventForClass, which returns void, so we can't capture the return value directly
		// We'll need to find the node after creating it
		FKismetEditorUtilities::CreateNewBoundEventForClass(
			Widget->GetClass(),
			FName(*EventName),
			WidgetBlueprint,
			nullptr  // We don't need a specific property binding
		);

		// Now find the newly created node
		TArray<UK2Node_Event*> UpdatedEventNodes;
		FBlueprintEditorUtils::GetAllNodesOfClass<UK2Node_Event>(WidgetBlueprint, UpdatedEventNodes);
		
		for (UK2Node_Event* Node : UpdatedEventNodes)
		{
			if (Node->CustomFunctionName == FName(*EventName) && Node->EventReference.GetMemberParentClass() == Widget->GetClass())
			{
				EventNode = Node;
				
				// Set position of the node
				EventNode->NodePosX = NodePos.X;
				EventNode->NodePosY = NodePos.Y;
				
				break;
			}
		}
	}

	if (!EventNode)
	{
		Response->SetStringField(TEXT("error"), TEXT("Failed to create event node"));
		return Response;
	}

	// Save the Widget Blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(BlueprintPath, false);

	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("event_name"), EventName);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetTextBlockBinding(const TSharedPtr<FJsonObject>& Params)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();

	// Get required parameters
	FString BlueprintName;
	if (!Params->TryGetStringField(TEXT("blueprint_name"), BlueprintName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing blueprint_name parameter"));
		return Response;
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing widget_name parameter"));
		return Response;
	}

	FString BindingName;
	if (!Params->TryGetStringField(TEXT("binding_name"), BindingName))
	{
		Response->SetStringField(TEXT("error"), TEXT("Missing binding_name parameter"));
		return Response;
	}

	// Load the Widget Blueprint
	const FString BlueprintPath = FString::Printf(TEXT("/Game/Widgets/%s.%s"), *BlueprintName, *BlueprintName);
	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(BlueprintPath));
	if (!WidgetBlueprint)
	{
		Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *BlueprintPath));
		return Response;
	}

	// Create a variable for binding if it doesn't exist
	FBlueprintEditorUtils::AddMemberVariable(
		WidgetBlueprint,
		FName(*BindingName),
		FEdGraphPinType(UEdGraphSchema_K2::PC_Text, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType())
	);

	// Find the TextBlock widget
	UTextBlock* TextBlock = Cast<UTextBlock>(WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName)));
	if (!TextBlock)
	{
		Response->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to find TextBlock widget: %s"), *WidgetName));
		return Response;
	}

	// Create binding function
	const FString FunctionName = FString::Printf(TEXT("Get%s"), *BindingName);
	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		WidgetBlueprint,
		FName(*FunctionName),
		UEdGraph::StaticClass(),
		UEdGraphSchema_K2::StaticClass()
	);

	if (FuncGraph)
	{
		// Add the function to the blueprint with proper template parameter
		// Template requires null for last parameter when not using a signature-source
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(WidgetBlueprint, FuncGraph, false, nullptr);

		// Create entry node
		UK2Node_FunctionEntry* EntryNode = nullptr;
		
		// Create entry node - use the API that exists in UE 5.5
		EntryNode = NewObject<UK2Node_FunctionEntry>(FuncGraph);
		FuncGraph->AddNode(EntryNode, false, false);
		EntryNode->NodePosX = 0;
		EntryNode->NodePosY = 0;
		EntryNode->FunctionReference.SetExternalMember(FName(*FunctionName), WidgetBlueprint->GeneratedClass);
		EntryNode->AllocateDefaultPins();

		// Create get variable node
		UK2Node_VariableGet* GetVarNode = NewObject<UK2Node_VariableGet>(FuncGraph);
		GetVarNode->VariableReference.SetSelfMember(FName(*BindingName));
		FuncGraph->AddNode(GetVarNode, false, false);
		GetVarNode->NodePosX = 200;
		GetVarNode->NodePosY = 0;
		GetVarNode->AllocateDefaultPins();

		// Connect nodes
		UEdGraphPin* EntryThenPin = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
		UEdGraphPin* GetVarOutPin = GetVarNode->FindPin(UEdGraphSchema_K2::PN_ReturnValue);
		if (EntryThenPin && GetVarOutPin)
		{
			EntryThenPin->MakeLinkTo(GetVarOutPin);
		}
	}

	// Save the Widget Blueprint
	FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	UEditorAssetLibrary::SaveAsset(BlueprintPath, false);

	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("binding_name"), BindingName);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleGetWidgetTree(const TSharedPtr<FJsonObject>& Params)
{
	UE_LOG(LogTemp, Display, TEXT("[MCP][GetWidgetTree] Enter IsInGameThread=%s"), IsInGameThread() ? TEXT("true") : TEXT("false"));

	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
	}

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!WidgetBlueprint)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}
	if (!WidgetBlueprint->WidgetTree || !WidgetBlueprint->WidgetTree->RootWidget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Widget Blueprint has no Widget Tree root: %s"), *AssetPath));
	}

	TArray<TSharedPtr<FJsonValue>> Widgets;
	AppendWidgetTree(WidgetBlueprint->WidgetTree->RootWidget, FString(), Widgets);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("asset_path"), AssetPath);
	Response->SetArrayField(TEXT("widgets"), Widgets);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetCanvasSlotLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
	}

	TOptional<double> PositionX;
	TOptional<double> PositionY;
	TOptional<double> SizeX;
	TOptional<double> SizeY;
	TOptional<double> AnchorMinX;
	TOptional<double> AnchorMinY;
	TOptional<double> AnchorMaxX;
	TOptional<double> AnchorMaxY;
	TOptional<double> AlignmentX;
	TOptional<double> AlignmentY;
	TOptional<double> ZOrder;
	TOptional<bool> AutoSize;
	FString Error;

	if (!ReadOptionalNumber(*Params, TEXT("position_x"), PositionX, Error)
		|| !ReadOptionalNumber(*Params, TEXT("position_y"), PositionY, Error)
		|| !ReadOptionalNumber(*Params, TEXT("size_x"), SizeX, Error)
		|| !ReadOptionalNumber(*Params, TEXT("size_y"), SizeY, Error)
		|| !ReadOptionalNumber(*Params, TEXT("anchor_min_x"), AnchorMinX, Error)
		|| !ReadOptionalNumber(*Params, TEXT("anchor_min_y"), AnchorMinY, Error)
		|| !ReadOptionalNumber(*Params, TEXT("anchor_max_x"), AnchorMaxX, Error)
		|| !ReadOptionalNumber(*Params, TEXT("anchor_max_y"), AnchorMaxY, Error)
		|| !ReadOptionalNumber(*Params, TEXT("alignment_x"), AlignmentX, Error)
		|| !ReadOptionalNumber(*Params, TEXT("alignment_y"), AlignmentY, Error)
		|| !ReadOptionalNumber(*Params, TEXT("z_order"), ZOrder, Error)
		|| !ReadOptionalBool(*Params, TEXT("auto_size"), AutoSize, Error))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(Error);
	}

	const bool bHasLayoutField = PositionX.IsSet() || PositionY.IsSet()
		|| SizeX.IsSet() || SizeY.IsSet()
		|| AnchorMinX.IsSet() || AnchorMinY.IsSet()
		|| AnchorMaxX.IsSet() || AnchorMaxY.IsSet()
		|| AlignmentX.IsSet() || AlignmentY.IsSet()
		|| ZOrder.IsSet() || AutoSize.IsSet();
	if (!bHasLayoutField)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("No Canvas slot layout fields were provided"));
	}

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
	}

	UCanvasPanelSlot* CanvasSlot = Cast<UCanvasPanelSlot>(Widget->Slot);
	if (!CanvasSlot)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Widget '%s' does not use a CanvasPanelSlot"), *WidgetName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetCanvasSlotLayout", "Set Canvas Slot Layout"));
	WidgetBlueprint->Modify();
	Widget->Modify();
	CanvasSlot->Modify();

	if (PositionX.IsSet() || PositionY.IsSet())
	{
		FVector2D Position = CanvasSlot->GetPosition();
		Position.X = PositionX.IsSet() ? static_cast<float>(PositionX.GetValue()) : Position.X;
		Position.Y = PositionY.IsSet() ? static_cast<float>(PositionY.GetValue()) : Position.Y;
		CanvasSlot->SetPosition(Position);
	}
	if (SizeX.IsSet() || SizeY.IsSet())
	{
		FVector2D Size = CanvasSlot->GetSize();
		Size.X = SizeX.IsSet() ? static_cast<float>(SizeX.GetValue()) : Size.X;
		Size.Y = SizeY.IsSet() ? static_cast<float>(SizeY.GetValue()) : Size.Y;
		CanvasSlot->SetSize(Size);
	}
	if (AnchorMinX.IsSet() || AnchorMinY.IsSet() || AnchorMaxX.IsSet() || AnchorMaxY.IsSet())
	{
		FAnchors Anchors = CanvasSlot->GetAnchors();
		Anchors.Minimum.X = AnchorMinX.IsSet() ? static_cast<float>(AnchorMinX.GetValue()) : Anchors.Minimum.X;
		Anchors.Minimum.Y = AnchorMinY.IsSet() ? static_cast<float>(AnchorMinY.GetValue()) : Anchors.Minimum.Y;
		Anchors.Maximum.X = AnchorMaxX.IsSet() ? static_cast<float>(AnchorMaxX.GetValue()) : Anchors.Maximum.X;
		Anchors.Maximum.Y = AnchorMaxY.IsSet() ? static_cast<float>(AnchorMaxY.GetValue()) : Anchors.Maximum.Y;
		CanvasSlot->SetAnchors(Anchors);
	}
	if (AlignmentX.IsSet() || AlignmentY.IsSet())
	{
		FVector2D Alignment = CanvasSlot->GetAlignment();
		Alignment.X = AlignmentX.IsSet() ? static_cast<float>(AlignmentX.GetValue()) : Alignment.X;
		Alignment.Y = AlignmentY.IsSet() ? static_cast<float>(AlignmentY.GetValue()) : Alignment.Y;
		CanvasSlot->SetAlignment(Alignment);
	}
	if (ZOrder.IsSet())
	{
		CanvasSlot->SetZOrder(static_cast<int32>(ZOrder.GetValue()));
	}
	if (AutoSize.IsSet())
	{
		CanvasSlot->SetAutoSize(AutoSize.GetValue());
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	WidgetBlueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("asset_path"), AssetPath);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	AddCanvasSlotFields(Response, CanvasSlot);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSaveAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
	}

	if (!UEditorAssetLibrary::LoadAsset(AssetPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load asset: %s"), *AssetPath));
	}
	if (!UEditorAssetLibrary::SaveAsset(AssetPath, false))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to save asset: %s"), *AssetPath));
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("asset_path"), AssetPath);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleDuplicateAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString SourceAsset;
	if (!Params->TryGetStringField(TEXT("source_asset"), SourceAsset))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'source_asset' parameter"));
	}

	FString DestinationAsset;
	if (!Params->TryGetStringField(TEXT("destination_asset"), DestinationAsset))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'destination_asset' parameter"));
	}

	if (!UEditorAssetLibrary::DoesAssetExist(SourceAsset))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Source asset does not exist: %s"), *SourceAsset));
	}
	if (UEditorAssetLibrary::DoesAssetExist(DestinationAsset))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Destination asset already exists: %s"), *DestinationAsset));
	}

	UObject* DuplicatedAsset = UEditorAssetLibrary::DuplicateAsset(SourceAsset, DestinationAsset);
	if (!DuplicatedAsset || !UEditorAssetLibrary::DoesAssetExist(DestinationAsset))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to duplicate asset from %s to %s"), *SourceAsset, *DestinationAsset));
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("source_asset"), SourceAsset);
	Response->SetStringField(TEXT("destination_asset"), DestinationAsset);
	return Response;
}

TSharedPtr<FJsonObject> FUnrealMCPUMGCommands::HandleSetTextBlockFontSize(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'asset_path' parameter"));
	}

	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing 'widget_name' parameter"));
	}

	double FontSizeValue = 0.0;
	if (!Params->TryGetNumberField(TEXT("font_size"), FontSizeValue))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("Missing or invalid 'font_size' parameter"));
	}
	const int32 FontSize = static_cast<int32>(FontSizeValue);
	if (FontSize <= 0 || !FMath::IsNearlyEqual(FontSizeValue, static_cast<double>(FontSize)))
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(TEXT("'font_size' must be a positive integer"));
	}

	UWidgetBlueprint* WidgetBlueprint = Cast<UWidgetBlueprint>(UEditorAssetLibrary::LoadAsset(AssetPath));
	if (!WidgetBlueprint || !WidgetBlueprint->WidgetTree)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Failed to load Widget Blueprint: %s"), *AssetPath));
	}

	UWidget* Widget = WidgetBlueprint->WidgetTree->FindWidget(FName(*WidgetName));
	if (!Widget)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Widget not found: %s"), *WidgetName));
	}

	UTextBlock* TextBlock = Cast<UTextBlock>(Widget);
	if (!TextBlock)
	{
		return FUnrealMCPCommonUtils::CreateErrorResponse(
			FString::Printf(TEXT("Widget '%s' is not a TextBlock"), *WidgetName));
	}

	const FScopedTransaction Transaction(NSLOCTEXT("UnrealMCP", "SetTextBlockFontSize", "Set TextBlock Font Size"));
	WidgetBlueprint->Modify();
	TextBlock->Modify();

	FSlateFontInfo Font = TextBlock->GetFont();
	Font.Size = FontSize;
	TextBlock->SetFont(Font);

	FBlueprintEditorUtils::MarkBlueprintAsModified(WidgetBlueprint);
	WidgetBlueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetBoolField(TEXT("success"), true);
	Response->SetStringField(TEXT("asset_path"), AssetPath);
	Response->SetStringField(TEXT("widget_name"), WidgetName);
	Response->SetNumberField(TEXT("font_size"), TextBlock->GetFont().Size);
	return Response;
}
