#include "SControlFlowBPDebugger.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPDebugGraph.h"
#include "ControlFlowBPDebuggerShared.h"
#include "ControlFlowBPEditorDebugger.h"

#include "Algo/Reverse.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GraphEditor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ConfigCacheIni.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SHyperlink.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SExpanderArrow.h"
#include "Widgets/Views/SHeaderRow.h"
#include "Widgets/Views/STableRow.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "SControlFlowBPDebugger"

const FName SControlFlowBPDebugger::TabId(TEXT("ControlFlowBPDebugger"));
TWeakPtr<SControlFlowBPDebugger> SControlFlowBPDebugger::Instance;

namespace UE::ControlFlowBP::DebuggerTab
{
	static const FName ColFlow(TEXT("Flow"));
	static const FName ColOwner(TEXT("Owner"));
	static const FName ColState(TEXT("State"));
	static const FName ColNow(TEXT("Now"));
	static const FName ColStep(TEXT("Step"));
	static const FName ColType(TEXT("Type"));
	static const FName ColStarted(TEXT("Started"));
	static const FName ColDuration(TEXT("Duration"));
	static const FName ColDetail(TEXT("Detail"));
	static const FName ColQueuedAt(TEXT("QueuedAt"));

	static const FLinearColor RunningColor(0.4f, 0.9f, 0.4f);
	static const FLinearColor WaitingColor(0.4f, 0.75f, 1.f);
	static const FLinearColor DoneColor(0.55f, 0.65f, 0.55f);
	static const FLinearColor FailedColor(1.f, 0.35f, 0.3f);
	static const FLinearColor CancelledColor(1.f, 0.65f, 0.2f);
	static const FLinearColor InactiveColor(0.45f, 0.45f, 0.45f);

	static FControlFlowBPEditorDebugger* Debugger()
	{
		return FControlFlowBPEditorDebugger::Get();
	}

	static IConsoleVariable* FindCVar(const TCHAR* Name)
	{
		return IConsoleManager::Get().FindConsoleVariable(Name);
	}

	static int32 GetTraceLevel()
	{
		const IConsoleVariable* CVar = FindCVar(TEXT("ControlFlowBP.Trace"));
		return CVar ? CVar->GetInt() : 0;
	}

	static void SetTraceLevel(int32 Level)
	{
		if (IConsoleVariable* CVar = FindCVar(TEXT("ControlFlowBP.Trace")))
		{
			CVar->Set(Level, ECVF_SetByConsole);
		}
	}

	static FString GetBreakPattern()
	{
		const IConsoleVariable* CVar = FindCVar(TEXT("ControlFlowBP.BreakOnStep"));
		return CVar ? CVar->GetString() : FString();
	}

	static void SetBreakPattern(const FString& Pattern)
	{
		if (IConsoleVariable* CVar = FindCVar(TEXT("ControlFlowBP.BreakOnStep")))
		{
			CVar->Set(*Pattern, ECVF_SetByConsole);
		}
	}

	FLinearColor StateColor(EControlFlowBPStepState State, EControlFlowBPStepType Type)
	{
		switch (State)
		{
		case EControlFlowBPStepState::Running:
			return (Type == EControlFlowBPStepType::Wait || Type == EControlFlowBPStepType::Delay) ? WaitingColor : RunningColor;
		case EControlFlowBPStepState::Succeeded: return DoneColor;
		case EControlFlowBPStepState::Failed:    return FailedColor;
		case EControlFlowBPStepState::Cancelled: return CancelledColor;
		default:                                 return InactiveColor;
		}
	}

	static FSlateFontInfo GroupFont()
	{
		FSlateFontInfo Font = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText").Font;
		Font.TypefaceFontName = TEXT("Italic");
		return Font;
	}

	static FSlateColor FlowColor(const FControlFlowBPFlowRecord& Flow)
	{
		switch (Flow.State)
		{
		case EControlFlowBPFlowState::Building:  return CancelledColor;
		case EControlFlowBPFlowState::Running:   return RunningColor;
		case EControlFlowBPFlowState::Cancelled: return CancelledColor;
		default:                                 return FSlateColor::UseSubduedForeground();
		}
	}

	static void AccumulateStates(const FControlFlowBPDebuggerStepItem& Item, TSet<EControlFlowBPStepState>& OutStates)
	{
		if (Item.Step.IsValid())
		{
			OutStates.Add(Item.Step->State);
		}

		for (const TSharedPtr<FControlFlowBPDebuggerStepItem>& Child : Item.Children)
		{
			AccumulateStates(*Child, OutStates);
		}
	}

	EControlFlowBPStepState GetItemState(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (Item.Step.IsValid())
		{
			return Item.Step->State;
		}

		TSet<EControlFlowBPStepState> States;
		AccumulateStates(Item, States);

		const bool bAnyFinished = States.Contains(EControlFlowBPStepState::Succeeded) || States.Contains(EControlFlowBPStepState::Failed)
			|| States.Contains(EControlFlowBPStepState::Cancelled) || States.Contains(EControlFlowBPStepState::Skipped);

		if (States.Contains(EControlFlowBPStepState::Running)
			|| (States.Contains(EControlFlowBPStepState::Pending) && bAnyFinished))
		{
			return EControlFlowBPStepState::Running;
		}

		if (States.Contains(EControlFlowBPStepState::Pending))
		{
			return EControlFlowBPStepState::Pending;
		}
		if (States.Contains(EControlFlowBPStepState::Failed))
		{
			return EControlFlowBPStepState::Failed;
		}
		if (States.Contains(EControlFlowBPStepState::Cancelled))
		{
			return EControlFlowBPStepState::Cancelled;
		}
		if (States.Contains(EControlFlowBPStepState::Succeeded))
		{
			return EControlFlowBPStepState::Succeeded;
		}
		return EControlFlowBPStepState::Skipped;
	}

	static FLinearColor ItemColor(const FControlFlowBPDebuggerStepItem& Item)
	{
		return StateColor(GetItemState(Item), Item.Step.IsValid() ? Item.Step->Type : EControlFlowBPStepType::Function);
	}

	static void GetItemTimes(const FControlFlowBPDebuggerStepItem& Item, double& InOutStart, double& InOutEnd, bool& bOutAllFinished)
	{
		if (Item.Step.IsValid())
		{
			if (Item.Step->StartTime > 0.0)
			{
				InOutStart = InOutStart > 0.0 ? FMath::Min(InOutStart, Item.Step->StartTime) : Item.Step->StartTime;
			}

			if (Item.Step->IsFinished())
			{
				InOutEnd = FMath::Max(InOutEnd, Item.Step->EndTime);
			}
			else
			{
				bOutAllFinished = false;
			}
		}

		for (const TSharedPtr<FControlFlowBPDebuggerStepItem>& Child : Item.Children)
		{
			GetItemTimes(*Child, InOutStart, InOutEnd, bOutAllFinished);
		}
	}

	static TSharedPtr<FControlFlowBPFlowRecord> GetItemFlow(const FControlFlowBPDebuggerStepItem& Item)
	{
		const TSharedPtr<FControlFlowBPStepRecord>& Step = Item.Step.IsValid() ? Item.Step : Item.GroupOwner;
		return Step.IsValid() ? Step->Flow.Pin() : nullptr;
	}

	FText ItemLabel(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (Item.Step.IsValid())
		{
			return FText::FromString(Item.Step->Name);
		}

		switch (Item.GroupOwner.IsValid() ? Item.GroupOwner->Type : EControlFlowBPStepType::Function)
		{
		case EControlFlowBPStepType::Loop:
			{
				if (Item.bPreview)
				{
					return LOCTEXT("PreviewIterationLabel", "Each iteration (preview)");
				}

				FString Number = Item.GroupName;
				Number.RemoveFromStart(TEXT("#"));
				return FText::Format(LOCTEXT("IterationLabel", "Iteration {0}"), FText::FromString(Number));
			}
		case EControlFlowBPStepType::Branch:
		case EControlFlowBPStepType::If:
			return FText::Format(Item.bPreview ? LOCTEXT("PreviewCaseLabel", "Case '{0}' (preview)") : LOCTEXT("CaseLabel", "Case '{0}'"),
				FText::FromString(Item.GroupName));
		case EControlFlowBPStepType::Fork:
		case EControlFlowBPStepType::Race:
			return FText::Format(Item.bPreview ? LOCTEXT("PreviewTrackLabel", "Track '{0}' (preview)") : LOCTEXT("TrackLabel", "Track '{0}'"),
				FText::FromString(Item.GroupName));
		default:
			return FText::FromString(Item.GroupName);
		}
	}

	static FText ItemTypeText(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (Item.Step.IsValid())
		{
			return FText::FromString(FControlFlowBPStepRecord::LexType(Item.Step->Type));
		}

		switch (Item.GroupOwner.IsValid() ? Item.GroupOwner->Type : EControlFlowBPStepType::Function)
		{
		case EControlFlowBPStepType::Loop:   return LOCTEXT("IterationType", "Iteration");
		case EControlFlowBPStepType::Branch:
		case EControlFlowBPStepType::If:     return LOCTEXT("CaseType", "Case");
		case EControlFlowBPStepType::Fork:
		case EControlFlowBPStepType::Race:   return LOCTEXT("TrackType", "Track");
		default:                             return FText::GetEmpty();
		}
	}

	static FText ItemStateText(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (Item.bPreview)
		{
			return LOCTEXT("PreviewState", "preview");
		}

		const EControlFlowBPStepState State = GetItemState(Item);
		if (State == EControlFlowBPStepState::Running && Item.Step.IsValid())
		{
			switch (Item.Step->Type)
			{
			case EControlFlowBPStepType::Wait:  return LOCTEXT("Waiting", "waiting");
			case EControlFlowBPStepType::Delay: return LOCTEXT("Delaying", "delaying");
			default: break;
			}
		}

		return FText::FromString(FControlFlowBPStepRecord::LexState(State));
	}

	static FText ItemStartedText(const FControlFlowBPDebuggerStepItem& Item)
	{
		double Start = 0.0;
		double End = 0.0;
		bool bAllFinished = true;
		GetItemTimes(Item, Start, End, bAllFinished);

		const TSharedPtr<FControlFlowBPFlowRecord> Flow = GetItemFlow(Item);
		if (Start <= 0.0 || !Flow.IsValid() || Flow->StartTime <= 0.0)
		{
			return FText::GetEmpty();
		}

		return FText::FromString(FString::Printf(TEXT("+%.3f s"), Start - Flow->StartTime));
	}

	static FText ItemDurationText(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (Item.Step.IsValid())
		{
			return Item.Step->StartTime > 0.0 ? FText::FromString(FControlFlowBPDebug::FormatSeconds(Item.Step->GetElapsed())) : FText::GetEmpty();
		}

		double Start = 0.0;
		double End = 0.0;
		bool bAllFinished = true;
		GetItemTimes(Item, Start, End, bAllFinished);

		const TSharedPtr<FControlFlowBPFlowRecord> Flow = GetItemFlow(Item);
		if (Start <= 0.0 || !Flow.IsValid())
		{
			return FText::GetEmpty();
		}

		return FText::FromString(FControlFlowBPDebug::FormatSeconds((bAllFinished ? End : Flow->GetTime()) - Start));
	}

	static FText ItemDetailText(const FControlFlowBPDebuggerStepItem& Item)
	{
		if (!Item.Step.IsValid())
		{
			int32 NumSteps = 0;
			for (const TSharedPtr<FControlFlowBPDebuggerStepItem>& Child : Item.Children)
			{
				NumSteps += Child->Step.IsValid() ? 1 : 0;
			}
			return FText::Format(LOCTEXT("GroupDetail", "{0} {0}|plural(one=step,other=steps)"), NumSteps);
		}

		const FControlFlowBPStepRecord& Step = *Item.Step;
		if (Step.State == EControlFlowBPStepState::Running)
		{
			if (Step.Type == EControlFlowBPStepType::Wait && Step.TimeoutSeconds > 0.f)
			{
				return FText::FromString(FString::Printf(TEXT("times out in %s"), *FControlFlowBPDebug::FormatSeconds(Step.TimeoutSeconds - Step.GetElapsed())));
			}

			if (Step.Type == EControlFlowBPStepType::Loop)
			{
				return FText::FromString(FString::Printf(TEXT("iteration %d"), Step.Iterations));
			}
		}

		return FText::FromString(!Step.Detail.IsEmpty() ? Step.Detail : Step.Info);
	}

	void JumpToQueueNodeOrHandler(const FControlFlowBPStepRecord& Step)
	{
		if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger())
		{
			if (!EditorDebugger->TryJumpToCallSite(Step.QueueSite))
			{
				EditorDebugger->TryJumpToHandler(Step);
			}
		}
	}

	static bool IsBlueprintObject(const UObject* Object)
	{
		return Object && UBlueprint::GetBlueprintFromClass(Object->GetClass()) != nullptr;
	}

	static void AddFlowMenuEntries(FMenuBuilder& MenuBuilder, const TSharedRef<FControlFlowBPFlowRecord>& Flow)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("GoToCreate", "Go to Create Node"),
			LOCTEXT("GoToCreateTooltip", "The node that created this flow."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Flow]()
				{
					if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger())
					{
						EditorDebugger->TryJumpToCallSite(Flow->CreateSite);
					}
				}),
				FCanExecuteAction::CreateLambda([Flow]() { return Flow->CreateSite.IsSet(); })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("GoToExecute", "Go to Execute Node"),
			LOCTEXT("GoToExecuteTooltip", "The Execute Flow node that started this flow."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Flow]()
				{
					if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger())
					{
						EditorDebugger->TryJumpToCallSite(Flow->ExecuteSite);
					}
				}),
				FCanExecuteAction::CreateLambda([Flow]() { return Flow->ExecuteSite.IsSet(); })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("DebugOwnerMenu", "Debug Owner in Blueprint Editor"),
			LOCTEXT("DebugOwnerMenuTooltip", "Open the owner's Blueprint with the owner picked as the object being debugged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Flow]() { FControlFlowBPEditorDebugger::DebugInBlueprintEditor(Flow->Owner.Get()); }),
				FCanExecuteAction::CreateLambda([Flow]() { return IsBlueprintObject(Flow->Owner.Get()); })));

		MenuBuilder.AddMenuSeparator();

		MenuBuilder.AddMenuEntry(
			LOCTEXT("TraceFlowMenu", "Log This Flow's Steps"),
			LOCTEXT("TraceFlowMenuTooltip", "Log this flow's steps as they start and end (Enable Step Trace)."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Flow]() { Flow->bTraceSteps = !Flow->bTraceSteps; }),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Flow]() { return Flow->bTraceSteps; })),
			NAME_None,
			EUserInterfaceActionType::ToggleButton);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("DumpMenu", "Dump to Log"),
			LOCTEXT("DumpMenuTooltip", "Write this flow's running steps, queued steps and recent history to the Output Log."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Flow]() { Flow->Dump(ELogVerbosity::Display); })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("CancelFlowMenu", "Cancel Flow"),
			LOCTEXT("CancelFlowMenuTooltip", "Cancel this flow, as Cancel Flow would."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Flow]()
				{
					if (UControlFlowBP* Builder = Flow->Builder.Get())
					{
						Builder->CancelWithReason(EControlFlowBPCancelCause::Requested, TEXT("cancelled from the Control Flow Debugger"));
					}
				}),
				FCanExecuteAction::CreateLambda([Flow]() { return !Flow->IsFinished() && Flow->Builder.IsValid(); })));
	}

	static void AddStepMenuEntries(FMenuBuilder& MenuBuilder, const TSharedPtr<FControlFlowBPStepRecord>& Step, const UEdGraphNode* QueueNode)
	{
		const TWeakObjectPtr<const UEdGraphNode> WeakQueueNode(QueueNode);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("GoToQueueNode", "Go to Queue Node"),
			LOCTEXT("GoToQueueNodeTooltip", "The node that queued this step."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakQueueNode]()
				{
					if (const UEdGraphNode* Node = WeakQueueNode.Get())
					{
						FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(Node);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakQueueNode]() { return WeakQueueNode.IsValid(); })));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("GoToHandler", "Go to Event"),
			LOCTEXT("GoToHandlerTooltip", "The event or function that implements this step."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Step]()
				{
					if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger())
					{
						EditorDebugger->TryJumpToHandler(*Step);
					}
				}),
				FCanExecuteAction::CreateLambda([Step]() -> bool
				{
					const FControlFlowBPEditorDebugger* EditorDebugger = Debugger();
					return EditorDebugger && EditorDebugger->FindHandlerDefinition(*Step) != nullptr;
				})));

		MenuBuilder.AddMenuEntry(
			LOCTEXT("DebugQueuedBy", "Debug Queuing Object in Blueprint Editor"),
			LOCTEXT("DebugQueuedByTooltip", "Open the Blueprint that queued this step, with that object picked as the one being debugged."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([Step]() { FControlFlowBPEditorDebugger::DebugInBlueprintEditor(Step->QueueSite.Context.Get()); }),
				FCanExecuteAction::CreateLambda([Step]() { return IsBlueprintObject(Step->QueueSite.Context.Get()); })));

		MenuBuilder.AddMenuSeparator();

		MenuBuilder.AddMenuEntry(
			LOCTEXT("BreakWhenStepRunsMenu", "Break When This Step Runs"),
			LOCTEXT("BreakWhenStepRunsMenuTooltip", "Pause in the Blueprint debugger whenever a step from the same Queue node starts. Lasts for this editor session."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([WeakQueueNode]()
				{
					FControlFlowBPEditorDebugger* EditorDebugger = Debugger();
					if (const UEdGraphNode* Node = WeakQueueNode.Get(); EditorDebugger && Node)
					{
						EditorDebugger->ToggleBreak(Node->NodeGuid);
					}
				}),
				FCanExecuteAction::CreateLambda([WeakQueueNode]() -> bool
				{
					return FControlFlowBPEditorDebugger::IsQueueNode(WeakQueueNode.Get(), true);
				}),
				FIsActionChecked::CreateLambda([WeakQueueNode]() -> bool
				{
					const FControlFlowBPEditorDebugger* EditorDebugger = Debugger();
					const UEdGraphNode* Node = WeakQueueNode.Get();
					return EditorDebugger && Node && EditorDebugger->IsBreakEnabled(Node->NodeGuid);
				})),
			NAME_None,
			EUserInterfaceActionType::ToggleButton);

		MenuBuilder.AddMenuEntry(
			LOCTEXT("CopyPath", "Copy Path"),
			LOCTEXT("CopyPathTooltip", "Copy the step's path - for ControlFlowBP.BreakOnStep, say."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Step]() { FPlatformApplicationMisc::ClipboardCopy(*Step->Path); })));
	}

	static const TCHAR* const ConfigSection = TEXT("ControlFlowBPDebugger");
	static const TCHAR* const ConfigShowGraph = TEXT("ShowGraph");

	static FAutoConsoleCommand CmdOpenDebugger(
		TEXT("ControlFlowBP.Debugger"),
		TEXT("Opens the Control Flow Debugger tab."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(SControlFlowBPDebugger::TabId);
		}));
}

class SControlFlowBPDebuggerFlowRow : public SMultiColumnTableRow<TSharedPtr<FControlFlowBPDebuggerFlowItem>>
{
public:
	SLATE_BEGIN_ARGS(SControlFlowBPDebuggerFlowRow) {}
		SLATE_ARGUMENT(TSharedPtr<FControlFlowBPDebuggerFlowItem>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		FSuperRowType::Construct(FSuperRowType::FArguments().Padding(FMargin(2.f, 1.f)), InOwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		using namespace UE::ControlFlowBP::DebuggerTab;

		const TSharedRef<FControlFlowBPFlowRecord> Flow = Item->Flow;

		if (ColumnName == ColFlow)
		{
			return SNew(STextBlock)
				.Text(FText::FromString(FString::Printf(TEXT("#%d  %s"), Flow->Id, *Flow->Name)))
				.ColorAndOpacity_Lambda([Flow]() { return FlowColor(*Flow); })
				.ToolTipText_Lambda([Flow]() { return FText::FromString(Flow->DescribeState()); });
		}

		if (ColumnName == ColOwner)
		{
			return SNew(STextBlock).Text(FText::FromString(Flow->OwnerName));
		}

		if (ColumnName == ColState)
		{
			return SNew(STextBlock)
				.Text_Lambda([Flow]() { return FText::FromString(Flow->DescribeState()); })
				.ColorAndOpacity_Lambda([Flow]() { return FlowColor(*Flow); });
		}

		if (ColumnName == ColNow)
		{
			return SNew(STextBlock).Text_Lambda([Flow]() -> FText
			{
				TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
				Flow->GetActiveSteps(Active, true);

				FString Text;
				for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
				{
					FString Path = Step->Path;
					Path.RemoveFromStart(Flow->Name + TEXT("."));
					Text += FString::Printf(TEXT("%s%s (%s)"), Text.IsEmpty() ? TEXT("") : TEXT("  |  "), *Path, *Step->DescribeState());
				}
				return FText::FromString(Text);
			});
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FControlFlowBPDebuggerFlowItem> Item;
};

class SControlFlowBPDebuggerStepRow : public SMultiColumnTableRow<TSharedPtr<FControlFlowBPDebuggerStepItem>>
{
public:
	SLATE_BEGIN_ARGS(SControlFlowBPDebuggerStepRow) {}
		SLATE_ARGUMENT(TSharedPtr<FControlFlowBPDebuggerStepItem>, Item)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& InOwnerTable)
	{
		Item = InArgs._Item;
		FSuperRowType::Construct(FSuperRowType::FArguments().Padding(FMargin(0.f, 1.f)), InOwnerTable);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override
	{
		using namespace UE::ControlFlowBP::DebuggerTab;

		const TSharedPtr<FControlFlowBPDebuggerStepItem> RowItem = Item;

		if (ColumnName == ColStep)
		{
			return SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SExpanderArrow, SharedThis(this))
					.IndentAmount(14.f)
					.ShouldDrawWires(true)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 6.f, 0.f)
				[
					SNew(SBox)
					.WidthOverride(8.f)
					.HeightOverride(8.f)
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("WhiteBrush"))
						.ColorAndOpacity_Lambda([RowItem]() { return FSlateColor(ItemColor(*RowItem)); })
					]
				]
				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(ItemLabel(*RowItem))
					.Font((RowItem->IsGroup() || RowItem->bPreview) ? GroupFont() : FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText").Font)
					.ColorAndOpacity_Lambda([RowItem]() -> FSlateColor
					{
						const EControlFlowBPStepState State = GetItemState(*RowItem);
						return (State == EControlFlowBPStepState::Pending || State == EControlFlowBPStepState::Skipped)
							? FSlateColor::UseSubduedForeground()
							: FSlateColor::UseForeground();
					})
					.ToolTipText_Lambda([RowItem]() { return RowItem->Step.IsValid() ? FText::FromString(RowItem->Step->Path) : FText::GetEmpty(); })
				];
		}

		if (ColumnName == ColType)
		{
			return SNew(STextBlock)
				.Text(ItemTypeText(*RowItem))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground());
		}

		if (ColumnName == ColState)
		{
			return SNew(STextBlock)
				.Text_Lambda([RowItem]() { return ItemStateText(*RowItem); })
				.ColorAndOpacity_Lambda([RowItem]() { return FSlateColor(ItemColor(*RowItem)); });
		}

		if (ColumnName == ColStarted)
		{
			return SNew(STextBlock).Text_Lambda([RowItem]() { return ItemStartedText(*RowItem); });
		}

		if (ColumnName == ColDuration)
		{
			return SNew(STextBlock).Text_Lambda([RowItem]() { return ItemDurationText(*RowItem); });
		}

		if (ColumnName == ColDetail)
		{
			return SNew(STextBlock)
				.Text_Lambda([RowItem]() { return ItemDetailText(*RowItem); })
				.ToolTipText_Lambda([RowItem]() { return ItemDetailText(*RowItem); });
		}

		if (ColumnName == ColQueuedAt && RowItem->Step.IsValid() && RowItem->Step->QueueSite.IsSet())
		{
			const TSharedPtr<FControlFlowBPStepRecord> Step = RowItem->Step;
			return SNew(SHyperlink)
				.Text(FText::FromString(Step->QueueSite.Describe()))
				.ToolTipText(LOCTEXT("QueuedAtTooltip", "Go to the Queue node that queued this step."))
				.OnNavigate_Lambda([Step]()
				{
					if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger())
					{
						EditorDebugger->TryJumpToCallSite(Step->QueueSite);
					}
				});
		}

		return SNullWidget::NullWidget;
	}

private:
	TSharedPtr<FControlFlowBPDebuggerStepItem> Item;
};

void SControlFlowBPDebugger::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabId, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&) -> TSharedRef<SDockTab>
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SControlFlowBPDebugger)
			];
	}))
	.SetDisplayName(LOCTEXT("TabTitle", "Control Flow Debugger"))
	.SetTooltipText(LOCTEXT("TabTooltip", "Running and recently finished control flows, step by step."))
	.SetGroup(WorkspaceMenu::GetMenuStructure().GetDeveloperToolsDebugCategory())
	.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "BlueprintDebugger.TabIcon"));
}

void SControlFlowBPDebugger::UnregisterTabSpawner()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}
}

void SControlFlowBPDebugger::ShowInDebugger(const TSharedPtr<FControlFlowBPFlowRecord>& Flow, const TSharedPtr<FControlFlowBPStepRecord>& Step)
{
	FGlobalTabmanager::Get()->TryInvokeTab(TabId);

	if (const TSharedPtr<SControlFlowBPDebugger> Debugger = Instance.Pin(); Debugger.IsValid() && Flow.IsValid())
	{
		Debugger->SelectFlow(Flow);
		Debugger->SelectStep(Step);
	}
}

void SControlFlowBPDebugger::Construct(const FArguments& InArgs)
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	Instance = SharedThis(this);

	TraceLevelOptions.Add(MakeShared<FString>(TEXT("Off")));
	TraceLevelOptions.Add(MakeShared<FString>(TEXT("Steps")));
	TraceLevelOptions.Add(MakeShared<FString>(TEXT("Verbose")));

	bool bShowGraph = true;
	GConfig->GetBool(ConfigSection, ConfigShowGraph, bShowGraph, GEditorPerProjectIni);
	StepView = bShowGraph ? EStepView::Graph : EStepView::Tree;
	bZoomToFitPending = true;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(4.f)
		[
			MakeToolbar()
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Horizontal)

			+ SSplitter::Slot()
			.Value(0.36f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(2.f)
				[
					SNew(SOverlay)

					+ SOverlay::Slot()
					[
						SAssignNew(FlowList, SListView<FFlowItemPtr>)
						.ListItemsSource(&FlowItems)
						.SelectionMode(ESelectionMode::Single)
						.OnGenerateRow(this, &SControlFlowBPDebugger::GenerateFlowRow)
						.OnSelectionChanged(this, &SControlFlowBPDebugger::HandleFlowSelectionChanged)
						.OnMouseButtonDoubleClick(this, &SControlFlowBPDebugger::HandleFlowDoubleClick)
						.OnContextMenuOpening(this, &SControlFlowBPDebugger::MakeFlowContextMenu)
						.HeaderRow
						(
							SNew(SHeaderRow)
							+ SHeaderRow::Column(ColFlow).DefaultLabel(LOCTEXT("FlowColumn", "Flow")).FillWidth(0.28f)
							+ SHeaderRow::Column(ColOwner).DefaultLabel(LOCTEXT("OwnerColumn", "Owner")).FillWidth(0.2f)
							+ SHeaderRow::Column(ColState).DefaultLabel(LOCTEXT("FlowStateColumn", "State")).FillWidth(0.22f)
							+ SHeaderRow::Column(ColNow).DefaultLabel(LOCTEXT("NowColumn", "Now At")).FillWidth(0.3f)
						)
					]

					+ SOverlay::Slot()
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.Padding(16.f)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("NoFlows", "No control flows yet.\nThey appear here as soon as one is created - start Play In Editor."))
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility_Lambda([this]() { return FlowItems.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
			]

			+ SSplitter::Slot()
			.Value(0.64f)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					MakeFlowHeader()
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.f)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)

					+ SSplitter::Slot()
					.Value(0.72f)
					[
						SNew(SVerticalBox)

						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(FMargin(4.f, 4.f))
						[
							MakeViewBar()
						]

						+ SVerticalBox::Slot()
						.FillHeight(1.f)
						[
							SNew(SWidgetSwitcher)
							.WidgetIndex_Lambda([this]() { return StepView == EStepView::Graph ? 0 : 1; })

							+ SWidgetSwitcher::Slot()
							[
								MakeStepGraph()
							]

							+ SWidgetSwitcher::Slot()
							[
								SNew(SBorder)
								.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
								.Padding(2.f)
								[
									SNew(SOverlay)

									+ SOverlay::Slot()
									[
										SAssignNew(StepTree, STreeView<FStepItemPtr>)
										.TreeItemsSource(&RootStepItems)
										.SelectionMode(ESelectionMode::Single)
										.OnGenerateRow(this, &SControlFlowBPDebugger::GenerateStepRow)
										.OnGetChildren(this, &SControlFlowBPDebugger::GetStepChildren)
										.OnSelectionChanged(this, &SControlFlowBPDebugger::HandleStepSelectionChanged)
										.OnMouseButtonDoubleClick(this, &SControlFlowBPDebugger::HandleStepDoubleClick)
										.OnContextMenuOpening(this, &SControlFlowBPDebugger::MakeStepContextMenu)
										.HeaderRow
										(
											SNew(SHeaderRow)
											+ SHeaderRow::Column(ColStep).DefaultLabel(LOCTEXT("StepColumn", "Step")).FillWidth(0.27f)
											+ SHeaderRow::Column(ColType).DefaultLabel(LOCTEXT("TypeColumn", "Type")).FillWidth(0.08f)
											+ SHeaderRow::Column(ColState).DefaultLabel(LOCTEXT("StepStateColumn", "State")).FillWidth(0.08f)
											+ SHeaderRow::Column(ColStarted).DefaultLabel(LOCTEXT("StartedColumn", "Started")).FillWidth(0.08f)
											+ SHeaderRow::Column(ColDuration).DefaultLabel(LOCTEXT("DurationColumn", "Duration")).FillWidth(0.08f)
											+ SHeaderRow::Column(ColDetail).DefaultLabel(LOCTEXT("DetailColumn", "Detail")).FillWidth(0.24f)
											+ SHeaderRow::Column(ColQueuedAt).DefaultLabel(LOCTEXT("QueuedAtColumn", "Queued At")).FillWidth(0.17f)
										)
									]

									+ SOverlay::Slot()
									.HAlign(HAlign_Center)
									.VAlign(VAlign_Center)
									.Padding(16.f)
									[
										SNew(STextBlock)
										.Text_Lambda([this]()
										{
											return SelectedFlow.IsValid()
												? LOCTEXT("NoSteps", "This flow has no steps.")
												: LOCTEXT("NoFlowSelected", "Select a flow on the left to see its steps.");
										})
										.ColorAndOpacity(FSlateColor::UseSubduedForeground())
										.Visibility_Lambda([this]() { return RootStepItems.IsEmpty() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
									]
								]
							]
						]
					]

					+ SSplitter::Slot()
					.Value(0.28f)
					[
						MakeStepDetails()
					]
				]
			]
		]
	];

	Refresh();
	RegisterActiveTimer(0.25f, FWidgetActiveTimerDelegate::CreateSP(this, &SControlFlowBPDebugger::HandleRefreshTimer));
}

SControlFlowBPDebugger::~SControlFlowBPDebugger() = default;

TSharedRef<SWidget> SControlFlowBPDebugger::MakeToolbar()
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		.VAlign(VAlign_Center)
		.Padding(0.f, 0.f, 8.f, 0.f)
		[
			SNew(SSearchBox)
			.HintText(LOCTEXT("FilterHint", "Filter flows by name, owner or current step"))
			.OnTextChanged_Lambda([this](const FText& Text) -> void
			{
				FlowFilter = Text.ToString();
				Refresh();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.f, 0.f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("ShowFinishedTooltip", "Also list flows that have completed or were cancelled (ControlFlowBP.RecentFlows of them are kept)."))
			.IsChecked_Lambda([this]() { return bShowFinished ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) -> void
			{
				bShowFinished = NewState == ECheckBoxState::Checked;
				Refresh();
			})
			[
				SNew(STextBlock).Text(LOCTEXT("ShowFinished", "Finished flows"))
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(4.f, 0.f)
		[
			SNew(SCheckBox)
			.ToolTipText(LOCTEXT("FollowTooltip", "Keep the running steps in view. Tree: open the rows leading to them, and fold away the loop iterations, cases and tracks that finish. Graph: bring a newly started step into view when it is off screen."))
			.IsChecked_Lambda([this]() { return bFollowRunning ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { bFollowRunning = NewState == ECheckBoxState::Checked; })
			[
				SNew(STextBlock).Text(LOCTEXT("Follow", "Follow running steps"))
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(12.f, 0.f, 4.f, 0.f)
		[
			SNew(STextBlock).Text(LOCTEXT("TraceLabel", "Log steps:"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SComboBox<TSharedPtr<FString>>)
			.OptionsSource(&TraceLevelOptions)
			.ToolTipText(LOCTEXT("TraceTooltip", "ControlFlowBP.Trace - log every step as it starts and ends. Verbose also logs queueing and steps that never ran."))
			.OnGenerateWidget_Lambda([](TSharedPtr<FString> Option) -> TSharedRef<SWidget>
			{
				return SNew(STextBlock).Text(FText::FromString(Option.IsValid() ? *Option : FString()));
			})
			.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Option, ESelectInfo::Type SelectInfo)
			{
				if (Option.IsValid() && SelectInfo != ESelectInfo::Direct)
				{
					SetTraceLevel(TraceLevelOptions.Find(Option));
				}
			})
			[
				SNew(STextBlock).Text_Lambda([this]()
				{
					return FText::FromString(*TraceLevelOptions[FMath::Clamp(GetTraceLevel(), 0, TraceLevelOptions.Num() - 1)]);
				})
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(12.f, 0.f, 4.f, 0.f)
		[
			SNew(STextBlock).Text(LOCTEXT("BreakLabel", "Break on:"))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(220.f)
			[
				SNew(SEditableTextBox)
				.Text_Lambda([]() { return FText::FromString(GetBreakPattern()); })
				.HintText(LOCTEXT("BreakHint", "step path wildcard"))
				.ToolTipText(LOCTEXT("BreakTooltip",
					"ControlFlowBP.BreakOnStep - during Play In Editor, pause in the Blueprint debugger when a step whose path matches starts.\n"
					"e.g. Encounter.Waves#*.Spawn*   Separate several with ';'. Clear to turn off."))
				.OnTextCommitted_Lambda([](const FText& Text, ETextCommit::Type) { SetBreakPattern(Text.ToString()); })
			]
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(12.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ClearFinished", "Clear Finished"))
			.ToolTipText(LOCTEXT("ClearFinishedTooltip", "Forget the flows that have finished. Running flows stay."))
			.OnClicked_Lambda([this]() -> FReply
			{
				FControlFlowBPDebug::ClearFinishedFlows();
				if (SelectedFlow.IsValid() && SelectedFlow->IsFinished())
				{
					SelectFlow(nullptr);
				}
				Refresh();
				return FReply::Handled();
			})
		];
}

TSharedRef<SWidget> SControlFlowBPDebugger::MakeFlowHeader()
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	const auto HasFlow = [this]() { return SelectedFlow.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; };

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.f, 6.f))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.f, 0.f, 12.f, 0.f)
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
					.Text_Lambda([this]()
					{
						return SelectedFlow.IsValid()
							? FText::FromString(FString::Printf(TEXT("%s  (#%d)"), *SelectedFlow->Name, SelectedFlow->Id))
							: LOCTEXT("NoSelection", "No flow selected");
					})
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return SelectedFlow.IsValid() ? FText::FromString(SelectedFlow->DescribeState()) : FText::GetEmpty(); })
					.ColorAndOpacity_Lambda([this]() { return SelectedFlow.IsValid() ? FlowColor(*SelectedFlow) : FSlateColor::UseForeground(); })
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("DebugOwner", "Debug Owner"))
					.ToolTipText(LOCTEXT("DebugOwnerTooltip", "Open the owner's Blueprint with the owner picked as the object being debugged."))
					.Visibility_Lambda([this]()
					{
						return (SelectedFlow.IsValid() && IsBlueprintObject(SelectedFlow->Owner.Get())) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]() -> FReply
					{
						if (SelectedFlow.IsValid())
						{
							FControlFlowBPEditorDebugger::DebugInBlueprintEditor(SelectedFlow->Owner.Get());
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f)
				[
					SNew(SCheckBox)
					.Visibility_Lambda(HasFlow)
					.ToolTipText(LOCTEXT("TraceFlowTooltip", "Log this flow's steps as they start and end (Enable Step Trace)."))
					.IsChecked_Lambda([this]() { return (SelectedFlow.IsValid() && SelectedFlow->bTraceSteps) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState NewState)
					{
						if (SelectedFlow.IsValid())
						{
							SelectedFlow->bTraceSteps = NewState == ECheckBoxState::Checked;
						}
					})
					[
						SNew(STextBlock).Text(LOCTEXT("TraceFlow", "Log this flow"))
					]
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("Dump", "Dump to Log"))
					.ToolTipText(LOCTEXT("DumpTooltip", "Write this flow's running steps, queued steps and recent history to the Output Log."))
					.Visibility_Lambda(HasFlow)
					.OnClicked_Lambda([this]() -> FReply
					{
						if (SelectedFlow.IsValid())
						{
							SelectedFlow->Dump(ELogVerbosity::Display);
						}
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(4.f, 0.f, 0.f, 0.f)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelFlow", "Cancel Flow"))
					.ToolTipText(LOCTEXT("CancelFlowTooltip", "Cancel this flow, as Cancel Flow would."))
					.Visibility_Lambda([this]()
					{
						return (SelectedFlow.IsValid() && !SelectedFlow->IsFinished() && SelectedFlow->Builder.IsValid()) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.OnClicked_Lambda([this]() -> FReply
					{
						if (UControlFlowBP* Builder = SelectedFlow.IsValid() ? SelectedFlow->Builder.Get() : nullptr)
						{
							Builder->CancelWithReason(EControlFlowBPCancelCause::Requested, TEXT("cancelled from the Control Flow Debugger"));
						}
						return FReply::Handled();
					})
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(SHorizontalBox)
				.Visibility_Lambda(HasFlow)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.f, 0.f, 16.f, 0.f)
				[
					SNew(STextBlock)
					.Text_Lambda([this]()
					{
						return SelectedFlow.IsValid()
							? FText::Format(LOCTEXT("OwnerFmt", "Owner: {0}"), FText::FromString(SelectedFlow->OwnerName))
							: FText::GetEmpty();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("CreatedAt", "Created at "))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.f, 0.f, 16.f, 0.f)
				[
					SNew(SHyperlink)
					.Text_Lambda([this]()
					{
						return (SelectedFlow.IsValid() && SelectedFlow->CreateSite.IsSet())
							? FText::FromString(SelectedFlow->CreateSite.Describe())
							: LOCTEXT("Unknown", "(not from a Blueprint)");
					})
					.OnNavigate_Lambda([this]()
					{
						if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger(); EditorDebugger && SelectedFlow.IsValid())
						{
							EditorDebugger->TryJumpToCallSite(SelectedFlow->CreateSite);
						}
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(STextBlock).Text(LOCTEXT("ExecutedAt", "Executed at "))
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SHyperlink)
					.Text_Lambda([this]() -> FText
					{
						if (!SelectedFlow.IsValid() || SelectedFlow->State == EControlFlowBPFlowState::Building)
						{
							return LOCTEXT("NotExecuted", "(Execute Flow not called yet)");
						}

						return SelectedFlow->ExecuteSite.IsSet()
							? FText::FromString(SelectedFlow->ExecuteSite.Describe())
							: LOCTEXT("Unknown", "(not from a Blueprint)");
					})
					.OnNavigate_Lambda([this]()
					{
						if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger(); EditorDebugger && SelectedFlow.IsValid())
						{
							EditorDebugger->TryJumpToCallSite(SelectedFlow->ExecuteSite);
						}
					})
				]
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(CancelledColor))
				.Visibility_Lambda([this]()
				{
					return (SelectedFlow.IsValid() && SelectedFlow->State == EControlFlowBPFlowState::Cancelled) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					return SelectedFlow.IsValid()
						? FText::Format(LOCTEXT("CancelledFmt", "Cancelled: {0}"), FText::FromString(SelectedFlow->CancelReason))
						: FText::GetEmpty();
				})
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Visibility_Lambda([this]()
				{
					return (SelectedFlow.IsValid() && !SelectedFlow->StepFailurePolicy.IsEmpty()) ? EVisibility::Visible : EVisibility::Collapsed;
				})
				.Text_Lambda([this]()
				{
					return SelectedFlow.IsValid()
						? FText::Format(LOCTEXT("FailurePolicyFmt", "When a step fails: {0}"), FText::FromString(SelectedFlow->StepFailurePolicy))
						: FText::GetEmpty();
				})
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Visibility_Lambda([this]()
				{
					return GetFlowVariablesText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
				})
				.Text_Lambda([this]()
				{
					return FText::Format(LOCTEXT("VariablesFmt", "Variables: {0}"), FText::FromString(GetFlowVariablesText()));
				})
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.f, 4.f, 0.f, 0.f)
			[
				SNew(STextBlock)
				.Visibility_Lambda(HasFlow)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text_Lambda([this]() { return GetFlowCounts(); })
			]
		];
}

TSharedRef<SWidget> SControlFlowBPDebugger::MakeViewBar()
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	const auto LegendEntry = [](const FLinearColor& Color, const FText& Label, const FText& Tooltip) -> TSharedRef<SWidget>
	{
		return SNew(SHorizontalBox)
			.ToolTipText(Tooltip)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(10.f, 0.f, 4.f, 0.f)
			[
				SNew(SBox)
				.WidthOverride(8.f)
				.HeightOverride(8.f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity(FSlateColor(Color))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(Label)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
	};

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SSegmentedControl<EStepView>)
			.Value_Lambda([this]() { return StepView; })
			.OnValueChanged(this, &SControlFlowBPDebugger::SetStepView)

			+ SSegmentedControl<EStepView>::Slot(EStepView::Graph)
			.Text(LOCTEXT("GraphView", "Graph"))
			.ToolTip(LOCTEXT("GraphViewTooltip", "The flow as nodes and wires, the way a Blueprint reads: each step a node, the flows a step runs in boxes below it, wires lit where execution has been."))

			+ SSegmentedControl<EStepView>::Slot(EStepView::Tree)
			.Text(LOCTEXT("TreeView", "Tree"))
			.ToolTip(LOCTEXT("TreeViewTooltip", "The flow as a tree, with every step's timing in columns."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(8.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.Text(LOCTEXT("ZoomToFitButton", "Zoom to Fit"))
			.ToolTipText(LOCTEXT("ZoomToFitButtonTooltip", "Fit the whole flow into view."))
			.Visibility_Lambda([this]() { return StepView == EStepView::Graph ? EVisibility::Visible : EVisibility::Collapsed; })
			.IsEnabled_Lambda([this]() { return SelectedFlow.IsValid(); })
			.OnClicked_Lambda([this]() -> FReply
			{
				if (GraphEditor.IsValid())
				{
					GraphEditor->ZoomToFit(false);
				}
				return FReply::Handled();
			})
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.f)
		[
			SNullWidget::NullWidget
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(RunningColor, LOCTEXT("LegendRunning", "running"), LOCTEXT("LegendRunningTooltip", "Running now. In the graph, the wire into it is animated."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(WaitingColor, LOCTEXT("LegendWaiting", "waiting"), LOCTEXT("LegendWaitingTooltip", "A Wait or Delay step, waiting."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(DoneColor, LOCTEXT("LegendDone", "done"), LOCTEXT("LegendDoneTooltip", "Finished."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(FailedColor, LOCTEXT("LegendFailed", "failed"), LOCTEXT("LegendFailedTooltip", "Failed - timed out, or its event was missing."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(CancelledColor, LOCTEXT("LegendCancelled", "cancelled"), LOCTEXT("LegendCancelledTooltip", "Cancelled while running, or never ran because its flow was cancelled."))
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			LegendEntry(InactiveColor, LOCTEXT("LegendQueued", "queued / skipped"), LOCTEXT("LegendQueuedTooltip", "Queued and not reached yet, or a switch case that was not taken."))
		];
}

TSharedRef<SWidget> SControlFlowBPDebugger::MakeStepDetails()
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	const auto HasStep = [this]() { return SelectedStep.IsValid() ? EVisibility::Visible : EVisibility::Collapsed; };

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(8.f, 6.f))
		[
			SNew(SScrollBox)

			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]() { return GetStepDetailsText(); })
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { return (SelectedStep.IsValid() && SelectedStep->QueueSite.IsSet()) ? EVisibility::Visible : EVisibility::Collapsed; })

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock).Text(LOCTEXT("DetailsQueuedAt", "Queued at "))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SHyperlink)
						.Text_Lambda([this]() { return SelectedStep.IsValid() ? FText::FromString(SelectedStep->QueueSite.Describe()) : FText::GetEmpty(); })
						.OnNavigate_Lambda([this]()
						{
							if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger(); EditorDebugger && SelectedStep.IsValid())
							{
								EditorDebugger->TryJumpToCallSite(SelectedStep->QueueSite);
							}
						})
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 2.f, 0.f, 0.f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { return (SelectedStep.IsValid() && !SelectedStep->HandlerFunction.IsNone()) ? EVisibility::Visible : EVisibility::Collapsed; })

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock).Text(LOCTEXT("DetailsRuns", "Runs "))
					]

					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(SHyperlink)
						.Text_Lambda([this]() -> FText
						{
							if (!SelectedStep.IsValid())
							{
								return FText::GetEmpty();
							}

							const UObject* Handler = SelectedStep->HandlerObject.Get();
							return FText::FromString(FString::Printf(TEXT("%s on %s"), *SelectedStep->HandlerFunction.ToString(), Handler ? *Handler->GetName() : TEXT("(gone)")));
						})
						.ToolTipText(LOCTEXT("RunsTooltip", "Go to the event or function that implements this step."))
						.OnNavigate_Lambda([this]()
						{
							if (FControlFlowBPEditorDebugger* EditorDebugger = Debugger(); EditorDebugger && SelectedStep.IsValid())
							{
								EditorDebugger->TryJumpToHandler(*SelectedStep);
							}
						})
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 6.f, 0.f, 0.f)
				[
					SNew(SCheckBox)
					.Visibility_Lambda([this]() -> EVisibility
					{
						const UEdGraphNode* QueueNode = SelectedStep.IsValid() ? FindQueueNode(*SelectedStep) : nullptr;
						return FControlFlowBPEditorDebugger::IsQueueNode(QueueNode, true) ? EVisibility::Visible : EVisibility::Collapsed;
					})
					.IsChecked_Lambda([this]() -> ECheckBoxState
					{
						const FControlFlowBPEditorDebugger* EditorDebugger = Debugger();
						const UEdGraphNode* QueueNode = SelectedStep.IsValid() ? FindQueueNode(*SelectedStep) : nullptr;
						return (EditorDebugger && QueueNode && EditorDebugger->IsBreakEnabled(QueueNode->NodeGuid)) ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState)
					{
						FControlFlowBPEditorDebugger* EditorDebugger = Debugger();
						if (const UEdGraphNode* QueueNode = SelectedStep.IsValid() ? FindQueueNode(*SelectedStep) : nullptr; EditorDebugger && QueueNode)
						{
							EditorDebugger->ToggleBreak(QueueNode->NodeGuid);
						}
					})
					[
						SNew(STextBlock).Text(LOCTEXT("BreakWhenStepRuns", "Break When This Step Runs (for its Queue node)"))
					]
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Visibility_Lambda([HasStep]() { return HasStep() == EVisibility::Visible ? EVisibility::Collapsed : EVisibility::Visible; })
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("NoStepSelected", "Select a step to see everything known about it. Double-click a step to go to its Queue node."))
				]
			]
		];
}

FString SControlFlowBPDebugger::GetFlowVariablesText() const
{
	if (!SelectedFlow.IsValid())
	{
		return FString();
	}

	if (SelectedFlow->IsFinished())
	{
		return SelectedFlow->FinalVariables;
	}

	const UControlFlowBP* Builder = SelectedFlow->Builder.Get();
	return Builder ? Builder->DescribeFlowVariables() : FString();
}

FText SControlFlowBPDebugger::GetFlowCounts() const
{
	if (!SelectedFlow.IsValid())
	{
		return FText::GetEmpty();
	}

	int32 Counts[6] = {};
	SelectedFlow->ForEachStep([&Counts](const TSharedRef<FControlFlowBPStepRecord>& Step)
	{
		if (!Step->IsInternal())
		{
			++Counts[static_cast<int32>(Step->State)];
		}
	});

	FString Text = FString::Printf(TEXT("%d running, %d queued, %d done"),
		Counts[static_cast<int32>(EControlFlowBPStepState::Running)],
		Counts[static_cast<int32>(EControlFlowBPStepState::Pending)],
		Counts[static_cast<int32>(EControlFlowBPStepState::Succeeded)]);

	const auto AddCount = [&Text, &Counts](EControlFlowBPStepState State, const TCHAR* Label)
	{
		if (Counts[static_cast<int32>(State)] > 0)
		{
			Text += FString::Printf(TEXT(", %d %s"), Counts[static_cast<int32>(State)], Label);
		}
	};
	AddCount(EControlFlowBPStepState::Failed, TEXT("failed"));
	AddCount(EControlFlowBPStepState::Cancelled, TEXT("cancelled"));
	AddCount(EControlFlowBPStepState::Skipped, TEXT("skipped"));

	if (SelectedFlow->NumForgotten > 0)
	{
		Text += FString::Printf(TEXT("   (%d earlier finished steps not kept - raise ControlFlowBP.HistorySize to keep more)"), SelectedFlow->NumForgotten);
	}

	return FText::FromString(Text);
}

FText SControlFlowBPDebugger::GetStepDetailsText() const
{
	if (!SelectedStep.IsValid())
	{
		return FText::GetEmpty();
	}

	const FControlFlowBPStepRecord& Step = *SelectedStep;

	if (IsPreview(Step))
	{
		return FText::Format(LOCTEXT("PreviewDetails",
			"{0}\n{1} step - preview, not queued yet: {2}.\n"
			"Read from the Blueprint, so the step that runs may differ. Double-click it to go to its Queue node."),
			FText::FromString(Step.Path), FText::FromString(FControlFlowBPStepRecord::LexType(Step.Type)), FText::FromString(Step.Info));
	}

	FString Text = FString::Printf(TEXT("%s\n%s step - %s"), *Step.Path, FControlFlowBPStepRecord::LexType(Step.Type), *Step.DescribeState());

	if (!Step.Info.IsEmpty())
	{
		Text += FString::Printf(TEXT("\nQueued as: %s"), *Step.Info);
	}

	if (Step.Type == EControlFlowBPStepType::Loop && Step.Iterations > 0)
	{
		Text += FString::Printf(TEXT("\nIterations: %d"), Step.Iterations);
	}

	if (const TSharedPtr<FControlFlowBPFlowRecord> Flow = Step.Flow.Pin(); Flow.IsValid() && Flow->StartTime > 0.0)
	{
		if (Step.StartTime > 0.0)
		{
			Text += FString::Printf(TEXT("\nStarted %.3f s into the flow"), Step.StartTime - Flow->StartTime);
		}

		Text += FString::Printf(TEXT(", queued %.3f s %s the flow started"),
			FMath::Abs(Step.QueuedTime - Flow->StartTime),
			Step.QueuedTime < Flow->StartTime ? TEXT("before") : TEXT("after"));
	}

	return FText::FromString(Text);
}

EActiveTimerReturnType SControlFlowBPDebugger::HandleRefreshTimer(double InCurrentTime, float InDeltaTime)
{
	Refresh();
	return EActiveTimerReturnType::Continue;
}

void SControlFlowBPDebugger::Refresh()
{
	RefreshFlows();
	RefreshSteps();
}

void SControlFlowBPDebugger::RefreshFlows()
{
	TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
	FControlFlowBPDebug::GetFlows(Flows);

	const int32 FirstFinished = Flows.IndexOfByPredicate([](const TSharedRef<FControlFlowBPFlowRecord>& Flow) { return Flow->IsFinished(); });
	if (FirstFinished != INDEX_NONE)
	{
		Algo::Reverse(MakeArrayView(Flows).RightChop(FirstFinished));
	}

	const auto MatchesFilter = [this](const FControlFlowBPFlowRecord& Flow) -> bool
	{
		if (FlowFilter.IsEmpty() || Flow.Name.Contains(FlowFilter) || Flow.OwnerName.Contains(FlowFilter))
		{
			return true;
		}

		TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
		Flow.GetActiveSteps(Active, true);
		return Active.ContainsByPredicate([this](const TSharedRef<FControlFlowBPStepRecord>& Step) { return Step->Path.Contains(FlowFilter); });
	};

	TMap<int32, FFlowItemPtr> NewItems;
	FlowItems.Reset();

	for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
	{
		if ((!bShowFinished && Flow->IsFinished()) || !MatchesFilter(*Flow))
		{
			continue;
		}

		FFlowItemPtr ItemPtr = FlowItemsById.FindRef(Flow->Id);
		if (!ItemPtr.IsValid() || ItemPtr->Flow != Flow)
		{
			ItemPtr = MakeShared<FControlFlowBPDebuggerFlowItem>(Flow);
		}

		NewItems.Add(Flow->Id, ItemPtr);
		FlowItems.Add(ItemPtr);
	}

	FlowItemsById = MoveTemp(NewItems);
	FlowList->RequestListRefresh();

	if (FlowList->GetNumItemsSelected() == 0)
	{
		SyncFlowListSelection();
	}
}

void SControlFlowBPDebugger::SyncFlowListSelection()
{
	if (!SelectedFlow.IsValid())
	{
		FlowList->ClearSelection();
		return;
	}

	if (const FFlowItemPtr* Item = FlowItemsById.Find(SelectedFlow->Id); Item && !FlowList->IsItemSelected(*Item))
	{
		FlowList->SetSelection(*Item, ESelectInfo::Direct);
	}
}

void SControlFlowBPDebugger::RefreshSteps()
{
	if (!SelectedFlow.IsValid())
	{
		RootStepItems.Reset();
		StepItemsByRecord.Reset();
		GroupItemsByKey.Reset();
		AutoExpandedItems.Reset();
		Previews.Reset();
		StepTree->RequestTreeRefresh();
		UpdateGraph();
		return;
	}

	TArray<TSharedRef<FControlFlowBPStepRecord>> Records;
	TSet<const FControlFlowBPStepRecord*> Known;
	SelectedFlow->ForEachStep([&Records, &Known](const TSharedRef<FControlFlowBPStepRecord>& Step)
	{
		if (!Step->IsInternal() && !Known.Contains(&Step.Get()))
		{
			Known.Add(&Step.Get());
			Records.Add(Step);
		}
	});
	
	FChildrenMap ChildrenOf;
	TArray<TSharedRef<FControlFlowBPStepRecord>> Roots;
	for (const TSharedRef<FControlFlowBPStepRecord>& Record : Records)
	{
		const TSharedPtr<FControlFlowBPStepRecord> Parent = Record->Parent.Pin();
		if (Parent.IsValid() && Known.Contains(Parent.Get()))
		{
			ChildrenOf.FindOrAdd(Parent.Get()).Add(Record);
		}
		else
		{
			Roots.Add(Record);
		}
	}

	const auto BySequence = [](const TSharedRef<FControlFlowBPStepRecord>& A, const TSharedRef<FControlFlowBPStepRecord>& B) { return A->Sequence < B->Sequence; };
	Roots.Sort(BySequence);
	for (TPair<const FControlFlowBPStepRecord*, TArray<TSharedRef<FControlFlowBPStepRecord>>>& Pair : ChildrenOf)
	{
		Pair.Value.Sort(BySequence);
	}

	TMap<const FControlFlowBPStepRecord*, FStepItemPtr> NewStepItems;
	TMap<FString, FStepItemPtr> NewGroupItems;
	RootStepItems.Reset();
	for (const TSharedRef<FControlFlowBPStepRecord>& Root : Roots)
	{
		RootStepItems.Add(BuildStepItem(Root, nullptr, ChildrenOf, NewStepItems, NewGroupItems));
	}

	StepItemsByRecord = MoveTemp(NewStepItems);
	GroupItemsByKey = MoveTemp(NewGroupItems);

	for (auto It = Previews.CreateIterator(); It; ++It)
	{
		const TSharedPtr<FControlFlowBPStepRecord> Step = It.Value().Step.Pin();
		if (!Step.IsValid() || Step->State != EControlFlowBPStepState::Pending)
		{
			It.RemoveCurrent();
		}
	}

	if (SelectedStep.IsValid() && !StepItemsByRecord.Contains(SelectedStep.Get()))
	{
		SelectedStep.Reset();
	}

	for (auto It = AutoExpandedItems.CreateIterator(); It; ++It)
	{
		const FStepItemPtr& Expanded = *It;
		const bool bStillShown = Expanded->IsGroup()
			? GroupItemsByKey.FindKey(Expanded) != nullptr
			: StepItemsByRecord.Contains(Expanded->Step.Get());

		if (!bStillShown)
		{
			It.RemoveCurrent();
		}
	}

	if (bFollowRunning)
	{
		FollowRunningSteps(RootStepItems);
	}

	StepTree->RequestTreeRefresh();
	UpdateGraph();
}

SControlFlowBPDebugger::FStepItemPtr SControlFlowBPDebugger::BuildStepItem(
	const TSharedRef<FControlFlowBPStepRecord>& Step,
	const FStepItemPtr& ParentItem,
	const FChildrenMap& ChildrenOf,
	TMap<const FControlFlowBPStepRecord*, FStepItemPtr>& OutStepItems,
	TMap<FString, FStepItemPtr>& OutGroupItems)
{
	FStepItemPtr ItemPtr = StepItemsByRecord.FindRef(&Step.Get());
	if (!ItemPtr.IsValid())
	{
		ItemPtr = MakeShared<FControlFlowBPDebuggerStepItem>();
	}

	ItemPtr->Step = Step;
	ItemPtr->ParentItem = ParentItem;
	ItemPtr->Children.Reset();
	ItemPtr->bSubtreeRunning = Step->State == EControlFlowBPStepState::Running;
	ItemPtr->bPreview = false;
	ItemPtr->PreviewNode.Reset();
	OutStepItems.Add(&Step.Get(), ItemPtr);

	const TArray<TSharedRef<FControlFlowBPStepRecord>>* Kids = ChildrenOf.Find(&Step.Get());
	if (!Kids)
	{
		if (Step->State == EControlFlowBPStepState::Pending && FControlFlowBPStepRecord::RunsFlows(Step->Type))
		{
			AddPreviewItems(ItemPtr, Step, GetPreviewLanes(Step), OutStepItems, OutGroupItems);
		}
		return ItemPtr;
	}
	
	TArray<FString> GroupOrder;
	TMap<FString, TArray<TSharedRef<FControlFlowBPStepRecord>>> ByGroup;
	for (const TSharedRef<FControlFlowBPStepRecord>& Kid : *Kids)
	{
		if (Kid->Group.IsEmpty())
		{
			FStepItemPtr KidItem = BuildStepItem(Kid, ItemPtr, ChildrenOf, OutStepItems, OutGroupItems);
			ItemPtr->bSubtreeRunning |= KidItem->bSubtreeRunning;
			ItemPtr->Children.Add(KidItem);
			continue;
		}

		if (!ByGroup.Contains(Kid->Group))
		{
			GroupOrder.Add(Kid->Group);
		}
		ByGroup.FindOrAdd(Kid->Group).Add(Kid);
	}

	for (const FString& GroupName : GroupOrder)
	{
		const FString Key = FString::Printf(TEXT("%llu/%s"), static_cast<uint64>(reinterpret_cast<UPTRINT>(&Step.Get())), *GroupName);

		FStepItemPtr GroupItem = GroupItemsByKey.FindRef(Key);
		if (!GroupItem.IsValid())
		{
			GroupItem = MakeShared<FControlFlowBPDebuggerStepItem>();
		}

		GroupItem->Step.Reset();
		GroupItem->GroupOwner = Step;
		GroupItem->GroupName = GroupName;
		GroupItem->ParentItem = ItemPtr;
		GroupItem->Children.Reset();
		GroupItem->bSubtreeRunning = false;
		GroupItem->bPreview = false;
		GroupItem->PreviewNode.Reset();
		OutGroupItems.Add(Key, GroupItem);

		for (const TSharedRef<FControlFlowBPStepRecord>& Kid : ByGroup[GroupName])
		{
			FStepItemPtr KidItem = BuildStepItem(Kid, GroupItem, ChildrenOf, OutStepItems, OutGroupItems);
			GroupItem->bSubtreeRunning |= KidItem->bSubtreeRunning;
			GroupItem->Children.Add(KidItem);
		}

		ItemPtr->bSubtreeRunning |= GroupItem->bSubtreeRunning;
		ItemPtr->Children.Add(GroupItem);
	}

	return ItemPtr;
}

const TArray<FControlFlowBPPreviewLane>& SControlFlowBPDebugger::GetPreviewLanes(const TSharedRef<FControlFlowBPStepRecord>& Step)
{
	FPreviewEntry* Entry = Previews.Find(&Step.Get());
	if (!Entry || Entry->Step.Pin() != Step)
	{
		Entry = &Previews.Add(&Step.Get(), FPreviewEntry{ Step, FControlFlowBPDebugPreview::PredictLanes(*Step) });
	}

	return Entry->Lanes;
}

void SControlFlowBPDebugger::AddPreviewItems(
	const FStepItemPtr& OwnerItem,
	const TSharedRef<FControlFlowBPStepRecord>& Owner,
	const TArray<FControlFlowBPPreviewLane>& Lanes,
	TMap<const FControlFlowBPStepRecord*, FStepItemPtr>& OutStepItems,
	TMap<FString, FStepItemPtr>& OutGroupItems)
{
	for (const FControlFlowBPPreviewLane& Lane : Lanes)
	{
		FStepItemPtr ParentItem = OwnerItem;
		if (!Lane.Name.IsEmpty() || Owner->Type == EControlFlowBPStepType::Loop)
		{
			const FString GroupName = Lane.Name.IsEmpty() ? FString(TEXT("Each")) : Lane.Name;
			const FString Key = FString::Printf(TEXT("%llu/preview/%s"), static_cast<uint64>(reinterpret_cast<UPTRINT>(&Owner.Get())), *GroupName);

			FStepItemPtr GroupItem = GroupItemsByKey.FindRef(Key);
			if (!GroupItem.IsValid())
			{
				GroupItem = MakeShared<FControlFlowBPDebuggerStepItem>();
			}

			GroupItem->Step.Reset();
			GroupItem->GroupOwner = Owner;
			GroupItem->GroupName = GroupName;
			GroupItem->ParentItem = OwnerItem;
			GroupItem->Children.Reset();
			GroupItem->bSubtreeRunning = false;
			GroupItem->bPreview = true;
			GroupItem->PreviewNode = Lane.SourceNode;
			OutGroupItems.Add(Key, GroupItem);

			OwnerItem->Children.Add(GroupItem);
			ParentItem = GroupItem;
		}

		for (const TSharedRef<FControlFlowBPPreviewStep>& PreviewStep : Lane.Steps)
		{
			const TSharedRef<FControlFlowBPStepRecord>& Record = PreviewStep->Record;

			FStepItemPtr StepItem = StepItemsByRecord.FindRef(&Record.Get());
			if (!StepItem.IsValid())
			{
				StepItem = MakeShared<FControlFlowBPDebuggerStepItem>();
			}

			StepItem->Step = Record;
			StepItem->ParentItem = ParentItem;
			StepItem->Children.Reset();
			StepItem->bSubtreeRunning = false;
			StepItem->bPreview = true;
			StepItem->PreviewNode = PreviewStep->QueueNode;
			OutStepItems.Add(&Record.Get(), StepItem);

			ParentItem->Children.Add(StepItem);
			AddPreviewItems(StepItem, Record, PreviewStep->Lanes, OutStepItems, OutGroupItems);
		}
	}
}

bool SControlFlowBPDebugger::IsPreview(const FControlFlowBPStepRecord& Step) const
{
	const FStepItemPtr* Item = StepItemsByRecord.Find(&Step);
	return Item && (*Item)->bPreview;
}

const UEdGraphNode* SControlFlowBPDebugger::FindQueueNode(const FControlFlowBPStepRecord& Step) const
{
	if (const FStepItemPtr* Item = StepItemsByRecord.Find(&Step); Item && (*Item)->bPreview)
	{
		return (*Item)->PreviewNode.Get();
	}

	FControlFlowBPEditorDebugger* EditorDebugger = UE::ControlFlowBP::DebuggerTab::Debugger();
	return EditorDebugger ? EditorDebugger->ResolveNode(Step.QueueSite) : nullptr;
}

void SControlFlowBPDebugger::FollowRunningSteps(const TArray<FStepItemPtr>& Items)
{
	for (const FStepItemPtr& ItemPtr : Items)
	{
		const bool bHasChildren = ItemPtr->Children.Num() > 0;

		if (ItemPtr->bSubtreeRunning && bHasChildren && !AutoExpandedItems.Contains(ItemPtr))
		{
			AutoExpandedItems.Add(ItemPtr);
			StepTree->SetItemExpansion(ItemPtr, true);
		}
		else if (!ItemPtr->bSubtreeRunning && ItemPtr->IsGroup() && AutoExpandedItems.Contains(ItemPtr))
		{
			AutoExpandedItems.Remove(ItemPtr);
			StepTree->SetItemExpansion(ItemPtr, false);
		}

		FollowRunningSteps(ItemPtr->Children);
	}
}

void SControlFlowBPDebugger::SelectFlow(const TSharedPtr<FControlFlowBPFlowRecord>& Flow)
{
	if (SelectedFlow == Flow)
	{
		return;
	}

	SelectedFlow = Flow;
	SelectedStep.Reset();

	RootStepItems.Reset();
	StepItemsByRecord.Reset();
	GroupItemsByKey.Reset();
	AutoExpandedItems.Reset();
	Previews.Reset();
	StepTree->ClearSelection();
	
	bZoomToFitPending = true;
	LastFollowedStep.Reset();

	Refresh();

	SyncFlowListSelection();
}

void SControlFlowBPDebugger::SelectStep(const TSharedPtr<FControlFlowBPStepRecord>& Step)
{
	SelectedStep = Step;

	TGuardValue<bool> Guard(bSyncingSelection, true);
	SelectStepInTree(Step);
	SelectStepInGraph(Step, true);
}

TSharedRef<ITableRow> SControlFlowBPDebugger::GenerateFlowRow(FFlowItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(SControlFlowBPDebuggerFlowRow, OwnerTable).Item(Item);
}

void SControlFlowBPDebugger::HandleFlowSelectionChanged(FFlowItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && SelectInfo != ESelectInfo::Direct)
	{
		SelectFlow(Item->Flow);
	}
}

void SControlFlowBPDebugger::HandleFlowDoubleClick(FFlowItemPtr Item) const
{
	if (FControlFlowBPEditorDebugger* EditorDebugger = UE::ControlFlowBP::DebuggerTab::Debugger(); EditorDebugger && Item.IsValid())
	{
		if (!EditorDebugger->TryJumpToCallSite(Item->Flow->CreateSite))
		{
			EditorDebugger->TryJumpToCallSite(Item->Flow->ExecuteSite);
		}
	}
}

TSharedPtr<SWidget> SControlFlowBPDebugger::MakeFlowContextMenu() const
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	const TArray<FFlowItemPtr> Selected = FlowList->GetSelectedItems();
	if (Selected.IsEmpty())
	{
		return nullptr;
	}

	FMenuBuilder MenuBuilder(true, nullptr);
	AddFlowMenuEntries(MenuBuilder, Selected[0]->Flow);
	return MenuBuilder.MakeWidget();
}

TSharedRef<ITableRow> SControlFlowBPDebugger::GenerateStepRow(FStepItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable) const
{
	return SNew(SControlFlowBPDebuggerStepRow, OwnerTable).Item(Item);
}

void SControlFlowBPDebugger::GetStepChildren(FStepItemPtr Item, TArray<FStepItemPtr>& OutChildren) const
{
	if (Item.IsValid())
	{
		OutChildren = Item->Children;
	}
}

void SControlFlowBPDebugger::HandleStepSelectionChanged(FStepItemPtr Item, ESelectInfo::Type SelectInfo)
{
	if (Item.IsValid() && Item->Step.IsValid())
	{
		SelectedStep = Item->Step;

		if (!bSyncingSelection)
		{
			SelectStepInGraph(Item->Step, false);
		}
	}
}

void SControlFlowBPDebugger::HandleStepDoubleClick(FStepItemPtr Item)
{
	if (!Item.IsValid())
	{
		return;
	}

	if (Item->bPreview && Item->Step.IsValid())
	{
		if (const UEdGraphNode* QueueNode = Item->PreviewNode.Get())
		{
			FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(QueueNode);
		}
	}
	else if (Item->Step.IsValid())
	{
		UE::ControlFlowBP::DebuggerTab::JumpToQueueNodeOrHandler(*Item->Step);
	}
	else
	{
		StepTree->SetItemExpansion(Item, !StepTree->IsItemExpanded(Item));
	}
}

TSharedPtr<SWidget> SControlFlowBPDebugger::MakeStepContextMenu()
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	const TArray<FStepItemPtr> Selected = StepTree->GetSelectedItems();
	if (Selected.IsEmpty() || !Selected[0]->Step.IsValid())
	{
		return nullptr;
	}

	const TSharedPtr<FControlFlowBPStepRecord> Step = Selected[0]->Step;

	FMenuBuilder MenuBuilder(true, nullptr);
	AddStepMenuEntries(MenuBuilder, Step, FindQueueNode(*Step));

	MenuBuilder.AddMenuSeparator();
	MenuBuilder.AddMenuEntry(
		LOCTEXT("ShowInGraph", "Show in Graph"),
		LOCTEXT("ShowInGraphTooltip", "Switch to the graph, centred on this step."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this, Step]() -> void
		{
			SetStepView(EStepView::Graph);
			SelectStep(Step);
		})));

	return MenuBuilder.MakeWidget();
}

void SControlFlowBPDebugger::SelectStepInTree(const TSharedPtr<FControlFlowBPStepRecord>& Step)
{
	const FStepItemPtr* ItemPtr = Step.IsValid() ? StepItemsByRecord.Find(Step.Get()) : nullptr;
	if (!ItemPtr || !StepTree.IsValid())
	{
		return;
	}

	for (FStepItemPtr Ancestor = (*ItemPtr)->ParentItem.Pin(); Ancestor.IsValid(); Ancestor = Ancestor->ParentItem.Pin())
	{
		StepTree->SetItemExpansion(Ancestor, true);
	}

	StepTree->SetSelection(*ItemPtr, ESelectInfo::Direct);
	StepTree->RequestScrollIntoView(*ItemPtr);
}

TSharedRef<SWidget> SControlFlowBPDebugger::MakeStepGraph()
{
	StepGraph.Reset(NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient));
	StepGraph->Schema = UControlFlowBPDebugGraphSchema::StaticClass();
	GraphBuilder = MakeShared<FControlFlowBPDebugGraphBuilder>(StepGraph.Get());

	SGraphEditor::FGraphEditorEvents Events;
	Events.OnSelectionChanged = SGraphEditor::FOnSelectionChanged::CreateSP(this, &SControlFlowBPDebugger::HandleGraphSelectionChanged);
	Events.OnNodeDoubleClicked = FSingleNodeEvent::CreateSP(this, &SControlFlowBPDebugger::HandleGraphNodeDoubleClicked);
	Events.OnCreateNodeOrPinMenu = SGraphEditor::FOnCreateNodeOrPinMenu::CreateSP(this, &SControlFlowBPDebugger::MakeGraphNodeMenu);

	FGraphAppearanceInfo Appearance;
	Appearance.CornerText = LOCTEXT("GraphCornerText", "CONTROL FLOW");

	return SNew(SOverlay)

		+ SOverlay::Slot()
		[
			SAssignNew(GraphEditor, SGraphEditor)
			.GraphToEdit(StepGraph.Get())
			.IsEditable(false)
			.ShowGraphStateOverlay(false)
			.Appearance(Appearance)
			.GraphEvents(Events)
		]

		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		.Padding(16.f)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoFlowSelectedGraph", "Select a flow on the left to see its steps."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([this]() { return SelectedFlow.IsValid() ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
		];
}

void SControlFlowBPDebugger::SetStepView(EStepView NewView)
{
	if (StepView == NewView)
	{
		return;
	}

	StepView = NewView;
	GConfig->SetBool(UE::ControlFlowBP::DebuggerTab::ConfigSection, UE::ControlFlowBP::DebuggerTab::ConfigShowGraph, NewView == EStepView::Graph, GEditorPerProjectIni);

	if (NewView == EStepView::Graph)
	{
		bZoomToFitPending = true;
		UpdateGraph();

		TGuardValue<bool> Guard(bSyncingSelection, true);
		SelectStepInGraph(SelectedStep, true);
	}
	else
	{
		TGuardValue<bool> Guard(bSyncingSelection, true);
		SelectStepInTree(SelectedStep);
	}
}

void SControlFlowBPDebugger::UpdateGraph()
{
	if (StepView != EStepView::Graph || !GraphBuilder.IsValid() || !GraphEditor.IsValid())
	{
		return;
	}

	TArray<UEdGraphNode*> Repinned;
	if (GraphBuilder->Update(SelectedFlow, RootStepItems, Repinned))
	{
		for (UEdGraphNode* Node : Repinned)
		{
			GraphEditor->RefreshNode(*Node);
		}

		TArray<UEdGraphNode*> Dropped;
		for (UObject* Selected : GraphEditor->GetSelectedNodes())
		{
			UEdGraphNode* Node = Cast<UEdGraphNode>(Selected);
			if (Node && !StepGraph->Nodes.Contains(Node))
			{
				Dropped.Add(Node);
			}
		}

		TGuardValue<bool> Guard(bSyncingSelection, true);
		for (UEdGraphNode* Node : Dropped)
		{
			GraphEditor->SetNodeSelection(Node, false);
		}
	}

	GraphBuilder->RefreshLiveState();

	if (bZoomToFitPending && SelectedFlow.IsValid())
	{
		bZoomToFitPending = false;
		GraphEditor->ZoomToFit(false);
		FollowBlockedUntil = FPlatformTime::Seconds() + 1.5;
	}

	FollowRunningStepInGraph();
}

void SControlFlowBPDebugger::FollowRunningStepInGraph()
{
	if (!bFollowRunning || !SelectedFlow.IsValid() || SelectedFlow->IsFinished() || FPlatformTime::Seconds() < FollowBlockedUntil)
	{
		return;
	}
	
	TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
	SelectedFlow->GetActiveSteps(Active, true);

	TSharedPtr<FControlFlowBPStepRecord> Latest;
	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
	{
		if (!Step->IsInternal() && !FControlFlowBPStepRecord::RunsFlows(Step->Type) && (!Latest.IsValid() || Step->StartTime > Latest->StartTime))
		{
			Latest = Step;
		}
	}

	if (!Latest.IsValid() || LastFollowedStep.HasSameObject(Latest.Get()))
	{
		return;
	}

	const UControlFlowBPDebugGraphNode* Node = GraphBuilder->FindNode(Latest.Get());
	if (!Node)
	{
		return;
	}

	LastFollowedStep = Latest;
	
	FVector2f ViewLocation = FVector2f::ZeroVector;
	float Zoom = 1.f;
	GraphEditor->GetViewLocation(ViewLocation, Zoom);

	const FVector2f ViewSize = GraphEditor->GetTickSpaceGeometry().GetLocalSize() / FMath::Max(Zoom, 0.01f);
	const FVector2f NodeMin(static_cast<float>(Node->NodePosX), static_cast<float>(Node->NodePosY));
	const FVector2f NodeMax = NodeMin + FVector2f(270.f, 110.f);

	const bool bInView = NodeMin.X >= ViewLocation.X && NodeMin.Y >= ViewLocation.Y
		&& NodeMax.X <= ViewLocation.X + ViewSize.X && NodeMax.Y <= ViewLocation.Y + ViewSize.Y;

	if (!bInView)
	{
		GraphEditor->JumpToNode(Node, false, false);
	}
}

void SControlFlowBPDebugger::SelectStepInGraph(const TSharedPtr<FControlFlowBPStepRecord>& Step, bool bCenter)
{
	if (!GraphEditor.IsValid() || !GraphBuilder.IsValid())
	{
		return;
	}
	
	UControlFlowBPDebugGraphNode* Node = Step.IsValid() ? GraphBuilder->FindNode(Step.Get()) : nullptr;
	if (!Node)
	{
		return;
	}

	TGuardValue<bool> Guard(bSyncingSelection, true);
	GraphEditor->ClearSelectionSet();
	GraphEditor->SetNodeSelection(Node, true);
	
	if (bCenter && StepView == EStepView::Graph)
	{
		GraphEditor->JumpToNode(Node, false, false);
		bZoomToFitPending = false;
		FollowBlockedUntil = FPlatformTime::Seconds() + 3.0;
	}
}

void SControlFlowBPDebugger::HandleGraphSelectionChanged(const TSet<UObject*>& Selection)
{
	if (bSyncingSelection)
	{
		return;
	}
	
	TSharedPtr<FControlFlowBPStepRecord> Picked;
	for (UObject* Object : Selection)
	{
		const UControlFlowBPDebugGraphNode* Node = Cast<UControlFlowBPDebugGraphNode>(Object);
		if (Node && Node->Step.IsValid())
		{
			if (Picked.IsValid())
			{
				return;
			}
			Picked = Node->Step;
		}
	}

	if (Picked.IsValid())
	{
		SelectedStep = Picked;

		TGuardValue<bool> Guard(bSyncingSelection, true);
		SelectStepInTree(Picked);
	}
}

void SControlFlowBPDebugger::HandleGraphNodeDoubleClicked(UEdGraphNode* Node) const
{
	const UControlFlowBPDebugGraphNode* DebugNode = Cast<UControlFlowBPDebugGraphNode>(Node);
	if (!DebugNode)
	{
		return;
	}

	if (DebugNode->bPreview)
	{
		if (const UEdGraphNode* QueueNode = DebugNode->PreviewQueueNode.Get())
		{
			FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(QueueNode);
		}
	}
	else if (DebugNode->Step.IsValid())
	{
		UE::ControlFlowBP::DebuggerTab::JumpToQueueNodeOrHandler(*DebugNode->Step);
	}
	else if (FControlFlowBPEditorDebugger* EditorDebugger = UE::ControlFlowBP::DebuggerTab::Debugger(); EditorDebugger && DebugNode->Flow.IsValid())
	{
		if (!EditorDebugger->TryJumpToCallSite(DebugNode->Flow->CreateSite))
		{
			EditorDebugger->TryJumpToCallSite(DebugNode->Flow->ExecuteSite);
		}
	}
}

FActionMenuContent SControlFlowBPDebugger::MakeGraphNodeMenu(UEdGraph* Graph, const UEdGraphNode* Node, const UEdGraphPin* Pin, FMenuBuilder* MenuBuilder, bool bIsDebugging)
{
	using namespace UE::ControlFlowBP::DebuggerTab;

	if (!MenuBuilder)
	{
		return FActionMenuContent();
	}

	const UControlFlowBPDebugGraphNode* DebugNode = Cast<UControlFlowBPDebugGraphNode>(Node);
	const TSharedPtr<FControlFlowBPStepRecord> Step = DebugNode ? DebugNode->Step : nullptr;

	if (Step.IsValid())
	{
		AddStepMenuEntries(*MenuBuilder, Step, FindQueueNode(*Step));
		MenuBuilder->AddMenuSeparator();
		MenuBuilder->AddMenuEntry(
			LOCTEXT("ShowInTree", "Show in Tree"),
			LOCTEXT("ShowInTreeTooltip", "Switch to the tree, with this step selected."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateSPLambda(this, [this, Step]() -> void
			{
				SetStepView(EStepView::Tree);
				SelectStep(Step);
			})));
	}
	else if (DebugNode && DebugNode->Flow.IsValid())
	{
		AddFlowMenuEntries(*MenuBuilder, DebugNode->Flow.ToSharedRef());
		MenuBuilder->AddMenuSeparator();
	}

	MenuBuilder->AddMenuEntry(
		LOCTEXT("ZoomToFit", "Zoom to Fit"),
		LOCTEXT("ZoomToFitTooltip", "Fit the whole flow into view."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateSPLambda(this, [this]()
		{
			if (GraphEditor.IsValid())
			{
				GraphEditor->ZoomToFit(false);
			}
		})));

	return FActionMenuContent(MenuBuilder->MakeWidget());
}

#undef LOCTEXT_NAMESPACE
