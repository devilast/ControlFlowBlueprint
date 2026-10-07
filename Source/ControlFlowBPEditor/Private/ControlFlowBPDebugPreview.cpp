#include "ControlFlowBPDebugPreview.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPEditorDebugger.h"
#include "ControlFlowBPScopes.h"
#include "K2Node_QueueControlFlowForEach.h"
#include "K2Node_QueueControlFlowIf.h"
#include "K2Node_QueueControlFlowRepeat.h"
#include "K2Node_QueueControlFlowStep.h"
#include "K2Node_QueueControlFlowWaitForDispatcher.h"
#include "K2Node_QueueControlFlowWaitUntil.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"

namespace UE::ControlFlowBP::DebugPreview
{
	static constexpr int32 MaxDepth = 6;

	/** Where the flows an event fills come from. */
	enum class ELaneSource : uint8
	{
		Tracks,
		Cases,
		Flow,
		Body
	};

	/** An event that builds a step's flows. */
	struct FBuilder
	{
		FName Event;
		ELaneSource Source = ELaneSource::Flow;
		FString LaneName;
	};

	/** What a Queue node queues. */
	struct FQueueInfo
	{
		EControlFlowBPStepType Type = EControlFlowBPStepType::Function;
		FString Name;
		FName Handler;

		TArray<FBuilder> Builders;
	};

	static const UFunction* GetCalledQueueFunction(const UEdGraphNode* Node)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
		return (Function && Function->GetOwnerClass() == UControlFlowBP::StaticClass() && Function->GetName().StartsWith(TEXT("Queue"))) ? Function : nullptr;
	}

	static bool IsCustomQueueNode(const UEdGraphNode* Node)
	{
		return Cast<UK2Node_QueueControlFlowStep>(Node) || Cast<UK2Node_QueueControlFlowIf>(Node) || Cast<UK2Node_QueueControlFlowBase>(Node);
	}

	static bool IsQueueNode(const UEdGraphNode* Node)
	{
		return IsCustomQueueNode(Node) || GetCalledQueueFunction(Node);
	}

	static const UEdGraphPin* GetFlowInputPin(const UEdGraphNode* Node)
	{
		return IsCustomQueueNode(Node)
			? Node->FindPin(UK2Node_QueueControlFlowStep::PN_Target, EGPD_Input)
			: Node->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
	}

	static const UEdGraphPin* GetFlowOutputPin(const UEdGraphNode* Node)
	{
		if (IsCustomQueueNode(Node))
		{
			return Node->FindPin(UK2Node_QueueControlFlowStep::PN_Flow, EGPD_Output);
		}

		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		return Call ? Call->GetReturnValuePin() : nullptr;
	}

	static TOptional<FString> GetLiteral(const UEdGraphNode* Node, FName PinName)
	{
		const UEdGraphPin* Pin = Node->FindPin(PinName, EGPD_Input);
		if (!Pin || Pin->LinkedTo.Num() > 0)
		{
			return {};
		}
		return Pin->DefaultValue;
	}

	static FName GetBoundFunction(const UEdGraphNode* Node, FName PinName)
	{
		const UEdGraphPin* Pin = Node->FindPin(PinName, EGPD_Input);
		for (int32 Hops = 0; Pin && Pin->LinkedTo.Num() == 1 && Hops < 64; ++Hops)
		{
			const UEdGraphPin* Source = Pin->LinkedTo[0];
			const UEdGraphNode* SourceNode = Source->GetOwningNode();

			if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(SourceNode))
			{
				Pin = Knot->GetInputPin();
				continue;
			}

			if (const UK2Node_CreateDelegate* CreateEvent = Cast<UK2Node_CreateDelegate>(SourceNode))
			{
				return CreateEvent->GetFunctionName();
			}

			const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(SourceNode);
			return (Event && Source->PinName == UK2Node_Event::DelegateOutputName) ? Event->CustomFunctionName : NAME_None;
		}

		return NAME_None;
	}

	static FString MakeStepName(const TOptional<FString>& StepName, FName Bound, const TCHAR* Fallback)
	{
		if (StepName.IsSet() && !StepName->IsEmpty())
		{
			return *StepName;
		}
		return Bound.IsNone() ? FString(Fallback) : Bound.ToString();
	}

	static void DescribeKind(EControlFlowStepKind Kind, FName Function, const TOptional<FString>& StepName, FQueueInfo& OutInfo)
	{
		OutInfo.Handler = Function;

		switch (Kind)
		{
		case EControlFlowStepKind::Wait:
			OutInfo.Type = EControlFlowBPStepType::Wait;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Wait"));
			break;

		case EControlFlowStepKind::SubFlow:
			OutInfo.Type = EControlFlowBPStepType::SubFlow;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("SubFlow"));
			OutInfo.Builders.Add({ Function, ELaneSource::Flow });
			break;

		case EControlFlowStepKind::Branch:
			OutInfo.Type = EControlFlowBPStepType::Branch;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Switch"));
			OutInfo.Builders.Add({ Function, ELaneSource::Cases });
			break;

		case EControlFlowStepKind::Fork:
			OutInfo.Type = EControlFlowBPStepType::Fork;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Parallel"));
			OutInfo.Builders.Add({ Function, ELaneSource::Tracks });
			break;

		case EControlFlowStepKind::Race:
			OutInfo.Type = EControlFlowBPStepType::Race;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Race"));
			OutInfo.Builders.Add({ Function, ELaneSource::Tracks });
			break;

		case EControlFlowStepKind::Loop:
			OutInfo.Type = EControlFlowBPStepType::Loop;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Loop"));
			OutInfo.Builders.Add({ Function, ELaneSource::Body });
			break;

		case EControlFlowStepKind::Delay:
			OutInfo.Type = EControlFlowBPStepType::Delay;
			OutInfo.Handler = NAME_None;
			OutInfo.Name = MakeStepName(StepName, NAME_None, TEXT("Delay"));
			break;

		default:
			OutInfo.Type = EControlFlowBPStepType::Function;
			OutInfo.Name = MakeStepName(StepName, Function, TEXT("Function"));
			break;
		}
	}

	static bool DescribeQueueNode(const UEdGraphNode* Node, const UBlueprint* Blueprint, FQueueInfo& OutInfo)
	{
		if (const UK2Node_QueueControlFlowStep* QueueStep = Cast<UK2Node_QueueControlFlowStep>(Node))
		{
			DescribeKind(QueueStep->GetStepKind(), QueueStep->GetStepFunctionName(), GetLiteral(Node, UK2Node_QueueControlFlowStep::PN_StepName), OutInfo);
			return true;
		}

		if (const UK2Node_QueueControlFlowIf* QueueIf = Cast<UK2Node_QueueControlFlowIf>(Node))
		{
			OutInfo.Type = EControlFlowBPStepType::If;
			OutInfo.Handler = QueueIf->GetConditionFunction();
			OutInfo.Name = MakeStepName(GetLiteral(Node, UK2Node_QueueControlFlowIf::PN_StepName), OutInfo.Handler, TEXT("If"));
			OutInfo.Builders.Add({ QueueIf->GetThenFunction(), ELaneSource::Flow, TEXT("Then") });
			OutInfo.Builders.Add({ QueueIf->GetElseFunction(), ELaneSource::Flow, TEXT("Else") });
			return true;
		}

		if (const UK2Node_QueueControlFlowBase* Custom = Cast<UK2Node_QueueControlFlowBase>(Node))
		{
			const TOptional<FString> StepName = GetLiteral(Node, UK2Node_QueueControlFlowBase::PN_StepName);
			const FName Picked = Custom->GetPickedName();

			if (Cast<UK2Node_QueueControlFlowWaitUntil>(Node))
			{
				OutInfo.Type = EControlFlowBPStepType::Wait;
				OutInfo.Handler = Picked;
				OutInfo.Name = MakeStepName(StepName, Picked, TEXT("WaitUntil"));
			}
			else if (Cast<UK2Node_QueueControlFlowWaitForDispatcher>(Node))
			{
				OutInfo.Type = EControlFlowBPStepType::Wait;
				OutInfo.Name = MakeStepName(StepName, Picked, TEXT("WaitForDispatcher"));
			}
			else if (Cast<UK2Node_QueueControlFlowRepeat>(Node) || Cast<UK2Node_QueueControlFlowForEach>(Node))
			{
				OutInfo.Type = EControlFlowBPStepType::Loop;
				OutInfo.Handler = Picked;
				OutInfo.Name = MakeStepName(StepName, Picked, TEXT("Loop"));
				OutInfo.Builders.Add({ Picked, ELaneSource::Body });
			}
			else
			{
				return false;
			}

			return true;
		}

		const UFunction* Function = GetCalledQueueFunction(Node);
		if (!Function)
		{
			return false;
		}

		static const FName StepName(TEXT("StepName"));
		static const FName TaskName(TEXT("TaskName"));
		static const FName Step(TEXT("Step"));
		static const FName Condition(TEXT("Condition"));
		static const FName Populate(TEXT("Populate"));
		static const FName Define(TEXT("Define"));
		static const FName BuildIteration(TEXT("BuildIteration"));

		const FName Called = Function->GetFName();
		const auto Bind = [Node](FName PinName) { return GetBoundFunction(Node, PinName); };

		if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueFunction) || Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSimpleFunction))
		{
			OutInfo.Type = EControlFlowBPStepType::Function;
			OutInfo.Handler = Bind(Step);
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), OutInfo.Handler, TEXT("Function"));
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWait))
		{
			OutInfo.Type = EControlFlowBPStepType::Wait;
			OutInfo.Handler = Bind(Step);
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), OutInfo.Handler, TEXT("Wait"));
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWaitUntil))
		{
			OutInfo.Type = EControlFlowBPStepType::Wait;
			OutInfo.Handler = Bind(Condition);
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), OutInfo.Handler, TEXT("WaitUntil"));
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWaitForEventDispatcher))
		{
			static const FName DispatcherName(TEXT("DispatcherName"));
			const TOptional<FString> Dispatcher = GetLiteral(Node, DispatcherName);

			OutInfo.Type = EControlFlowBPStepType::Wait;
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), Dispatcher.IsSet() ? FName(**Dispatcher) : NAME_None, TEXT("WaitForDispatcher"));
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueDelay))
		{
			OutInfo.Type = EControlFlowBPStepType::Delay;
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), NAME_None, TEXT("Delay"));
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSubFlow))
		{
			OutInfo.Type = EControlFlowBPStepType::SubFlow;
			OutInfo.Handler = Bind(Populate);
			OutInfo.Name = MakeStepName(GetLiteral(Node, TaskName), OutInfo.Handler, TEXT("SubFlow"));
			OutInfo.Builders.Add({ OutInfo.Handler, ELaneSource::Flow });
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueIf))
		{
			static const FName Then(TEXT("Then"));
			static const FName Else(TEXT("Else"));

			OutInfo.Type = EControlFlowBPStepType::If;
			OutInfo.Handler = Bind(Condition);
			OutInfo.Name = MakeStepName(GetLiteral(Node, TaskName), OutInfo.Handler, TEXT("If"));
			OutInfo.Builders.Add({ Bind(Then), ELaneSource::Flow, TEXT("Then") });
			OutInfo.Builders.Add({ Bind(Else), ELaneSource::Flow, TEXT("Else") });
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueBranch))
		{
			OutInfo.Type = EControlFlowBPStepType::Branch;
			OutInfo.Handler = Bind(Define);
			OutInfo.Name = MakeStepName(GetLiteral(Node, TaskName), OutInfo.Handler, TEXT("Switch"));
			OutInfo.Builders.Add({ OutInfo.Handler, ELaneSource::Cases });
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueFork) || Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRace))
		{
			const bool bRace = Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRace);
			OutInfo.Type = bRace ? EControlFlowBPStepType::Race : EControlFlowBPStepType::Fork;
			OutInfo.Handler = Bind(Define);
			OutInfo.Name = MakeStepName(GetLiteral(Node, TaskName), OutInfo.Handler, bRace ? TEXT("Race") : TEXT("Parallel"));
			OutInfo.Builders.Add({ OutInfo.Handler, ELaneSource::Tracks });
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueLoop) || Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueCombinedLoop)
			|| Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRepeat) || Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueForEach))
		{
			OutInfo.Type = EControlFlowBPStepType::Loop;
			OutInfo.Handler = Bind(BuildIteration);
			OutInfo.Name = MakeStepName(GetLiteral(Node, TaskName), OutInfo.Handler, TEXT("Loop"));
			OutInfo.Builders.Add({ OutInfo.Handler, ELaneSource::Body });
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueStep))
		{
			static const FName Target(TEXT("Target"));
			static const FName FunctionName(TEXT("FunctionName"));

			const TOptional<FString> Name = GetLiteral(Node, FunctionName);
			const FName StepFunction = Name.IsSet() ? FName(**Name) : NAME_None;
			const UEdGraphPin* TargetPin = Node->FindPin(Target, EGPD_Input);
			const UClass* Class = (Blueprint && TargetPin && TargetPin->LinkedTo.IsEmpty()) ? Blueprint->SkeletonGeneratedClass.Get() : nullptr;
			const UFunction* Resolved = (Class && !StepFunction.IsNone()) ? Class->FindFunctionByName(StepFunction) : nullptr;

			FString Unused;
			const EControlFlowStepKind Kind = Resolved ? UControlFlowBP::DeduceStepKind(Resolved, Unused) : EControlFlowStepKind::Function;
			DescribeKind(Kind, StepFunction, GetLiteral(Node, StepName), OutInfo);
		}
		else if (Called == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSetCancelledStepAsComplete))
		{
			OutInfo.Type = EControlFlowBPStepType::Setting;
			OutInfo.Name = MakeStepName(GetLiteral(Node, StepName), NAME_None, TEXT("SetCancelledStepAsComplete"));
		}
		else
		{
			return false;
		}

		return true;
	}

	static const UEdGraphNode* FindEventNode(const UBlueprint* Blueprint, FName Name)
	{
		if (!Blueprint || Name.IsNone())
		{
			return nullptr;
		}

		for (const UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph && Graph->GetFName() == Name)
			{
				for (const UEdGraphNode* Node : Graph->Nodes)
				{
					if (Cast<UK2Node_FunctionEntry>(Node))
					{
						return Node;
					}
				}
			}
		}

		TArray<UK2Node_CustomEvent*> Events;
		FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
		for (const UK2Node_CustomEvent* Event : Events)
		{
			if (Event && Event->CustomFunctionName == Name)
			{
				return Event;
			}
		}

		return nullptr;
	}

	static TArray<const UEdGraphNode*> GetExecOrder(const UEdGraphNode* Entry)
	{
		TArray<const UEdGraphNode*> Order;
		TSet<const UEdGraphNode*> Visited;
		TArray<const UEdGraphNode*> Stack = { Entry };

		while (!Stack.IsEmpty())
		{
			const UEdGraphNode* Node = Stack.Pop();
			if (!Node || Visited.Contains(Node))
			{
				continue;
			}
			Visited.Add(Node);
			Order.Add(Node);

			TArray<const UEdGraphNode*> Next;
			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					for (const UEdGraphPin* Linked : Pin->LinkedTo)
					{
						Next.Add(Linked->GetOwningNode());
					}
				}
			}

			for (int32 Index = Next.Num() - 1; Index >= 0; --Index)
			{
				Stack.Push(Next[Index]);
			}
		}

		return Order;
	}

	static const UEdGraphPin* ResolveFlowSource(const UEdGraphPin* FlowInput)
	{
		const UEdGraphPin* Pin = FlowInput;
		for (int32 Hops = 0; Pin && Pin->LinkedTo.Num() == 1 && Hops < 1024; ++Hops)
		{
			const UEdGraphPin* Source = Pin->LinkedTo[0];
			const UEdGraphNode* SourceNode = Source->GetOwningNode();

			if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(SourceNode))
			{
				Pin = Knot->GetInputPin();
			}
			else if (IsQueueNode(SourceNode) && Source == GetFlowOutputPin(SourceNode))
			{
				Pin = GetFlowInputPin(SourceNode);
			}
			else
			{
				return Source;
			}
		}

		return nullptr;
	}

	static bool IsCallTo(const UEdGraphNode* Node, const UClass* Class, FName Function)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		const UFunction* Target = Call ? Call->GetTargetFunction() : nullptr;
		return Target && Target->GetFName() == Function && Target->GetOwnerClass() == Class;
	}

	static TSharedRef<FControlFlowBPPreviewStep> PredictStep(const UEdGraphNode* QueueNode, const FQueueInfo& Info, const UBlueprint* Blueprint, int32 Depth,
		TSet<const UEdGraphNode*>& ExpandingEvents);

	static void PredictLanesFromEvent(const UBlueprint* Blueprint, const FBuilder& Builder, int32 Depth, TSet<const UEdGraphNode*>& ExpandingEvents,
		TArray<FControlFlowBPPreviewLane>& OutLanes)
	{
		const UEdGraphNode* Entry = FindEventNode(Blueprint, Builder.Event);
		if (!Entry || ExpandingEvents.Contains(Entry))
		{
			return;
		}
		ExpandingEvents.Add(Entry);

		const TArray<const UEdGraphNode*> Order = GetExecOrder(Entry);

		TMap<const UEdGraphPin*, int32> LaneBySource;
		const auto AddLane = [&OutLanes](const FString& Name, const UEdGraphNode* SourceNode) -> int32
		{
			FControlFlowBPPreviewLane& Lane = OutLanes.AddDefaulted_GetRef();
			Lane.Name = Name;
			Lane.SourceNode = SourceNode;
			return OutLanes.Num() - 1;
		};

		switch (Builder.Source)
		{
		case ELaneSource::Tracks:
		case ELaneSource::Cases:
			{
				const bool bTracks = Builder.Source == ELaneSource::Tracks;
				const UClass* ScopeClass = bTracks ? UControlFlowForkScope::StaticClass() : UControlFlowBranchScope::StaticClass();
				const FName AddFunction = bTracks
					? GET_FUNCTION_NAME_CHECKED(UControlFlowForkScope, AddProng)
					: GET_FUNCTION_NAME_CHECKED(UControlFlowBranchScope, AddBranch);
				static const FName KeyPin(TEXT("Key"));
				const FName NamePin = bTracks ? FName(TEXT("ProngName")) : FName(TEXT("BranchName"));

				TMap<int32, int32> LaneByKey;
				for (const UEdGraphNode* Node : Order)
				{
					if (!IsCallTo(Node, ScopeClass, AddFunction))
					{
						continue;
					}

					const TOptional<FString> KeyText = GetLiteral(Node, KeyPin);
					const TOptional<int32> Key = KeyText.IsSet() ? TOptional<int32>(FCString::Atoi(**KeyText)) : TOptional<int32>();
					const TOptional<FString> Name = GetLiteral(Node, NamePin);

					int32 LaneIndex = INDEX_NONE;
					if (const int32* Existing = Key.IsSet() ? LaneByKey.Find(*Key) : nullptr)
					{
						LaneIndex = *Existing;
					}
					else
					{
						const TCHAR* Prefix = bTracks ? TEXT("Track") : TEXT("Case");
						const FString LaneName = (Name.IsSet() && !Name->IsEmpty()) ? *Name
							: Key.IsSet() ? FString::Printf(TEXT("%s%d"), Prefix, *Key)
							: FString::Printf(TEXT("%s?"), Prefix);

						LaneIndex = AddLane(LaneName, Node);
						if (Key.IsSet())
						{
							LaneByKey.Add(*Key, LaneIndex);
						}
					}

					LaneBySource.Add(CastChecked<UK2Node_CallFunction>(Node)->GetReturnValuePin(), LaneIndex);
				}
				break;
			}

		case ELaneSource::Flow:
			{
				const int32 LaneIndex = AddLane(Builder.LaneName, Entry);
				for (const UEdGraphPin* Pin : Entry->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object
						&& Pin->PinType.PinSubCategoryObject == UControlFlowBP::StaticClass())
					{
						LaneBySource.Add(Pin, LaneIndex);

						if (Cast<UK2Node_FunctionEntry>(Entry) && Entry->GetGraph())
						{
							for (const UEdGraphNode* Other : Entry->GetGraph()->Nodes)
							{
								const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Other);
								if (const UEdGraphPin* Value = (Get && Get->GetVarName() == Pin->PinName) ? Get->FindPin(Pin->PinName, EGPD_Output) : nullptr)
								{
									LaneBySource.Add(Value, LaneIndex);
								}
							}
						}
					}
				}
				break;
			}

		case ELaneSource::Body:
			{
				const int32 LaneIndex = AddLane(FString(), Entry);
				if (Entry->GetGraph())
				{
					for (const UEdGraphNode* Node : Entry->GetGraph()->Nodes)
					{
						if (IsCallTo(Node, UControlFlowLoopScope::StaticClass(), GET_FUNCTION_NAME_CHECKED(UControlFlowLoopScope, GetBody)))
						{
							LaneBySource.Add(CastChecked<UK2Node_CallFunction>(Node)->GetReturnValuePin(), LaneIndex);
						}
					}
				}
				break;
			}
		}

		for (const UEdGraphNode* Node : Order)
		{
			FQueueInfo Info;
			if (!IsQueueNode(Node) || !DescribeQueueNode(Node, Blueprint, Info))
			{
				continue;
			}

			const UEdGraphPin* Source = ResolveFlowSource(GetFlowInputPin(Node));
			if (const int32* LaneIndex = Source ? LaneBySource.Find(Source) : nullptr)
			{
				OutLanes[*LaneIndex].Steps.Add(PredictStep(Node, Info, Blueprint, Depth + 1, ExpandingEvents));
			}
		}

		ExpandingEvents.Remove(Entry);
	}

	static TSharedRef<FControlFlowBPPreviewStep> PredictStep(const UEdGraphNode* QueueNode, const FQueueInfo& Info, const UBlueprint* Blueprint, int32 Depth,
		TSet<const UEdGraphNode*>& ExpandingEvents)
	{
		TSharedRef<FControlFlowBPPreviewStep> Preview = MakeShared<FControlFlowBPPreviewStep>();
		Preview->QueueNode = QueueNode;
		Preview->Record->Name = Info.Name;
		Preview->Record->Type = Info.Type;
		Preview->Record->HandlerFunction = Info.Handler;

		if (Depth < MaxDepth)
		{
			for (const FBuilder& Builder : Info.Builders)
			{
				PredictLanesFromEvent(Blueprint, Builder, Depth, ExpandingEvents, Preview->Lanes);
			}
		}

		return Preview;
	}

	static void PlaceRecords(TArray<FControlFlowBPPreviewLane>& Lanes, const FControlFlowBPStepRecord& Owner)
	{
		for (FControlFlowBPPreviewLane& Lane : Lanes)
		{
			const FString LanePath = Owner.Type == EControlFlowBPStepType::Loop ? Owner.Path + TEXT("#n")
				: Lane.Name.IsEmpty() ? Owner.Path
				: Owner.Path + TEXT(".") + Lane.Name;

			for (const TSharedRef<FControlFlowBPPreviewStep>& Step : Lane.Steps)
			{
				FControlFlowBPStepRecord& Record = *Step->Record;
				Record.Path = LanePath + TEXT(".") + Record.Name;
				Record.Depth = Owner.Depth + 1;
				Record.Group = Lane.Name;
				Record.HandlerObject = Owner.HandlerObject;
				Record.Info = FString::Printf(TEXT("preview: queued when '%s' starts"), *Owner.Name);

				PlaceRecords(Step->Lanes, Record);
			}
		}
	}
}

TArray<FControlFlowBPPreviewLane> FControlFlowBPDebugPreview::PredictLanes(const FControlFlowBPStepRecord& Step)
{
	using namespace UE::ControlFlowBP::DebugPreview;

	TArray<FControlFlowBPPreviewLane> Lanes;

	FControlFlowBPEditorDebugger* EditorDebugger = FControlFlowBPEditorDebugger::Get();
	if (Step.State != EControlFlowBPStepState::Pending || !FControlFlowBPStepRecord::RunsFlows(Step.Type) || !EditorDebugger)
	{
		return Lanes;
	}

	const UEdGraphNode* QueueNode = EditorDebugger->ResolveNode(Step.QueueSite);
	const UBlueprint* Blueprint = QueueNode ? FBlueprintEditorUtils::FindBlueprintForNode(QueueNode) : nullptr;

	FQueueInfo Info;
	if (!Blueprint || !DescribeQueueNode(QueueNode, Blueprint, Info))
	{
		return Lanes;
	}

	TSet<const UEdGraphNode*> ExpandingEvents;
	for (const FBuilder& Builder : Info.Builders)
	{
		PredictLanesFromEvent(Blueprint, Builder, 0, ExpandingEvents, Lanes);
	}

	PlaceRecords(Lanes, Step);
	return Lanes;
}
