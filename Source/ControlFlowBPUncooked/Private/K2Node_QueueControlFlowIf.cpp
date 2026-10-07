#include "K2Node_QueueControlFlowIf.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPNodeUtils.h"

#include "BlueprintActionDatabaseRegistrar.h"
#include "BlueprintNodeSpawner.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "KismetCompiler.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "K2Node_QueueControlFlowIf"

const FName UK2Node_QueueControlFlowIf::PN_Target(TEXT("Target"));
const FName UK2Node_QueueControlFlowIf::PN_ConditionFunction(TEXT("ConditionFunction"));
const FName UK2Node_QueueControlFlowIf::PN_ThenFunction(TEXT("ThenFunction"));
const FName UK2Node_QueueControlFlowIf::PN_ElseFunction(TEXT("ElseFunction"));
const FName UK2Node_QueueControlFlowIf::PN_StepName(TEXT("StepName"));
const FName UK2Node_QueueControlFlowIf::PN_Flow(TEXT("Flow"));

namespace UE::ControlFlowBP::QueueIfNode
{
	static TConstArrayView<FName> GetFunctionPins()
	{
		static const FName Pins[] =
		{
			UK2Node_QueueControlFlowIf::PN_ConditionFunction,
			UK2Node_QueueControlFlowIf::PN_ThenFunction,
			UK2Node_QueueControlFlowIf::PN_ElseFunction,
		};
		return Pins;
	}

	static FText GetPinLabel(FName PinName)
	{
		if (PinName == UK2Node_QueueControlFlowIf::PN_ConditionFunction)
		{
			return LOCTEXT("ConditionLabel", "Condition");
		}

		return PinName == UK2Node_QueueControlFlowIf::PN_ThenFunction ? LOCTEXT("ThenLabel", "Then") : LOCTEXT("ElseLabel", "Else");
	}
}

void UK2Node_QueueControlFlowIf::AllocateDefaultPins()
{
	CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);

	UEdGraphPin* TargetPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Object, UControlFlowBP::StaticClass(), PN_Target);
	TargetPin->PinToolTip = LOCTEXT("TargetTooltip", "The flow to queue the If onto.").ToString();

	UEdGraphPin* ConditionPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, PN_ConditionFunction);
	ConditionPin->PinFriendlyName = LOCTEXT("ConditionPin", "Condition");
	ConditionPin->PinToolTip = LOCTEXT("ConditionTooltip", "The function on this Blueprint that decides: it takes nothing and returns a bool. Asked when the step is reached.").ToString();
	ConditionPin->bNotConnectable = true;

	UEdGraphPin* ThenPin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, PN_ThenFunction);
	ThenPin->PinFriendlyName = LOCTEXT("ThenPin", "Then");
	ThenPin->PinToolTip = LOCTEXT("ThenTooltip", "The function or custom event that queues the steps to run when Condition is true. None runs nothing.").ToString();
	ThenPin->bNotConnectable = true;

	UEdGraphPin* ElsePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Name, PN_ElseFunction);
	ElsePin->PinFriendlyName = LOCTEXT("ElsePin", "Else");
	ElsePin->PinToolTip = LOCTEXT("ElseTooltip", "The function or custom event that queues the steps to run when Condition is false. None runs nothing.").ToString();
	ElsePin->bNotConnectable = true;

	UEdGraphPin* NamePin = CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_String, PN_StepName);
	NamePin->PinFriendlyName = LOCTEXT("StepNamePin", "Step Name");
	NamePin->PinToolTip = LOCTEXT("StepNameTooltip", "Shown in logs and Get Current Step Path. Defaults to the Condition's name.").ToString();
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

FControlFlowPickedFunction* UK2Node_QueueControlFlowIf::FindPick(FName PinName)
{
	if (PinName == PN_ConditionFunction)
	{
		return &Condition;
	}

	if (PinName == PN_ThenFunction)
	{
		return &Then;
	}

	if (PinName == PN_ElseFunction)
	{
		return &Else;
	}

	return nullptr;
}

const FControlFlowPickedFunction* UK2Node_QueueControlFlowIf::FindPick(FName PinName) const
{
	return const_cast<UK2Node_QueueControlFlowIf*>(this)->FindPick(PinName);
}

UFunction* UK2Node_QueueControlFlowIf::ResolvePick(const FControlFlowPickedFunction& Pick, FName* OutResolvedName) const
{
	return UE::ControlFlowBP::NodeUtils::ResolveFunction(FBlueprintEditorUtils::FindBlueprintForNode(this), Pick.Name, Pick.Guid, OutResolvedName);
}

bool UK2Node_QueueControlFlowIf::Fits(FName PinName, const UFunction* Function, FString* OutWhyNot)
{
	FString Why;
	bool bFits = false;

	if (PinName == PN_ConditionFunction)
	{
		bFits = UControlFlowBP::IsConditionFunction(Function, Why);
	}
	else
	{
		bFits = UControlFlowBP::DeduceStepKind(Function, Why) == EControlFlowStepKind::SubFlow;
		if (!bFits && Why.IsEmpty())
		{
			Why = TEXT("a case takes a single Control Flow to queue its steps onto");
		}
	}

	if (OutWhyNot)
	{
		*OutWhyNot = MoveTemp(Why);
	}

	return bFits;
}

UFunction* UK2Node_QueueControlFlowIf::GetPickSignature(FName PinName)
{
	static const FName ConditionParam(TEXT("Condition"));
	static const FName ThenParam(TEXT("Then"));
	static const FName ElseParam(TEXT("Else"));

	FName Param;
	if (PinName == PN_ConditionFunction)
	{
		Param = ConditionParam;
	}
	else if (PinName == PN_ThenFunction)
	{
		Param = ThenParam;
	}
	else if (PinName == PN_ElseFunction)
	{
		Param = ElseParam;
	}
	else
	{
		return nullptr;
	}

	const UFunction* QueueIf = UControlFlowBP::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueIf));
	const FDelegateProperty* DelegateParam = QueueIf ? CastField<FDelegateProperty>(QueueIf->FindPropertyByName(Param)) : nullptr;
	return DelegateParam ? DelegateParam->SignatureFunction.Get() : nullptr;
}

void UK2Node_QueueControlFlowIf::SetPick(FName PinName, FName FunctionName)
{
	FControlFlowPickedFunction* Pick = FindPick(PinName);
	if (!Pick)
	{
		return;
	}

	Pick->Name = FunctionName;
	Pick->Guid = UE::ControlFlowBP::NodeUtils::FindFunctionGuid(FBlueprintEditorUtils::FindBlueprintForNode(this), FunctionName);

	if (UEdGraphPin* Pin = FindPin(PinName, EGPD_Input))
	{
		Pin->DefaultValue = FunctionName.IsNone() ? FString() : FunctionName.ToString();
	}
}

void UK2Node_QueueControlFlowIf::MirrorPropertiesToPins()
{
	for (const FName PinName : UE::ControlFlowBP::QueueIfNode::GetFunctionPins())
	{
		UEdGraphPin* Pin = FindPin(PinName, EGPD_Input);
		const FControlFlowPickedFunction* Pick = FindPick(PinName);
		if (Pin && Pick)
		{
			Pin->DefaultValue = Pick->Name.IsNone() ? FString() : Pick->Name.ToString();
		}
	}
}

void UK2Node_QueueControlFlowIf::PinDefaultValueChanged(UEdGraphPin* Pin)
{
	Super::PinDefaultValueChanged(Pin);

	const FControlFlowPickedFunction* Pick = Pin ? FindPick(Pin->PinName) : nullptr;
	if (!Pick)
	{
		return;
	}

	const FName NewName = Pin->DefaultValue.IsEmpty() ? NAME_None : FName(*Pin->DefaultValue);
	if (NewName != Pick->Name)
	{
		Modify();
		SetPick(Pin->PinName, NewName);
	}
}

void UK2Node_QueueControlFlowIf::ReconstructNode()
{
	FollowRenamedPicks();

	Super::ReconstructNode();
}

void UK2Node_QueueControlFlowIf::PostReconstructNode()
{
	Super::PostReconstructNode();

	MirrorPropertiesToPins();
}

FName UK2Node_QueueControlFlowIf::FindRenamedName(const FControlFlowPickedFunction& Pick) const
{
	FName ResolvedName;
	return (!Pick.Name.IsNone() && ResolvePick(Pick, &ResolvedName) && ResolvedName != Pick.Name) ? ResolvedName : NAME_None;
}

bool UK2Node_QueueControlFlowIf::HasRenamedPicks() const
{
	return !FindRenamedName(Condition).IsNone() || !FindRenamedName(Then).IsNone() || !FindRenamedName(Else).IsNone();
}

void UK2Node_QueueControlFlowIf::FollowRenamedPicks()
{
	for (FControlFlowPickedFunction* Pick : { &Condition, &Then, &Else })
	{
		const FName NewName = FindRenamedName(*Pick);
		if (!NewName.IsNone())
		{
			Pick->Name = NewName;
		}
	}
}

void UK2Node_QueueControlFlowIf::HandleFunctionRenamed(UBlueprint* InBlueprint, UClass* InFunctionClass, UEdGraph* InGraph, const FName& InOldFuncName, const FName& InNewFuncName)
{
	Super::HandleFunctionRenamed(InBlueprint, InFunctionClass, InGraph, InOldFuncName, InNewFuncName);

	if (!ReferencesFunction(InOldFuncName, InFunctionClass))
	{
		return;
	}

	Modify();
	for (FControlFlowPickedFunction* Pick : { &Condition, &Then, &Else })
	{
		if (Pick->Name == InOldFuncName)
		{
			Pick->Name = InNewFuncName;
		}
	}

	MirrorPropertiesToPins();
}

void UK2Node_QueueControlFlowIf::ClearCachedBlueprintData(UBlueprint* Blueprint)
{
	Super::ClearCachedBlueprintData(Blueprint);

	if (HasRenamedPicks())
	{
		Modify();
		FollowRenamedPicks();
		MirrorPropertiesToPins();
	}
}

bool UK2Node_QueueControlFlowIf::ReferencesFunction(const FName& InFunctionName, const UStruct* InScope) const
{
	using namespace UE::ControlFlowBP;

	if (InFunctionName.IsNone() || (Condition.Name != InFunctionName && Then.Name != InFunctionName && Else.Name != InFunctionName))
	{
		return false;
	}

	return NodeUtils::IsClassOrChildOf(NodeUtils::GetSearchClass(FBlueprintEditorUtils::FindBlueprintForNode(this)), InScope);
}

void UK2Node_QueueControlFlowIf::ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const
{
	using namespace UE::ControlFlowBP::QueueIfNode;

	Super::ValidateNodeDuringCompilation(MessageLog);

	if (Condition.Name.IsNone())
	{
		MessageLog.Error(*LOCTEXT("NoCondition", "@@: pick the Condition - a function that returns a bool.").ToString(), this);
	}

	for (const FName PinName : GetFunctionPins())
	{
		const FControlFlowPickedFunction& Pick = *FindPick(PinName);
		if (Pick.Name.IsNone())
		{
			continue;
		}

		FName ResolvedName;
		const UFunction* Function = ResolvePick(Pick, &ResolvedName);
		if (!Function)
		{
			MessageLog.Error(*FText::Format(
				LOCTEXT("MissingFunction", "@@: the {0} function '{1}' does not exist on this Blueprint."),
				GetPinLabel(PinName), FText::FromName(Pick.Name)).ToString(), this);
			continue;
		}

		FString Why;
		if (!Fits(PinName, Function, &Why))
		{
			MessageLog.Error(*FText::Format(
				LOCTEXT("DoesNotFit", "@@: '{0}' cannot be used as {1} - {2}."),
				FText::FromName(ResolvedName), GetPinLabel(PinName), FText::FromString(Why)).ToString(), this);
		}
	}

	if (Then.Name.IsNone() && Else.Name.IsNone())
	{
		MessageLog.Warning(*LOCTEXT("NoCases", "@@ has neither Then nor Else, so whatever the Condition answers, nothing runs.").ToString(), this);
	}
}

void UK2Node_QueueControlFlowIf::ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph)
{
	using namespace UE::ControlFlowBP::QueueIfNode;

	Super::ExpandNode(CompilerContext, SourceGraph);

	UEdGraphPin* TargetPin = FindPin(PN_Target);
	if (!TargetPin || TargetPin->LinkedTo.Num() == 0)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NoTarget", "@@ needs a Control Flow on its @@ pin.").ToString(), this, TargetPin);

		BreakAllNodeLinks();
		return;
	}

	TMap<FName, FName> ResolvedNames;
	for (const FName PinName : GetFunctionPins())
	{
		const FControlFlowPickedFunction& Pick = *FindPick(PinName);
		if (Pick.Name.IsNone())
		{
			continue;
		}

		FName ResolvedName;
		const UFunction* Function = ResolvePick(Pick, &ResolvedName);
		if (!Function || !Fits(PinName, Function))
		{
			BreakAllNodeLinks();
			return;
		}

		ResolvedNames.Add(PinName, ResolvedName);
	}

	if (!ResolvedNames.Contains(PN_ConditionFunction))
	{
		BreakAllNodeLinks();
		return;
	}

	UFunction* QueueFunction = UControlFlowBP::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueIfByName));
	if (!QueueFunction)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("NoQueueFunction", "@@: internal error - the Control Flow queue function is missing.").ToString(), this);
		BreakAllNodeLinks();
		return;
	}

	const UEdGraphSchema_K2* Schema = CompilerContext.GetSchema();
	bool bSucceeded = true;

	UK2Node_CallFunction* CallNode = CompilerContext.SpawnIntermediateNode<UK2Node_CallFunction>(this, SourceGraph);
	CallNode->SetFromFunction(QueueFunction);
	CallNode->AllocateDefaultPins();

	const auto MoveToCall = [this, CallNode, &CompilerContext, &bSucceeded](FName FromPinName, FName ToPinName) -> void
	{
		UEdGraphPin* FromPin = FindPin(FromPinName);
		UEdGraphPin* ToPin = CallNode->FindPin(ToPinName);
		if (!FromPin || !ToPin)
		{
			bSucceeded = false;
			return;
		}

		bSucceeded &= CompilerContext.MovePinLinksToIntermediate(*FromPin, *ToPin).CanSafeConnect();
	};

	static const FName OwnerParam(TEXT("Owner"));
	static const FName TaskNameParam(TEXT("TaskName"));

	MoveToCall(UEdGraphSchema_K2::PN_Execute, UEdGraphSchema_K2::PN_Execute);
	MoveToCall(UEdGraphSchema_K2::PN_Then, UEdGraphSchema_K2::PN_Then);
	MoveToCall(PN_Target, UEdGraphSchema_K2::PN_Self);
	MoveToCall(PN_Flow, UEdGraphSchema_K2::PN_ReturnValue);
	MoveToCall(PN_StepName, TaskNameParam);

	for (const FName PinName : GetFunctionPins())
	{
		UEdGraphPin* NamePin = CallNode->FindPin(PinName, EGPD_Input);
		if (!NamePin)
		{
			bSucceeded = false;
			continue;
		}

		const FName* ResolvedName = ResolvedNames.Find(PinName);
		NamePin->DefaultValue = ResolvedName ? ResolvedName->ToString() : FString();
	}

	UK2Node_Self* SelfNode = CompilerContext.SpawnIntermediateNode<UK2Node_Self>(this, SourceGraph);
	SelfNode->AllocateDefaultPins();

	UEdGraphPin* OwnerPin = CallNode->FindPin(OwnerParam, EGPD_Input);
	bSucceeded &= OwnerPin && Schema->TryCreateConnection(SelfNode->FindPinChecked(UEdGraphSchema_K2::PN_Self), OwnerPin);

	if (!bSucceeded)
	{
		CompilerContext.MessageLog.Error(*LOCTEXT("ExpansionFailed", "@@: internal error while expanding the If.").ToString(), this);
	}

	BreakAllNodeLinks();
}

FText UK2Node_QueueControlFlowIf::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (TitleType == ENodeTitleType::MenuTitle || Condition.Name.IsNone())
	{
		return LOCTEXT("Title", "Queue If");
	}

	return FText::Format(LOCTEXT("TitleWithCondition", "Queue If\n{0}"), FText::FromName(Condition.Name));
}

FText UK2Node_QueueControlFlowIf::GetTooltipText() const
{
	return LOCTEXT("Tooltip",
		"Queues a step that asks Condition when it is reached, then runs the steps Then queues if the answer is true, or the steps Else queues if it is false.\n\n"
		"All three are picked from this Blueprint's own functions. Condition takes nothing and returns a bool - a pure function will do, and its output may have "
		"any name. Then and Else are functions or custom events that take a Control Flow to queue their steps onto; leave either at None to run nothing.");
}

FText UK2Node_QueueControlFlowIf::GetKeywords() const
{
	return LOCTEXT("Keywords", "control flow queue if branch else condition bool function");
}

FLinearColor UK2Node_QueueControlFlowIf::GetNodeTitleColor() const
{
	return FLinearColor(0.190525f, 0.583898f, 1.0f);
}

FSlateIcon UK2Node_QueueControlFlowIf::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = GetNodeTitleColor();
	static const FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), TEXT("GraphEditor.Branch_16x"));
	return Icon;
}

UObject* UK2Node_QueueControlFlowIf::GetJumpTargetForDoubleClick() const
{
	FName ResolvedName;
	ResolvePick(Condition, &ResolvedName);

	return UE::ControlFlowBP::NodeUtils::FindFunctionDefinition(FBlueprintEditorUtils::FindBlueprintForNode(this), ResolvedName);
}

bool UK2Node_QueueControlFlowIf::IsCompatibleWithGraph(const UEdGraph* TargetGraph) const
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

void UK2Node_QueueControlFlowIf::GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const
{
	UClass* ActionKey = GetClass();
	if (ActionRegistrar.IsOpenForRegistration(ActionKey))
	{
		UBlueprintNodeSpawner* NodeSpawner = UBlueprintNodeSpawner::Create(ActionKey);
		check(NodeSpawner);
		ActionRegistrar.AddBlueprintAction(ActionKey, NodeSpawner);
	}
}

FText UK2Node_QueueControlFlowIf::GetMenuCategory() const
{
	return LOCTEXT("MenuCategory", "Control Flow|Queue");
}

bool UK2Node_QueueControlFlowIf::IsFunctionPin(const UEdGraphPin& Pin) const
{
	return Pin.Direction == EGPD_Input && FindPick(Pin.PinName) != nullptr;
}

bool UK2Node_QueueControlFlowIf::IsFunctionOptional(const UEdGraphPin& Pin) const
{
	return Pin.PinName != PN_ConditionFunction;
}

TArray<FName> UK2Node_QueueControlFlowIf::GetPickableFunctions(const UEdGraphPin& Pin) const
{
	using namespace UE::ControlFlowBP;

	TArray<FName> Names;

	const UClass* Class = NodeUtils::GetSearchClass(FBlueprintEditorUtils::FindBlueprintForNode(this));
	if (!Class || !FindPick(Pin.PinName))
	{
		return Names;
	}

	for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		if (NodeUtils::IsPickableFunction(*It, Class) && Fits(Pin.PinName, *It))
		{
			Names.AddUnique(It->GetFName());
		}
	}

	Names.Sort(FNameLexicalLess());
	return Names;
}

FText UK2Node_QueueControlFlowIf::GetCreateFunctionLabel(const UEdGraphPin& Pin) const
{
	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this);
	if (!Blueprint || !GetPickSignature(Pin.PinName))
	{
		return FText::GetEmpty();
	}

	if (Pin.PinName == PN_ConditionFunction)
	{
		return LOCTEXT("CreateConditionFunction", "[Create a matching Condition function]");
	}

	const FText Label = UE::ControlFlowBP::QueueIfNode::GetPinLabel(Pin.PinName);
	return FBlueprintEditorUtils::FindEventGraph(Blueprint)
		? FText::Format(LOCTEXT("CreateCaseEvent", "[Create a matching {0} event]"), Label)
		: FText::Format(LOCTEXT("CreateCaseFunction", "[Create a matching {0} function]"), Label);
}

UObject* UK2Node_QueueControlFlowIf::CreateFunctionFor(const UEdGraphPin& Pin)
{
	using namespace UE::ControlFlowBP;

	const FName PinName = Pin.PinName;

	UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(this);
	UFunction* Signature = GetPickSignature(PinName);
	if (!Blueprint || !Signature || !GetGraph())
	{
		return nullptr;
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateMatchingFunction", "Create Matching If Function"));
	Modify();

	FString BaseName = TEXT("IfCondition");
	if (PinName != PN_ConditionFunction)
	{
		BaseName = (Condition.Name.IsNone() ? FString(TEXT("If")) : Condition.Name.ToString() + TEXT("_"))
			+ (PinName == PN_ThenFunction ? TEXT("Then") : TEXT("Else"));
	}

	if (PinName != PN_ConditionFunction && FBlueprintEditorUtils::FindEventGraph(Blueprint))
	{
		UK2Node_CustomEvent* Event = NodeUtils::AddMatchingEvent(Blueprint, Signature, BaseName);
		if (!Event)
		{
			return nullptr;
		}

		SetPick(PinName, Event->CustomFunctionName);

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Event;
	}

	UEdGraph* Graph = NodeUtils::AddMatchingFunction(Blueprint, Signature, BaseName, GetGraph());
	if (!Graph)
	{
		return nullptr;
	}

	SetPick(PinName, Graph->GetFName());
	return Graph;
}

FText UK2Node_QueueControlFlowIf::GetNoFunctionsLabel(const UEdGraphPin& Pin) const
{
	return Pin.PinName == PN_ConditionFunction
		? LOCTEXT("NoConditionFunctions", "(no functions on this Blueprint return just a bool yet)")
		: LOCTEXT("NoCaseFunctions", "(no functions or events on this Blueprint take a Control Flow yet)");
}

FText UK2Node_QueueControlFlowIf::GetFunctionPinHint(const UEdGraphPin& Pin) const
{
	if (Pin.PinName == PN_ConditionFunction)
	{
		return LOCTEXT("ConditionHint",
			"Functions on this Blueprint that take nothing and return a bool - pure or not, whatever the output is called.\n"
			"Asked when the step is reached.");
	}

	return Pin.PinName == PN_ThenFunction
		? LOCTEXT("ThenHint", "Functions and custom events on this Blueprint that take a Control Flow.\nWhen Condition is true, it queues onto that flow the steps to run.")
		: LOCTEXT("ElseHint", "Functions and custom events on this Blueprint that take a Control Flow.\nWhen Condition is false, it queues onto that flow the steps to run.");
}

UObject* UK2Node_QueueControlFlowIf::GetFunctionDefinition(const UEdGraphPin& Pin) const
{
	const FControlFlowPickedFunction* Pick = FindPick(Pin.PinName);
	if (!Pick)
	{
		return nullptr;
	}

	FName ResolvedName;
	ResolvePick(*Pick, &ResolvedName);

	return UE::ControlFlowBP::NodeUtils::FindFunctionDefinition(FBlueprintEditorUtils::FindBlueprintForNode(this), ResolvedName);
}

#undef LOCTEXT_NAMESPACE
