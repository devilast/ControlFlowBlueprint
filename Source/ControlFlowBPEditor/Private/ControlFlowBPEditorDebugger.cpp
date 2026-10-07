#include "ControlFlowBPEditorDebugger.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPModule.h"
#include "K2Node_QueueControlFlowBase.h"
#include "K2Node_QueueControlFlowIf.h"
#include "K2Node_QueueControlFlowStep.h"
#include "SControlFlowBPDebugger.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Styling/AppStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetDebugUtilities.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetNodes/KismetNodeInfoContext.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/UObjectToken.h"
#include "SNodePanel.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "ControlFlowBPEditorDebugger"

FControlFlowBPEditorDebugger* FControlFlowBPEditorDebugger::Instance = nullptr;

namespace UE::ControlFlowBP::EditorDebugger
{
	static const FLinearColor RunningColor(0.15f, 0.42f, 0.15f);
	static const FLinearColor WaitingColor(0.1f, 0.3f, 0.5f);
	static const FLinearColor QueuedColor(0.25f, 0.25f, 0.25f);
	static const FLinearColor DoneColor(0.2f, 0.3f, 0.2f);
	static const FLinearColor FailedColor(0.6f, 0.12f, 0.08f);
	static const FLinearColor CancelledColor(0.55f, 0.33f, 0.05f);
	static const FLinearColor BreakColor(0.6f, 0.1f, 0.1f);
	static constexpr double FinishedBubbleSeconds = 30.0;
	static constexpr int32 MaxBubblesPerNode = 3;

	static bool IsHandlerQueueFunction(FName Name)
	{
		static const TSet<FName> Names =
		{
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueStep),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueFunction),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWait),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWaitUntil),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSubFlow),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueIf),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueBranch),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueFork),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRace),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueLoop),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueRepeat),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueForEach),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSimpleFunction),
			GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueCombinedLoop),
		};

		return Names.Contains(Name);
	}

	static bool IsHandlerlessQueueFunction(FName Name)
	{
		return Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueDelay)
			|| Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueWaitForEventDispatcher)
			|| Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, QueueSetCancelledStepAsComplete);
	}

	static bool IsFlowFunction(FName Name)
	{
		return Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, CreateControlFlow)
			|| Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, FindOrCreateNamedFlow)
			|| Name == GET_FUNCTION_NAME_CHECKED(UControlFlowBP, ExecuteFlow);
	}

	static const UFunction* GetCalledControlFlowFunction(const UEdGraphNode* Node)
	{
		const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node);
		if (!Call || (Call->GetClass() != UK2Node_CallFunction::StaticClass() && Call->GetClass() != UK2Node_CallArrayFunction::StaticClass()))
		{
			return nullptr;
		}

		const UFunction* Function = Call->GetTargetFunction();
		return (Function && Function->GetOwnerClass() == UControlFlowBP::StaticClass()) ? Function : nullptr;
	}

	static FName GetHandlerFunctionName(const UEdGraphNode* Node)
	{
		if (const UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node))
		{
			return Event->CustomFunctionName;
		}

		if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node))
		{
			return Entry->GetGraph() ? Entry->GetGraph()->GetFName() : NAME_None;
		}

		return NAME_None;
	}

	static FString Capitalize(FString Text)
	{
		if (!Text.IsEmpty())
		{
			Text[0] = FChar::ToUpper(Text[0]);
		}

		return Text;
	}

	static FString ObjectLabel(const UObject* Object)
	{
		if (const AActor* Actor = Cast<AActor>(Object))
		{
			return Actor->GetActorLabel();
		}

		return Object ? Object->GetName() : FString(TEXT("(gone)"));
	}

	static FLinearColor StepColor(const FControlFlowBPStepRecord& Step)
	{
		switch (Step.State)
		{
		case EControlFlowBPStepState::Running:
			return (Step.Type == EControlFlowBPStepType::Wait || Step.Type == EControlFlowBPStepType::Delay) ? WaitingColor : RunningColor;
		case EControlFlowBPStepState::Succeeded:
			return DoneColor;
		case EControlFlowBPStepState::Failed:
			return FailedColor;
		case EControlFlowBPStepState::Cancelled:
			return CancelledColor;
		default:
			return QueuedColor;
		}
	}

	static FLinearColor FlowColor(const FControlFlowBPFlowRecord& Flow)
	{
		switch (Flow.State)
		{
		case EControlFlowBPFlowState::Building:  return CancelledColor;
		case EControlFlowBPFlowState::Running:   return RunningColor;
		case EControlFlowBPFlowState::Completed: return DoneColor;
		default:                                 return CancelledColor;
		}
	}

	static void FocusTokenObject(const TSharedRef<IMessageToken>& Token)
	{
		if (Token->GetType() == EMessageToken::Object)
		{
			const TSharedRef<FUObjectToken> ObjectToken = StaticCastSharedRef<FUObjectToken>(Token);
			if (const UObject* Object = ObjectToken->GetObject().Get())
			{
				FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Object);
			}
		}
	}
}

void FControlFlowBPEditorDebugger::Startup()
{
	if (!Instance)
	{
		Instance = new FControlFlowBPEditorDebugger();
	}
}

void FControlFlowBPEditorDebugger::Shutdown()
{
	delete Instance;
	Instance = nullptr;
}

FControlFlowBPEditorDebugger* FControlFlowBPEditorDebugger::Get()
{
	return Instance;
}

FControlFlowBPEditorDebugger::FControlFlowBPEditorDebugger()
{
	IssueHandle = FControlFlowBPDebug::OnIssue.AddRaw(this, &FControlFlowBPEditorDebugger::HandleIssue);
	HandlerStartingHandle = FControlFlowBPDebug::OnStepHandlerStarting.AddRaw(this, &FControlFlowBPEditorDebugger::HandleStepHandlerStarting);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(this, &FControlFlowBPEditorDebugger::HandleEndPIE);
	MenusStartupHandle = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FControlFlowBPEditorDebugger::RegisterMenus));
}

FControlFlowBPEditorDebugger::~FControlFlowBPEditorDebugger()
{
	FControlFlowBPDebug::OnIssue.Remove(IssueHandle);
	FControlFlowBPDebug::OnStepHandlerStarting.Remove(HandlerStartingHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);

	if (GEditor && BlueprintCompiledHandle.IsValid())
	{
		GEditor->OnBlueprintCompiled().Remove(BlueprintCompiledHandle);
	}

	UToolMenus::UnRegisterStartupCallback(MenusStartupHandle);
	UToolMenus::UnregisterOwner(this);
}

void FControlFlowBPEditorDebugger::HandleEndPIE(const bool)
{
	ClearCaches();
}

void FControlFlowBPEditorDebugger::ClearCaches()
{
	NodeCache.Reset();
	Index = FNodeIndex();
	IndexFrame = MAX_uint64;
}

UEdGraphNode* FControlFlowBPEditorDebugger::ResolveNode(const FControlFlowBPCallSite& Site)
{
	UFunction* Function = Site.Function.Get();
	if (!Site.IsSet() || !Function)
	{
		return nullptr;
	}
	
	if (!BlueprintCompiledHandle.IsValid() && GEditor)
	{
		BlueprintCompiledHandle = GEditor->OnBlueprintCompiled().AddRaw(this, &FControlFlowBPEditorDebugger::ClearCaches);
	}

	const TPair<TWeakObjectPtr<UFunction>, int32> Key(Function, Site.CodeOffset);
	if (const TWeakObjectPtr<UEdGraphNode>* Cached = NodeCache.Find(Key))
	{
		return Cached->Get();
	}
	
	UEdGraphNode* Node = FKismetDebugUtilities::FindSourceNodeForCodeLocation(Function, Function, Site.CodeOffset, true);
	NodeCache.Add(Key, Node);
	return Node;
}

void FControlFlowBPEditorDebugger::HandleIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPCallSite& Site, const FControlFlowBPStepRecord*)
{
	using namespace UE::ControlFlowBP::EditorDebugger;
	
	if (!GEditor || !GEditor->PlayWorld)
	{
		return;
	}

	const EMessageSeverity::Type Severity =
		Verbosity <= ELogVerbosity::Error ? EMessageSeverity::Error
		: Verbosity == ELogVerbosity::Warning ? EMessageSeverity::Warning
		: EMessageSeverity::Info;

	TSharedRef<FTokenizedMessage> Entry = FTokenizedMessage::Create(Severity);
	Entry->AddToken(FTextToken::Create(FText::Format(LOCTEXT("IssueMessage", "Control Flow: {0}"), FText::FromString(Message))));
	
	if (UEdGraphNode* Node = ResolveNode(Site))
	{
		Entry->AddToken(FTextToken::Create(LOCTEXT("NodeLabel", "Node:")));
		Entry->AddToken(FUObjectToken::Create(Node, Node->GetNodeTitle(ENodeTitleType::ListView))
			->OnMessageTokenActivated(FOnMessageTokenActivated::CreateStatic(&FocusTokenObject)));

		if (UEdGraph* Graph = Node->GetGraph())
		{
			Entry->AddToken(FTextToken::Create(LOCTEXT("GraphLabel", "Graph:")));
			Entry->AddToken(FUObjectToken::Create(Graph, FText::FromString(Graph->GetName()))
				->OnMessageTokenActivated(FOnMessageTokenActivated::CreateStatic(&FocusTokenObject)));
		}
	}

	FMessageLog(TEXT("PIE")).AddMessage(Entry);
}

void FControlFlowBPEditorDebugger::ToggleBreak(const FGuid& NodeGuid)
{
	if (BreakNodes.Remove(NodeGuid) == 0)
	{
		BreakNodes.Add(NodeGuid);
	}
}

bool FControlFlowBPEditorDebugger::TryJumpToCallSite(const FControlFlowBPCallSite& Site)
{
	if (UEdGraphNode* Node = ResolveNode(Site))
	{
		FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Node);
		return true;
	}

	return false;
}

UObject* FControlFlowBPEditorDebugger::FindHandlerDefinition(const FControlFlowBPStepRecord& Step) const
{
	const UObject* Handler = Step.HandlerObject.Get();
	if (!Handler || Step.HandlerFunction.IsNone())
	{
		return nullptr;
	}
	
	const UFunction* Function = Handler->FindFunction(Step.HandlerFunction);
	UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass(Function ? Function->GetOwnerClass() : Handler->GetClass());
	if (!Blueprint)
	{
		return nullptr;
	}
	
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == Step.HandlerFunction)
		{
			return Graph;
		}
	}

	TArray<UK2Node_CustomEvent*> Events;
	FBlueprintEditorUtils::GetAllNodesOfClass(Blueprint, Events);
	for (UK2Node_CustomEvent* Event : Events)
	{
		if (Event && Event->CustomFunctionName == Step.HandlerFunction)
		{
			return Event;
		}
	}

	return nullptr;
}

bool FControlFlowBPEditorDebugger::TryJumpToHandler(const FControlFlowBPStepRecord& Step)
{
	if (UObject* Definition = FindHandlerDefinition(Step))
	{
		FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Definition);
		return true;
	}

	return false;
}

void FControlFlowBPEditorDebugger::DebugInBlueprintEditor(UObject* Object)
{
	UBlueprint* Blueprint = Object ? UBlueprint::GetBlueprintFromClass(Object->GetClass()) : nullptr;
	if (!Blueprint || !GEditor)
	{
		return;
	}

	Blueprint->SetObjectBeingDebugged(Object);
	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Blueprint);
}

void FControlFlowBPEditorDebugger::ShowNodeInDebugger(const UEdGraphNode* Node)
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	if (!Node)
	{
		return;
	}

	RebuildIndexIfStale();
	
	const auto Relevance = [](const FControlFlowBPStepRecord& Step) -> double
	{
		switch (Step.State)
		{
		case EControlFlowBPStepState::Running: return 3e9 + Step.StartTime;
		case EControlFlowBPStepState::Pending: return 2e9 - Step.QueuedTime;
		default:                               return Step.EndTime;
		}
	};

	TSharedPtr<FControlFlowBPStepRecord> BestStep;
	const auto Consider = [&BestStep, &Relevance](const TSharedRef<FControlFlowBPStepRecord>& Step)
	{
		if (!BestStep.IsValid() || Relevance(*Step) > Relevance(*BestStep))
		{
			BestStep = Step;
		}
	};

	TSharedPtr<FControlFlowBPFlowRecord> BestFlow;

	if (const TArray<TSharedRef<FControlFlowBPStepRecord>>* Steps = Index.Steps.Find(Node))
	{
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : *Steps)
		{
			Consider(Step);
		}
	}
	else if (const TArray<TSharedRef<FControlFlowBPFlowRecord>>* Flows = Index.Flows.Find(Node))
	{
		for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : *Flows)
		{
			if (!BestFlow.IsValid() || Flow->CreateTime > BestFlow->CreateTime)
			{
				BestFlow = Flow;
			}
		}
	}
	else if (IsStepHandlerNode(Node))
	{
		const FName FunctionName = GetHandlerFunctionName(Node);
		const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
		const UClass* BlueprintClass = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;

		TArray<TSharedRef<FControlFlowBPFlowRecord>> AllFlows;
		FControlFlowBPDebug::GetFlows(AllFlows);
		for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : AllFlows)
		{
			Flow->ForEachStep([FunctionName, BlueprintClass, &Consider](const TSharedRef<FControlFlowBPStepRecord>& Step) -> void
			{
				const UObject* Handler = Step->HandlerObject.Get();
				if (Step->HandlerFunction == FunctionName && Handler && BlueprintClass && Handler->IsA(BlueprintClass))
				{
					Consider(Step);
				}
			});
		}
	}

	if (BestStep.IsValid())
	{
		BestFlow = BestStep->Flow.Pin();
	}

	SControlFlowBPDebugger::ShowInDebugger(BestFlow, BestStep);

	if (!BestFlow.IsValid())
	{
		FNotificationInfo Info(LOCTEXT("NothingRecorded", "No control flow activity recorded for this node yet. Run it in Play In Editor first."));
		Info.ExpireDuration = 4.f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void FControlFlowBPEditorDebugger::HandleStepHandlerStarting(const FControlFlowBPStepRecord& Step, bool bMatchesBreakPattern)
{
	if (!GEditor || !GEditor->PlayWorld)
	{
		return;
	}

	bool bBreak = bMatchesBreakPattern;
	if (!bBreak && BreakNodes.Num() > 0)
	{
		const UEdGraphNode* Node = ResolveNode(Step.QueueSite);
		bBreak = Node && BreakNodes.Contains(Node->NodeGuid);
	}

	if (!bBreak)
	{
		return;
	}
	
	const UObject* Handler = Step.HandlerObject.Get();
	const UFunction* Function = Handler ? Handler->FindFunction(Step.HandlerFunction) : nullptr;
	if (!Function || Function->HasAnyFunctionFlags(FUNC_Native))
	{
		UE_LOG(LogControlFlowBP, Warning, TEXT("Break When Step Runs: step '%s' has no Blueprint event to stop in."), *Step.Path);
		return;
	}

	UE_LOG(LogControlFlowBP, Display, TEXT("Break When Step Runs: pausing in '%s', which is about to run step '%s'."), *Function->GetName(), *Step.Path);
	
	FKismetDebugUtilities::RequestSingleStepIn();
}

void FControlFlowBPEditorDebugger::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	
	static const TCHAR* NodeClassNames[] =
	{
		TEXT("K2Node_QueueControlFlowStep"),
		TEXT("K2Node_QueueControlFlowIf"),
		TEXT("K2Node_QueueControlFlowWaitUntil"),
		TEXT("K2Node_QueueControlFlowWaitForDispatcher"),
		TEXT("K2Node_QueueControlFlowRepeat"),
		TEXT("K2Node_QueueControlFlowForEach"),
		TEXT("K2Node_CallFunction"),
		TEXT("K2Node_CallArrayFunction"),
		TEXT("K2Node_CustomEvent"),
		TEXT("K2Node_FunctionEntry"),
	};
	for (const TCHAR* NodeClassName : NodeClassNames)
	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(FName(*FString::Printf(TEXT("GraphEditor.GraphNodeContextMenu.%s"), NodeClassName)));
		Menu->AddDynamicSection(TEXT("ControlFlowBPDebugging"), FNewToolMenuDelegate::CreateRaw(this, &FControlFlowBPEditorDebugger::FillNodeContextMenu));
	}
}

void FControlFlowBPEditorDebugger::FillNodeContextMenu(UToolMenu* Menu)
{
	const UGraphNodeContextMenuContext* Context = Menu ? Menu->FindContext<UGraphNodeContextMenuContext>() : nullptr;
	const UEdGraphNode* Node = Context ? Context->Node.Get() : nullptr;
	if (!Node || !(IsQueueNode(Node, false) || IsFlowNode(Node) || IsStepHandlerNode(Node)))
	{
		return;
	}

	FToolMenuSection& Section = Menu->AddSection(TEXT("ControlFlowBPDebugging"), LOCTEXT("DebuggingSection", "Control Flow Debugging"));

	const TWeakObjectPtr<const UEdGraphNode> WeakNode(Node);
	Section.AddMenuEntry(
		TEXT("ShowInControlFlowDebugger"),
		LOCTEXT("ShowInControlFlowDebugger", "Show in Control Flow Debugger"),
		LOCTEXT("ShowInControlFlowDebuggerTooltip", "Open the Control Flow Debugger on the step this node queued, the flow it created, or the step it implements."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("BlueprintDebugger.TabIcon")),
		FUIAction(FExecuteAction::CreateLambda([this, WeakNode]() { ShowNodeInDebugger(WeakNode.Get()); })));

	if (IsQueueNode(Node, true))
	{
		const FGuid NodeGuid = Node->NodeGuid;
		Section.AddMenuEntry(
			TEXT("BreakWhenStepRuns"),
			LOCTEXT("BreakWhenStepRuns", "Break When This Step Runs"),
			LOCTEXT("BreakWhenStepRunsTooltip",
				"During Play In Editor, pause the Blueprint debugger on the first node of this step's event each time the step starts.\n"
				"Lasts for this editor session. To break by step path instead, use the ControlFlowBP.BreakOnStep console variable."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([this, NodeGuid]() { ToggleBreak(NodeGuid); }),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([this, NodeGuid]() { return IsBreakEnabled(NodeGuid); })),
			EUserInterfaceActionType::ToggleButton);
	}
}

bool FControlFlowBPEditorDebugger::IsQueueNode(const UEdGraphNode* Node, bool bRequireHandler)
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	if (const UK2Node_QueueControlFlowStep* QueueStep = Cast<UK2Node_QueueControlFlowStep>(Node))
	{
		return !bRequireHandler || QueueStep->GetStepKind() != EControlFlowStepKind::Delay;
	}

	if (Cast<UK2Node_QueueControlFlowIf>(Node))
	{
		return true;
	}

	if (const UK2Node_QueueControlFlowBase* Custom = Cast<UK2Node_QueueControlFlowBase>(Node))
	{
		return !bRequireHandler || Custom->HasStepHandler();
	}

	if (const UFunction* Function = GetCalledControlFlowFunction(Node))
	{
		return IsHandlerQueueFunction(Function->GetFName())
			|| (!bRequireHandler && IsHandlerlessQueueFunction(Function->GetFName()));
	}

	return false;
}

bool FControlFlowBPEditorDebugger::IsFlowNode(const UEdGraphNode* Node)
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	const UFunction* Function = GetCalledControlFlowFunction(Node);
	return Function && IsFlowFunction(Function->GetFName());
}

bool FControlFlowBPEditorDebugger::IsStepHandlerNode(const UEdGraphNode* Node)
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	const FName FunctionName = GetHandlerFunctionName(Node);
	if (FunctionName.IsNone())
	{
		return false;
	}

	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
	const UClass* Class = Blueprint
		? (Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get())
		: nullptr;

	const UFunction* Function = Class ? Class->FindFunctionByName(FunctionName) : nullptr;

	FString Unused;
	return Function
		&& (UControlFlowBP::DeduceStepKind(Function, Unused) != EControlFlowStepKind::Invalid || UControlFlowBP::IsConditionFunction(Function, Unused));
}

void FControlFlowBPEditorDebugger::RebuildIndexIfStale()
{
	const uint64 ChangeCount = FControlFlowBPDebug::GetChangeCount();
	if (IndexFrame == GFrameCounter && IndexChangeCount == ChangeCount)
	{
		return;
	}

	IndexFrame = GFrameCounter;
	IndexChangeCount = ChangeCount;
	Index = FNodeIndex();

	TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
	FControlFlowBPDebug::GetFlows(Flows);

	for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
	{
		if (UEdGraphNode* Node = ResolveNode(Flow->CreateSite))
		{
			Index.Flows.FindOrAdd(Node).AddUnique(Flow);
		}

		if (UEdGraphNode* Node = ResolveNode(Flow->ExecuteSite))
		{
			Index.Flows.FindOrAdd(Node).AddUnique(Flow);
		}

		Flow->ForEachStep([this](const TSharedRef<FControlFlowBPStepRecord>& Step) -> void
		{
			if (Step->IsInternal())
			{
				return;
			}

			if (UEdGraphNode* Node = ResolveNode(Step->QueueSite))
			{
				Index.Steps.FindOrAdd(Node).Add(Step);
			}

			if (Step->State == EControlFlowBPStepState::Running && !Step->HandlerFunction.IsNone())
			{
				Index.RunningHandlers.Add(Step);
			}
		});
	}
}

void FControlFlowBPEditorDebugger::GetNodePopups(const UEdGraphNode* Node, const FKismetNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups)
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	if (!Node)
	{
		return;
	}

	if (BreakNodes.Contains(Node->NodeGuid))
	{
		OutPopups.Emplace(nullptr, BreakColor, TEXT("Breaks when this step runs"));
	}
	
	if (!GEditor || !GEditor->PlayWorld)
	{
		return;
	}

	RebuildIndexIfStale();
	
	const UObject* DebugObject = Context ? Context->ActiveObjectBeingDebugged : nullptr;

	if (const TArray<TSharedRef<FControlFlowBPStepRecord>>* Steps = Index.Steps.Find(Node))
	{
		AddStepPopups(*Steps, DebugObject, OutPopups);
	}

	if (const TArray<TSharedRef<FControlFlowBPFlowRecord>>* Flows = Index.Flows.Find(Node))
	{
		AddFlowPopups(*Flows, DebugObject, OutPopups);
	}

	if (Index.RunningHandlers.Num() > 0 && (Cast<UK2Node_CustomEvent>(Node) || Cast<UK2Node_FunctionEntry>(Node)))
	{
		AddHandlerPopups(Node, DebugObject, OutPopups);
	}
}

void FControlFlowBPEditorDebugger::AddStepPopups(const TArray<TSharedRef<FControlFlowBPStepRecord>>& Steps, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	TArray<TSharedRef<FControlFlowBPStepRecord>> Live;
	TSharedPtr<FControlFlowBPStepRecord> LastFinished;
	TSet<const UObject*> Instances;

	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Steps)
	{
		const UObject* QueuedBy = Step->QueueSite.Context.Get();
		if (DebugObject && QueuedBy != DebugObject)
		{
			continue;
		}

		const TSharedPtr<FControlFlowBPFlowRecord> Flow = Step->Flow.Pin();
		if (!Flow.IsValid() || !Flow->IsClockLive())
		{
			continue;
		}

		if (!Step->IsFinished())
		{
			Live.Add(Step);
			Instances.Add(QueuedBy);
		}
		else if (!LastFinished.IsValid() || Step->EndTime > LastFinished->EndTime)
		{
			LastFinished = Step;
		}
	}

	Live.Sort([](const TSharedRef<FControlFlowBPStepRecord>& A, const TSharedRef<FControlFlowBPStepRecord>& B) -> bool
	{
		if (A->State != B->State)
		{
			return A->State == EControlFlowBPStepState::Running;
		}

		return A->QueuedTime < B->QueuedTime;
	});

	const bool bNameInstances = Instances.Num() > 1;

	for (int32 BubbleIndex = 0; BubbleIndex < FMath::Min(Live.Num(), MaxBubblesPerNode); ++BubbleIndex)
	{
		const FControlFlowBPStepRecord& Step = *Live[BubbleIndex];

		FString Text = Capitalize(Step.DescribeState()) + TEXT("\n") + Step.Path;
		if (bNameInstances)
		{
			Text = FString::Printf(TEXT("[%s] %s"), *ObjectLabel(Step.QueueSite.Context.Get()), *Text);
		}

		OutPopups.Emplace(nullptr, StepColor(Step), Text);
	}

	if (Live.Num() > MaxBubblesPerNode)
	{
		OutPopups.Emplace(nullptr, QueuedColor, FString::Printf(TEXT("+%d more running or queued from this node"), Live.Num() - MaxBubblesPerNode));
	}

	if (!Live.IsEmpty() || !LastFinished.IsValid())
	{
		return;
	}

	const TSharedPtr<FControlFlowBPFlowRecord> LastFlow = LastFinished->Flow.Pin();
	const double Ago = LastFlow.IsValid() ? LastFlow->GetTime() - LastFinished->EndTime : FinishedBubbleSeconds;
	if (Ago < FinishedBubbleSeconds)
	{
		OutPopups.Emplace(nullptr, StepColor(*LastFinished), FString::Printf(TEXT("%s (%s ago)\n%s"),
			*Capitalize(LastFinished->DescribeState()),
			*FControlFlowBPDebug::FormatSeconds(Ago),
			*LastFinished->Path));
	}
}

void FControlFlowBPEditorDebugger::AddFlowPopups(const TArray<TSharedRef<FControlFlowBPFlowRecord>>& Flows, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	TArray<TSharedRef<FControlFlowBPFlowRecord>> Shown;
	for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
	{
		const bool bForDebugObject = !DebugObject
			|| Flow->CreateSite.Context.Get() == DebugObject
			|| Flow->ExecuteSite.Context.Get() == DebugObject;

		const bool bRecent = Flow->IsClockLive() && (!Flow->IsFinished() || Flow->GetTime() - Flow->EndTime < FinishedBubbleSeconds);

		if (bForDebugObject && bRecent)
		{
			Shown.Add(Flow);
		}
	}

	Shown.Sort([](const TSharedRef<FControlFlowBPFlowRecord>& A, const TSharedRef<FControlFlowBPFlowRecord>& B)
	{
		return A->CreateTime > B->CreateTime;
	});

	for (int32 BubbleIndex = 0; BubbleIndex < FMath::Min(Shown.Num(), MaxBubblesPerNode); ++BubbleIndex)
	{
		const FControlFlowBPFlowRecord& Flow = *Shown[BubbleIndex];

		FString Text = FString::Printf(TEXT("Flow '%s' (#%d): %s"), *Flow.Name, Flow.Id, *Flow.DescribeState());

		TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
		Flow.GetActiveSteps(Active, true);
		for (int32 StepIndex = 0; StepIndex < FMath::Min(Active.Num(), 2); ++StepIndex)
		{
			Text += FString::Printf(TEXT("\nat %s - %s"), *Active[StepIndex]->Path, *Active[StepIndex]->DescribeState());
		}

		OutPopups.Emplace(nullptr, FlowColor(Flow), Text);
	}

	if (Shown.Num() > MaxBubblesPerNode)
	{
		OutPopups.Emplace(nullptr, QueuedColor, FString::Printf(TEXT("+%d more flows from this node - ControlFlowBP.List"), Shown.Num() - MaxBubblesPerNode));
	}
}

void FControlFlowBPEditorDebugger::AddHandlerPopups(const UEdGraphNode* Node, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const
{
	using namespace UE::ControlFlowBP::EditorDebugger;

	const FName FunctionName = GetHandlerFunctionName(Node);
	const UBlueprint* Blueprint = FBlueprintEditorUtils::FindBlueprintForNode(Node);
	const UClass* BlueprintClass = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
	if (FunctionName.IsNone() || !BlueprintClass)
	{
		return;
	}

	int32 NumShown = 0;
	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Index.RunningHandlers)
	{
		const UObject* Handler = Step->HandlerObject.Get();
		if (!Handler || Step->HandlerFunction != FunctionName || !Handler->IsA(BlueprintClass) || (DebugObject && Handler != DebugObject))
		{
			continue;
		}

		if (NumShown++ == MaxBubblesPerNode)
		{
			break;
		}

		OutPopups.Emplace(nullptr, StepColor(*Step), FString::Printf(TEXT("%sRunning as step %s (%s)\n%s"),
			DebugObject ? TEXT("") : *FString::Printf(TEXT("[%s] "), *ObjectLabel(Handler)),
			*Step->Path,
			FControlFlowBPStepRecord::LexType(Step->Type),
			*Capitalize(Step->DescribeState())));
	}
}

#undef LOCTEXT_NAMESPACE
