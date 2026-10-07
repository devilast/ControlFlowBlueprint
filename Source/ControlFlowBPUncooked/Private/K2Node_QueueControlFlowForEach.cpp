#include "K2Node_QueueControlFlowForEach.h"

#include "ControlFlowBP.h"

#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "KismetCompiler.h"
#include "Styling/AppStyle.h"

#define LOCTEXT_NAMESPACE "K2Node_QueueControlFlowForEach"

const FName UK2Node_QueueControlFlowForEach::PN_Items(TEXT("Items"));
const FName UK2Node_QueueControlFlowForEach::PN_IterationFunction(TEXT("IterationFunction"));

void UK2Node_QueueControlFlowForEach::CreateStepPins()
{
	FCreatePinParams ItemsParams;
	ItemsParams.ContainerType = EPinContainerType::Array;

	UEdGraphPin* ItemsPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Wildcard, PN_Items, ItemsParams);
	ItemsPin->PinToolTip = LOCTEXT("ItemsTooltip",
		"The array to loop over, of any type. It is copied when the step is queued: changes made to the array afterwards do not reach the loop.").ToString();

	CreatePickPin(LOCTEXT("IterationPin", "Iteration"),
		LOCTEXT("IterationTooltip", "The function or custom event on this Blueprint that builds each iteration: it takes a Control Flow Loop and queues the iteration's steps onto its Get Body."));
}

FText UK2Node_QueueControlFlowForEach::GetBaseTitle() const
{
	return LOCTEXT("Title", "Queue For Each");
}

FText UK2Node_QueueControlFlowForEach::GetPickLabel() const
{
	return LOCTEXT("PickLabel", "Iteration");
}

void UK2Node_QueueControlFlowForEach::PostReconstructNode()
{
	Super::PostReconstructNode();

	SyncItemsType();
}

void UK2Node_QueueControlFlowForEach::NotifyPinConnectionListChanged(UEdGraphPin* Pin)
{
	Super::NotifyPinConnectionListChanged(Pin);

	if (Pin && Pin->PinName == PN_Items && SyncItemsType())
	{
		GetGraph()->NotifyNodeChanged(this);
	}
}

bool UK2Node_QueueControlFlowForEach::SyncItemsType()
{
	UEdGraphPin* ItemsPin = FindPin(PN_Items, EGPD_Input);
	if (!ItemsPin)
	{
		return false;
	}

	FEdGraphPinType NewType;
	NewType.PinCategory = UEdGraphSchema_K2::PC_Wildcard;
	NewType.ContainerType = EPinContainerType::Array;

	const UEdGraphPin* LinkedPin = ItemsPin->LinkedTo.Num() > 0 ? ItemsPin->LinkedTo[0] : nullptr;
	if (LinkedPin && LinkedPin->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard)
	{
		NewType.PinCategory = LinkedPin->PinType.PinCategory;
		NewType.PinSubCategory = LinkedPin->PinType.PinSubCategory;
		NewType.PinSubCategoryObject = LinkedPin->PinType.PinSubCategoryObject;
		NewType.PinSubCategoryMemberReference = LinkedPin->PinType.PinSubCategoryMemberReference;
	}

	if (NewType == ItemsPin->PinType)
	{
		return false;
	}

	ItemsPin->PinType = NewType;
	return true;
}

bool UK2Node_QueueControlFlowForEach::CheckItemsConnected(FKismetCompilerContext& CompilerContext)
{
	SyncItemsType();

	UEdGraphPin* ItemsPin = FindPin(PN_Items, EGPD_Input);
	if (!ItemsPin || ItemsPin->LinkedTo.Num() == 0)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NoItems", "@@ needs an array on its @@ pin.").ToString(), this, ItemsPin);
		return false;
	}

	if (ItemsPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("UntypedItems", "@@: the type of the array on @@ is not known. Connect an array of a specific type.").ToString(), this, ItemsPin);
		return false;
	}

	return true;
}

void UK2Node_QueueControlFlowForEach::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	Super::ExpandNode(CompilerContext, SourceGraph);

	FName ResolvedName;
	const UFunction* Iteration = ResolvePickedFunction(&ResolvedName);
	const bool bTargetConnected = CheckTargetConnected(CompilerContext);
	const bool bItemsConnected = CheckItemsConnected(CompilerContext);
	if (!bTargetConnected || !bItemsConnected || !Iteration || !Fits(Iteration, nullptr))
	{
		BreakAllNodeLinks();
		return;
	}

	static const FName TaskNameParam(TEXT("TaskName"));
	static const FName BuildIterationParam(TEXT("BuildIteration"));

	bool bSucceeded = true;
	UK2Node_CallFunction* CallNode = SpawnQueueCall(CompilerContext, SourceGraph,
		GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueForEach), TaskNameParam, bSucceeded, true);

	if (CallNode)
	{
		const UEdGraphPin* ItemsPin = FindPin(PN_Items, EGPD_Input);
		if (UEdGraphPin* CallItemsPin = CallNode->FindPin(PN_Items, EGPD_Input))
		{
			CallItemsPin->PinType.PinCategory = ItemsPin->PinType.PinCategory;
			CallItemsPin->PinType.PinSubCategory = ItemsPin->PinType.PinSubCategory;
			CallItemsPin->PinType.PinSubCategoryObject = ItemsPin->PinType.PinSubCategoryObject;
			CallItemsPin->PinType.PinSubCategoryMemberReference = ItemsPin->PinType.PinSubCategoryMemberReference;
		}

		MoveToCall(CompilerContext, CallNode, PN_Items, PN_Items, bSucceeded);
		bSucceeded &= ConnectEvent(CompilerContext, SourceGraph, CallNode->FindPin(BuildIterationParam, EGPD_Input), ResolvedName);

		if (!bSucceeded)
		{
			ReportExpansionFailed(CompilerContext);
		}
	}

	BreakAllNodeLinks();
}

FText UK2Node_QueueControlFlowForEach::GetTooltipText() const
{
	return LOCTEXT("Tooltip",
		"Queues a loop that runs Iteration's steps once for every item of Items.\n\n"
		"Iteration is picked from this Blueprint's own functions and custom events. It takes a Control Flow Loop and runs once per item: "
		"queue that iteration's steps onto the loop's Get Body. On the loop, Get Current Item returns the item and Get Iteration Index its index.\n\n"
		"Items is copied when the step is queued.");
}

FText UK2Node_QueueControlFlowForEach::GetKeywords() const
{
	return LOCTEXT("Keywords", "control flow queue for each foreach loop array items iterate");
}

FSlateIcon UK2Node_QueueControlFlowForEach::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static const FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), TEXT("GraphEditor.Macro.Loop_16x"));
	return Icon;
}

FText UK2Node_QueueControlFlowForEach::GetNoFunctionsLabel(const UEdGraphPin&) const
{
	return LOCTEXT("NoFunctions", "(no functions or events on this Blueprint take a Control Flow Loop yet)");
}

FText UK2Node_QueueControlFlowForEach::GetFunctionPinHint(const UEdGraphPin&) const
{
	return LOCTEXT("PickHint",
		"Functions and custom events on this Blueprint that take a Control Flow Loop.\n"
		"Runs once per item: queue that iteration's steps onto the loop's Get Body, and read the item with the loop's Get Current Item.");
}

#undef LOCTEXT_NAMESPACE
