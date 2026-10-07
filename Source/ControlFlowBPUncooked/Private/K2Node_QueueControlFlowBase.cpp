#include "K2Node_QueueControlFlowBase.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPNodeUtils.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "K2Node_QueueControlFlowBase"

const FName UK2Node_QueueControlFlowBase::PN_Target(TEXT("Target"));
const FName UK2Node_QueueControlFlowBase::PN_StepName(TEXT("StepName"));
const FName UK2Node_QueueControlFlowBase::PN_Flow(TEXT("Flow"));

void UK2Node_QueueControlFlowBase::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);

	UEdGraphPin* TargetPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UControlFlowBP::StaticClass(), PN_Target);
	TargetPin->PinToolTip = LOCTEXT("TargetTooltip", "The flow to queue the step onto.").ToString();

	CreateStepPins();

	UEdGraphPin* NamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, PN_StepName);
	NamePin->PinFriendlyName = LOCTEXT("StepNamePin", "Step Name");
	NamePin->PinToolTip = FText::Format(LOCTEXT("StepNameTooltip", "Shown in logs and Get Current Step Path. Defaults to the {0}'s name."), GetPickLabel()).ToString();
	NamePin->bAdvancedView = true;

	UEdGraphPin* FlowPin = CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Object, UControlFlowBP::StaticClass(), PN_Flow);
	FlowPin->PinToolTip = LOCTEXT("FlowTooltip", "The same flow, so steps chain.").ToString();

	MirrorPropertiesToPins();

	if (AdvancedPinDisplay == ENodeAdvancedPins::NoPins)
	{
		AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
	}

	Super::AllocateDefaultPins();
}

UEdGraphPin* UK2Node_QueueControlFlowBase::CreatePickPin(const FText& FriendlyName, const FText& Tooltip)
{
	UEdGraphPin* Pin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, GetPickPinName());
	Pin->PinFriendlyName = FriendlyName;
	Pin->PinToolTip = Tooltip.ToString();
	Pin->bNotConnectable = true;
	return Pin;
}

FText UK2Node_QueueControlFlowBase::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle || Picked.Name.IsNone())
	{
		return GetBaseTitle();
	}

	return FText::Format(LOCTEXT("TitleWithPick", "{0}\n{1}"), GetBaseTitle(), FText::FromName(Picked.Name));
}

FLinearColor UK2Node_QueueControlFlowBase::GetNodeTitleColor() const
{
	return FLinearColor(0.190525f, 0.583898f, 1.0f);
}

FSlateIcon UK2Node_QueueControlFlowBase::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static const FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), TEXT("Kismet.AllClasses.FunctionIcon"));
	return Icon;
}

void UK2Node_QueueControlFlowBase::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	Super::PinDefaultValueChanged(Pin);

	if (!Pin || Pin->PinName != GetPickPinName())
	{
		return;
	}

	const FName NewName = Pin->DefaultValue.IsEmpty() ? NAME_None : FName(*Pin->DefaultValue);
	if (NewName != Picked.Name)
	{
		Modify();
		SetPick(NewName);
	}
}

void UK2Node_QueueControlFlowBase::ReconstructNode()
{
	const FName ResolvedName = ResolvePickedName();
	if (!ResolvedName.IsNone() && ResolvedName != Picked.Name)
	{
		Picked.Name = ResolvedName;
	}

	Super::ReconstructNode();
}

void UK2Node_QueueControlFlowBase::PostReconstructNode()
{
	Super::PostReconstructNode();

	MirrorPropertiesToPins();
}

void UK2Node_QueueControlFlowBase::HandleFunctionRenamed(UBlueprint* InBlueprint, UClass* InFunctionClass, UEdGraph* InGraph, const FName& InOldFuncName, const FName& InNewFuncName)
{
	Super::HandleFunctionRenamed(InBlueprint, InFunctionClass, InGraph, InOldFuncName, InNewFuncName);

	if (ReferencesFunction(InOldFuncName, InFunctionClass))
	{
		Modify();
		Picked.Name = InNewFuncName;
		MirrorPropertiesToPins();
	}
}

void UK2Node_QueueControlFlowBase::ClearCachedBlueprintData(UBlueprint* Blueprint)
{
	Super::ClearCachedBlueprintData(Blueprint);

	if (Picked.Name.IsNone())
	{
		return;
	}

	const FName ResolvedName = ResolvePickedName();
	if (!ResolvedName.IsNone() && ResolvedName != Picked.Name)
	{
		Modify();
		Picked.Name = ResolvedName;
		MirrorPropertiesToPins();
	}
}

bool UK2Node_QueueControlFlowBase::ReferencesFunction(const FName& InFunctionName, const UStruct* InScope) const
{
	using namespace UE::ControlFlowBP;

	return PicksFunction()
		&& !Picked.Name.IsNone()
		&& Picked.Name == InFunctionName
		&& NodeUtils::IsClassOrChildOf(NodeUtils::GetSearchClass(FBlueprintEditorUtils::FindBlueprintForNode(this)), InScope);
}

void UK2Node_QueueControlFlowBase::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	Super::ValidateNodeDuringCompilation(MessageLog);

	if (Picked.Name.IsNone())
	{
		MessageLog.Error(*FText::Format(LOCTEXT("NothingPicked", "@@: pick the {0}."), GetPickLabel()).ToString(), this);
		return;
	}

	ValidatePick(MessageLog);
}

void UK2Node_QueueControlFlowBase::ValidatePick(FCompilerResultsLog& MessageLog) const
{
	FName ResolvedName;
	const UFunction* Function = ResolvePickedFunction(&ResolvedName);
	if (!Function)
	{
		MessageLog.Error(*FText::Format(
			LOCTEXT("MissingFunction", "@@: the {0} function '{1}' does not exist on this Blueprint."),
			GetPickLabel(), FText::FromName(ResolvedName)).ToString(), this);
		return;
	}

	FString Why;
	if (!Fits(Function, &Why))
	{
		MessageLog.Error(*FText::Format(
			LOCTEXT("DoesNotFit", "@@: '{0}' cannot be used as the {1} - {2}."),
			FText::FromName(ResolvedName), GetPickLabel(), FText::FromString(Why)).ToString(), this);
	}
}

bool UK2Node_QueueControlFlowBase::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
{
	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForGraph(TargetGraph);
	if (!Blueprint
		|| Blueprint->BlueprintType == BPTYPE_FunctionLibrary
		|| Blueprint->BlueprintType == BPTYPE_MacroLibrary
		|| Blueprint->BlueprintType == BPTYPE_Interface)
	{
		return false;
	}

	return Super::IsCompatibleWithGraph(TargetGraph);
}

UObject* UK2Node_QueueControlFlowBase::GetJumpTargetForDoubleClick() const
{
	const UEdGraphPin* PickPin = FindPin(GetPickPinName(), EGPD_Input);
	return PickPin ? GetFunctionDefinition(*PickPin) : nullptr;
}

void UK2Node_QueueControlFlowBase::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(ActionKey);
		check(NodeSpawner);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_QueueControlFlowBase::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Control Flow|Queue");
}

bool UK2Node_QueueControlFlowBase::IsFunctionPin(const UEdGraphPin& Pin) const
{
	return Pin.Direction == EGPD_Input && Pin.PinName == GetPickPinName();
}

TArray<FName> UK2Node_QueueControlFlowBase::GetPickableFunctions(const UEdGraphPin& Pin) const
{
	using namespace UE::ControlFlowBP;

	TArray<FName> Names;

	const UClass* Class = NodeUtils::GetSearchClass(FBlueprintEditorUtils::FindBlueprintForNode(this));
	if (!Class || !IsFunctionPin(Pin))
	{
		return Names;
	}

	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (NodeUtils::IsPickableFunction(*It, Class) && Fits(*It, nullptr))
		{
			Names.AddUnique(It->GetFName());
		}
	}

	Names.Sort(FNameLexicalLess());
	return Names;
}

FText UK2Node_QueueControlFlowBase::GetCreateFunctionLabel(const UEdGraphPin& Pin) const
{
	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this);
	if (!Blueprint || !GetPickSignature() || !IsFunctionPin(Pin))
	{
		return FText::GetEmpty();
	}

	return (!MustBeFunction() && FBlueprintEditorUtils::FindEventGraph(Blueprint))
		? FText::Format(LOCTEXT("CreateMatchingEvent", "[Create a matching {0} event]"), GetPickLabel())
		: FText::Format(LOCTEXT("CreateMatchingFunction", "[Create a matching {0} function]"), GetPickLabel());
}

UObject* UK2Node_QueueControlFlowBase::CreateFunctionFor(const UEdGraphPin& Pin)
{
	using namespace UE::ControlFlowBP;

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this);
	UFunction* Signature = GetPickSignature();
	if (!Blueprint || !Signature || !GetGraph() || !IsFunctionPin(Pin))
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(FText::Format(LOCTEXT("CreateMatching", "Create Matching {0}"), GetPickLabel()));
	Modify();

	if (!MustBeFunction() && FBlueprintEditorUtils::FindEventGraph(Blueprint))
	{
		UK2Node_CustomEvent* Event = NodeUtils::AddMatchingEvent(Blueprint, Signature, GetNewFunctionName());
		if (!Event)
		{
			return nullptr;
		}

		SetPick(Event->CustomFunctionName);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Event;
	}

	UEdGraph* Graph = NodeUtils::AddMatchingFunction(Blueprint, Signature, GetNewFunctionName(), GetGraph());
	if (!Graph)
	{
		return nullptr;
	}

	SetPick(Graph->GetFName());
	return Graph;
}

FText UK2Node_QueueControlFlowBase::GetNoFunctionsLabel(const UEdGraphPin&) const
{
	return FText::Format(LOCTEXT("NoFunctions", "(nothing on this Blueprint can be the {0} yet)"), GetPickLabel());
}

FText UK2Node_QueueControlFlowBase::GetFunctionPinHint(const UEdGraphPin&) const
{
	return FText::Format(LOCTEXT("PickHint", "The functions and custom events on this Blueprint that can be the {0}."), GetPickLabel());
}

UObject* UK2Node_QueueControlFlowBase::GetFunctionDefinition(const UEdGraphPin& Pin) const
{
	if (!IsFunctionPin(Pin))
	{
		return nullptr;
	}

	FName ResolvedName;
	ResolvePickedFunction(&ResolvedName);

	return UE::ControlFlowBP::NodeUtils::FindFunctionDefinition(FBlueprintEditorUtils::FindBlueprintForNode(this), ResolvedName);
}

FName UK2Node_QueueControlFlowBase::ResolvePickedName() const
{
	FName ResolvedName;
	ResolvePickedFunction(&ResolvedName);
	return ResolvedName;
}

FGuid UK2Node_QueueControlFlowBase::FindPickGuid(FName Name) const
{
	return UE::ControlFlowBP::NodeUtils::FindFunctionGuid(FBlueprintEditorUtils::FindBlueprintForNode(this), Name);
}

UFunction* UK2Node_QueueControlFlowBase::ResolvePickedFunction(FName* OutResolvedName) const
{
	return UE::ControlFlowBP::NodeUtils::ResolveFunction(FBlueprintEditorUtils::FindBlueprintForNode(this), Picked.Name, Picked.Guid, OutResolvedName);
}

void UK2Node_QueueControlFlowBase::SetPick(FName Name)
{
	Picked.Name = Name;
	Picked.Guid = Name.IsNone() ? FGuid() : FindPickGuid(Name);

	if (UEdGraphPin* Pin = FindPin(GetPickPinName(), EGPD_Input))
	{
		Pin->DefaultValue = Name.IsNone() ? FString() : Name.ToString();
	}
}

void UK2Node_QueueControlFlowBase::MirrorPropertiesToPins()
{
	if (UEdGraphPin* Pin = FindPin(GetPickPinName(), EGPD_Input))
	{
		Pin->DefaultValue = Picked.Name.IsNone() ? FString() : Picked.Name.ToString();
	}
}

bool UK2Node_QueueControlFlowBase::CheckTargetConnected(FKismetCompilerContext& CompilerContext)
{
	UEdGraphPin* TargetPin = FindPin(PN_Target);
	if (TargetPin && TargetPin->LinkedTo.Num() > 0)
	{
		return true;
	}

	CompilerContext.MessageLog.Error(*LOCTEXT("NoTarget", "@@ needs a Control Flow on its @@ pin.").ToString(), this, TargetPin);
	return false;
}

UK2Node_CallFunction* UK2Node_QueueControlFlowBase::SpawnQueueCall(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, FName QueueFunctionName,
	FName StepNameParam, bool& bSucceeded, bool bArrayFunction)
{
	UFunction* QueueFunction = UControlFlowBP::StaticClass()->FindFunctionByName(QueueFunctionName);
	if (!QueueFunction)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NoQueueFunction", "@@: internal error - the Control Flow queue function is missing.").ToString(), this);
		bSucceeded = false;
		return nullptr;
	}

	UK2Node_CallFunction* CallNode = nullptr;
	if (bArrayFunction)
	{
		CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallArrayFunction>(this, SourceGraph);
	}
	else
	{
		CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	}

	CallNode->SetFromFunction(QueueFunction);
	CallNode->AllocateDefaultPins();

	MoveToCall(CompilerContext, CallNode, UEdGraphSchema_K2::PN_Execute, UEdGraphSchema_K2::PN_Execute, bSucceeded);
	MoveToCall(CompilerContext, CallNode, UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Then, bSucceeded);
	MoveToCall(CompilerContext, CallNode, PN_Target, UEdGraphSchema_K2::PN_Self, bSucceeded);
	MoveToCall(CompilerContext, CallNode, PN_Flow, UEdGraphSchema_K2::PN_ReturnValue, bSucceeded);
	MoveToCall(CompilerContext, CallNode, PN_StepName, StepNameParam, bSucceeded);
	return CallNode;
}

void UK2Node_QueueControlFlowBase::MoveToCall(FKismetCompilerContext& CompilerContext, UK2Node_CallFunction* CallNode, FName FromPinName, FName ToPinName, bool& bSucceeded)
{
	UEdGraphPin* FromPin = FindPin(FromPinName);
	UEdGraphPin* ToPin = CallNode ? CallNode->FindPin(ToPinName) : nullptr;
	if (!FromPin || !ToPin)
	{
		bSucceeded = false;
		return;
	}

	bSucceeded &= CompilerContext.MovePinLinksToIntermediate(*FromPin, *ToPin).CanSafeConnect();
}

bool UK2Node_QueueControlFlowBase::ConnectSelf(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* Pin)
{
	UK2Node_Self* SelfNode = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	SelfNode->AllocateDefaultPins();

	return Pin && CompilerContext.GetSchema()->TryCreateConnection(SelfNode->FindPinChecked(UEdGraphSchema_K2::PN_Self), Pin);
}

bool UK2Node_QueueControlFlowBase::ConnectEvent(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* DelegatePin, FName FunctionName)
{
	UK2Node_CreateDelegate* CreateDelegateNode = CompilerContext.SpawnIntermediateNode<UK2Node_CreateDelegate>(this, SourceGraph);
	CreateDelegateNode->AllocateDefaultPins();

	bool bSucceeded = DelegatePin && CompilerContext.GetSchema()->TryCreateConnection(CreateDelegateNode->GetDelegateOutPin(), DelegatePin);
	bSucceeded &= ConnectSelf(CompilerContext, SourceGraph, CreateDelegateNode->GetObjectInPin());

	CreateDelegateNode->SetFunction(FunctionName);
	return bSucceeded;
}

void UK2Node_QueueControlFlowBase::ReportExpansionFailed(FKismetCompilerContext& CompilerContext)
{
	CompilerContext.MessageLog.Error(*LOCTEXT("ExpansionFailed", "@@: internal error while expanding the step.").ToString(), this);
}

UFunction* UK2Node_QueueControlFlowBase::GetIterationSignature()
{
	static const FName BuildIterationParam(TEXT("BuildIteration"));

	const UFunction* Repeat = UControlFlowBP::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRepeat));
	const FDelegateProperty* Param = Repeat ? CastField<FDelegateProperty>(Repeat->FindPropertyByName(BuildIterationParam)) : nullptr;
	return Param ? Param->SignatureFunction.Get() : nullptr;
}

bool UK2Node_QueueControlFlowBase::FitsIteration(const UFunction* Function, FString* OutWhyNot)
{
	FString Why;
	bool bFits = UControlFlowBP::DeduceStepKind(Function, Why) == EControlFlowStepKind::Loop;

	if (bFits)
	{
		const UFunction* Signature = GetIterationSignature();
		bFits = Signature && Signature->IsSignatureCompatibleWith(Function);
		if (!bFits)
		{
			Why = TEXT("its Control Flow Loop parameter must not be passed by reference");
		}
	}
	else if (Why.IsEmpty())
	{
		Why = TEXT("an iteration takes a single Control Flow Loop to queue its steps onto");
	}

	if (OutWhyNot)
	{
		*OutWhyNot = MoveTemp(Why);
	}

	return bFits;
}

#undef LOCTEXT_NAMESPACE
