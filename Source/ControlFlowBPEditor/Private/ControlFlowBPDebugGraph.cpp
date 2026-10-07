#include "ControlFlowBPDebugGraph.h"

#include "ControlFlowBPDebuggerShared.h"
#include "SControlFlowBPDebugger.h"

#include "ConnectionDrawingPolicy.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphNode_Comment.h"
#include "KismetPins/SGraphPinExec.h"
#include "SGraphNode.h"
#include "Styling/AppStyle.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ControlFlowBPDebugGraph"

const FName UControlFlowBPDebugGraphSchema::PC_Flow(TEXT("ControlFlowExec"));
const FName UControlFlowBPDebugGraphNode::PN_In(TEXT("In"));
const FName UControlFlowBPDebugGraphNode::PN_Then(TEXT("Then"));
const FName UControlFlowBPDebugGraphNode::PN_Completed(TEXT("Completed"));

namespace UE::ControlFlowBP::DebugGraph
{
	static constexpr float NodeWidth = UControlFlowBPDebugGraphNode::ContentWidth + 32.f;
	static constexpr float ColumnPitch = NodeWidth + 90.f;
	static constexpr float RowGap = 84.f;
	static constexpr float BoxPadX = 26.f;
	static constexpr float BoxTitleSpace = 46.f;
	static constexpr float BoxPadBottom = 22.f;
	static constexpr float BoxNestGrow = 12.f;
	static constexpr float TitleMaxWidth = UControlFlowBPDebugGraphNode::ContentWidth - 36.f;

	static constexpr float TitleHeight = 42.f;
	static constexpr float PinRowHeight = 32.f;
	static constexpr float StatusLineHeight = 18.f;
	static constexpr float StatusPadding = 14.f;
	static constexpr float StatusCharWidth = 6.f;

	static int32 NumOutputPins(const UEdGraphNode& Node)
	{
		return Node.Pins.FilterByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Output; }).Num();
	}

	static int32 CountStatusLines(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);

		int32 Count = 0;
		for (const FString& Line : Lines)
		{
			Count += FMath::Max(1, FMath::CeilToInt(Line.Len() * StatusCharWidth / UControlFlowBPDebugGraphNode::ContentWidth));
		}
		return FMath::Max(1, Count);
	}

	static float EstimateNodeHeight(const UControlFlowBPDebugGraphNode& Node)
	{
		int32 StatusLines = CountStatusLines(Node.GetStatusText().ToString());
		if (Node.Kind == EControlFlowBPDebugNodeKind::Step)
		{
			StatusLines = FMath::Max(StatusLines, 2);
		}

		return TitleHeight + PinRowHeight * FMath::Max(1, NumOutputPins(Node)) + StatusLineHeight * StatusLines + StatusPadding;
	}

	static FText ShortenPinLabel(const FText& Label)
	{
		const FString& String = Label.ToString();
		return String.Len() > UControlFlowBPDebugGraphNode::MaxPinLabelLength
			? FText::FromString(String.Left(UControlFlowBPDebugGraphNode::MaxPinLabelLength - 1) + TEXT("…"))
			: Label;
	}

	static FString Capitalize(FString Text)
	{
		if (!Text.IsEmpty())
		{
			Text[0] = FChar::ToUpper(Text[0]);
		}
		return Text;
	}

	static FLinearColor TitleColor(EControlFlowBPStepState State, EControlFlowBPStepType Type)
	{
		switch (State)
		{
		case EControlFlowBPStepState::Running:
			return (Type == EControlFlowBPStepType::Wait || Type == EControlFlowBPStepType::Delay)
				? FLinearColor(0.04f, 0.26f, 0.55f)
				: FLinearColor(0.06f, 0.42f, 0.08f);
		case EControlFlowBPStepState::Succeeded: return FLinearColor(0.22f, 0.3f, 0.22f);
		case EControlFlowBPStepState::Failed:    return FLinearColor(0.6f, 0.06f, 0.04f);
		case EControlFlowBPStepState::Cancelled: return FLinearColor(0.6f, 0.33f, 0.02f);
		case EControlFlowBPStepState::Skipped:   return FLinearColor(0.09f, 0.09f, 0.09f);
		default:                                 return FLinearColor(0.17f, 0.17f, 0.17f);
		}
	}

	static const FLinearColor PreviewBoxColor(0.2f, 0.2f, 0.2f, 0.3f);

	static FLinearColor BoxColor(EControlFlowBPStepState State)
	{
		switch (State)
		{
		case EControlFlowBPStepState::Running:   return FLinearColor(0.12f, 0.4f, 0.16f, 0.6f);
		case EControlFlowBPStepState::Failed:    return FLinearColor(0.45f, 0.1f, 0.08f, 0.6f);
		case EControlFlowBPStepState::Cancelled: return FLinearColor(0.45f, 0.28f, 0.05f, 0.6f);
		case EControlFlowBPStepState::Skipped:   return FLinearColor(0.08f, 0.08f, 0.08f, 0.5f);
		default:                                 return FLinearColor(0.25f, 0.25f, 0.25f, 0.6f);
		}
	}

	static EControlFlowBPStepState LaneState(const TArray<TSharedPtr<FControlFlowBPDebuggerStepItem>>& Steps)
	{
		FControlFlowBPDebuggerStepItem Lane;
		Lane.Children = Steps;
		return UE::ControlFlowBP::DebuggerTab::GetItemState(Lane);
	}

	static const TCHAR* TypeIcon(EControlFlowBPStepType Type)
	{
		switch (Type)
		{
		case EControlFlowBPStepType::Wait:
		case EControlFlowBPStepType::Delay:   return TEXT("GraphEditor.Timeline_16x");
		case EControlFlowBPStepType::SubFlow: return TEXT("GraphEditor.SubGraph_16x");
		case EControlFlowBPStepType::Branch:  return TEXT("GraphEditor.Switch_16x");
		case EControlFlowBPStepType::If:      return TEXT("GraphEditor.Branch_16x");
		case EControlFlowBPStepType::Fork:    return TEXT("GraphEditor.Sequence_16x");
		case EControlFlowBPStepType::Race:    return TEXT("GraphEditor.Macro.Gate_16x");
		case EControlFlowBPStepType::Loop:    return TEXT("GraphEditor.Macro.Loop_16x");
		default:                              return TEXT("GraphEditor.Function_16x");
		}
	}
}

/**
 * Colours each wire by the step it leads into: bright where execution has been, animated - like a
 * Blueprint's wires while it runs - into whatever is running now, dim ahead of it.
 */
class FControlFlowBPDebugConnectionPolicy : public FConnectionDrawingPolicy
{
public:
	FControlFlowBPDebugConnectionPolicy(int32 InBackLayerID, int32 InFrontLayerID, float InZoomFactor, const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements)
		: FConnectionDrawingPolicy(InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements)
	{
	}

	virtual void DetermineWiringStyle(UEdGraphPin* OutputPin, UEdGraphPin* InputPin, FConnectionParams& Params) override
	{
		FConnectionDrawingPolicy::DetermineWiringStyle(OutputPin, InputPin, Params);

		const UControlFlowBPDebugGraphNode* Target = InputPin ? Cast<UControlFlowBPDebugGraphNode>(InputPin->GetOwningNodeUnchecked()) : nullptr;
		if (!Target)
		{
			return;
		}

		switch (Target->GetDisplayState())
		{
		case EControlFlowBPStepState::Running:
			Params.WireColor = Target->GetStateColor();
			Params.WireThickness = 3.f;
			Params.bDrawBubbles = true;
			break;

		case EControlFlowBPStepState::Succeeded:
		case EControlFlowBPStepState::Failed:
		case EControlFlowBPStepState::Cancelled:
			Params.WireColor = FLinearColor(0.85f, 0.85f, 0.85f);
			Params.WireThickness = 2.f;
			break;

		case EControlFlowBPStepState::Skipped:
			Params.WireColor = FLinearColor(0.2f, 0.2f, 0.2f, 0.6f);
			Params.WireThickness = 1.f;
			break;

		default:
			Params.WireColor = FLinearColor(0.38f, 0.38f, 0.38f);
			Params.WireThickness = 1.5f;
			break;
		}

		if (HoveredPins.Num() > 0)
		{
			ApplyHoverDeemphasis(OutputPin, InputPin, Params.WireThickness, Params.WireColor);
		}
	}
};

FLinearColor UControlFlowBPDebugGraphSchema::GetPinTypeColor(const FEdGraphPinType& PinType) const
{
	return FLinearColor::White;
}

const FPinConnectionResponse UControlFlowBPDebugGraphSchema::CanCreateConnection(const UEdGraphPin* A, const UEdGraphPin* B) const
{
	return FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, LOCTEXT("ReadOnly", "This graph shows a running flow; it cannot be edited."));
}

FConnectionDrawingPolicy* UControlFlowBPDebugGraphSchema::CreateConnectionDrawingPolicy(int32 InBackLayerID, int32 InFrontLayerID, float InZoomFactor,
	const FSlateRect& InClippingRect, FSlateWindowElementList& InDrawElements, UEdGraph* InGraphObj) const
{
	return new FControlFlowBPDebugConnectionPolicy(InBackLayerID, InFrontLayerID, InZoomFactor, InClippingRect, InDrawElements);
}

/** The standard node, with exec-arrow pins and a live status line under them, always ContentWidth wide. */
class SControlFlowBPDebugGraphNode : public SGraphNode
{
public:
	SLATE_BEGIN_ARGS(SControlFlowBPDebugGraphNode) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UControlFlowBPDebugGraphNode* InNode)
	{
		GraphNode = InNode;
		DebugNode = InNode;
		SetCursor(EMouseCursor::CardinalCross);
		UpdateGraphNode();
	}

protected:
	virtual TSharedPtr<SGraphPin> CreatePinWidget(UEdGraphPin* Pin) const override
	{
		return SNew(SGraphPinExec, Pin);
	}

	virtual TSharedRef<SWidget> CreateTitleWidget(TSharedPtr<SNodeTitle> NodeTitle) override
	{
		return SNew(SBox)
			.MaxDesiredWidth(UE::ControlFlowBP::DebugGraph::TitleMaxWidth)
			[
				SGraphNode::CreateTitleWidget(NodeTitle)
			];
	}

	virtual TOptional<ETextOverflowPolicy> GetNameOverflowPolicy() const override
	{
		return ETextOverflowPolicy::Ellipsis;
	}

	virtual void CreateBelowPinControls(TSharedPtr<SVerticalBox> MainBox) override
	{
		const TWeakObjectPtr<UControlFlowBPDebugGraphNode> WeakNode(DebugNode);

		MainBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(12.f, 4.f, 12.f, 8.f))
		[
			SNew(SBox)
			.WidthOverride(UControlFlowBPDebugGraphNode::ContentWidth)
			[
				SNew(STextBlock)
				.WrapTextAt(UControlFlowBPDebugGraphNode::ContentWidth)
				.WrappingPolicy(ETextWrappingPolicy::AllowPerCharacterWrapping)
				.Text_Lambda([WeakNode]() { return WeakNode.IsValid() ? WeakNode->GetStatusText() : FText::GetEmpty(); })
				.ColorAndOpacity_Lambda([WeakNode]() { return FSlateColor(WeakNode.IsValid() ? WeakNode->GetStateColor() : FLinearColor::White); })
			]
		];
	}

private:
	UControlFlowBPDebugGraphNode* DebugNode = nullptr;
};

void UControlFlowBPDebugGraphNode::AllocateDefaultPins()
{
	if (Kind != EControlFlowBPDebugNodeKind::Start)
	{
		CreatePin(EGPD_Input, UControlFlowBPDebugGraphSchema::PC_Flow, PN_In);
	}

	if (Kind != EControlFlowBPDebugNodeKind::End)
	{
		CreatePin(EGPD_Output, UControlFlowBPDebugGraphSchema::PC_Flow, PN_Then);
	}
}

bool UControlFlowBPDebugGraphNode::SetOutputs(const TArray<FControlFlowBPDebugNodeOutput>& Outputs)
{
	OutputLabels.Reset();
	for (const FControlFlowBPDebugNodeOutput& Output : Outputs)
	{
		OutputLabels.Add(Output.PinName, UE::ControlFlowBP::DebugGraph::ShortenPinLabel(Output.Label));
	}

	TArray<UEdGraphPin*> Current = Pins.FilterByPredicate([](const UEdGraphPin* Pin) { return Pin->Direction == EGPD_Output; });

	bool bSame = Current.Num() == Outputs.Num();
	for (int32 Index = 0; bSame && Index < Outputs.Num(); ++Index)
	{
		bSame = Current[Index]->PinName == Outputs[Index].PinName;
	}

	if (bSame)
	{
		return false;
	}

	for (UEdGraphPin* Pin : Current)
	{
		RemovePin(Pin);
	}

	for (const FControlFlowBPDebugNodeOutput& Output : Outputs)
	{
		CreatePin(EGPD_Output, UControlFlowBPDebugGraphSchema::PC_Flow, Output.PinName);
	}

	return true;
}

UEdGraphPin* UControlFlowBPDebugGraphNode::GetInputPin() const
{
	return FindPin(PN_In, EGPD_Input);
}

UEdGraphPin* UControlFlowBPDebugGraphNode::GetContinuationPin() const
{
	UEdGraphPin* Pin = FindPin(PN_Then, EGPD_Output);
	return Pin ? Pin : FindPin(PN_Completed, EGPD_Output);
}

EControlFlowBPStepState UControlFlowBPDebugGraphNode::GetDisplayState() const
{
	switch (Kind)
	{
	case EControlFlowBPDebugNodeKind::Step:
		return Step.IsValid() ? Step->State : EControlFlowBPStepState::Pending;

	case EControlFlowBPDebugNodeKind::Start:
		if (!Flow.IsValid() || Flow->State == EControlFlowBPFlowState::Building)
		{
			return EControlFlowBPStepState::Pending;
		}
		if (Flow->State == EControlFlowBPFlowState::Running)
		{
			return EControlFlowBPStepState::Running;
		}
		return Flow->State == EControlFlowBPFlowState::Cancelled ? EControlFlowBPStepState::Cancelled : EControlFlowBPStepState::Succeeded;

	default:
		if (!Flow.IsValid() || !Flow->IsFinished())
		{
			return EControlFlowBPStepState::Pending;
		}
		return Flow->State == EControlFlowBPFlowState::Cancelled ? EControlFlowBPStepState::Cancelled : EControlFlowBPStepState::Succeeded;
	}
}

FLinearColor UControlFlowBPDebugGraphNode::GetStateColor() const
{
	return UE::ControlFlowBP::DebuggerTab::StateColor(GetDisplayState(), Step.IsValid() ? Step->Type : EControlFlowBPStepType::Function);
}

FText UControlFlowBPDebugGraphNode::GetStatusText() const
{
	using namespace UE::ControlFlowBP::DebugGraph;

	switch (Kind)
	{
	case EControlFlowBPDebugNodeKind::Start:
	case EControlFlowBPDebugNodeKind::End:
		return Flow.IsValid() ? FText::FromString(Capitalize(Flow->DescribeState())) : FText::GetEmpty();

	default:
		{
			if (!Step.IsValid())
			{
				return FText::GetEmpty();
			}

			if (bPreview)
			{
				return FText::FromString(Capitalize(Step->Info));
			}

			FString Text = Capitalize(Step->DescribeState());
			if (NumHiddenIterations > 0)
			{
				Text += FString::Printf(TEXT("\n%d earlier iteration(s) not drawn"), NumHiddenIterations);
			}
			return FText::FromString(Text);
		}
	}
}

FText UControlFlowBPDebugGraphNode::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	switch (Kind)
	{
	case EControlFlowBPDebugNodeKind::Start:
		return Flow.IsValid()
			? FText::Format(LOCTEXT("StartTitle", "{0}\nControl Flow #{1}"), FText::FromString(Flow->Name), Flow->Id)
			: LOCTEXT("StartTitleNoFlow", "Control Flow");

	case EControlFlowBPDebugNodeKind::End:
		return (Flow.IsValid() && Flow->State == EControlFlowBPFlowState::Cancelled)
			? LOCTEXT("EndCancelled", "Cancelled\nEnd of flow")
			: LOCTEXT("EndCompleted", "Completed\nEnd of flow");

	default:
		return Step.IsValid()
			? FText::FromString(FString::Printf(TEXT("%s\n%s"), *Step->Name, FControlFlowBPStepRecord::LexType(Step->Type)))
			: FText::GetEmpty();
	}
}

FLinearColor UControlFlowBPDebugGraphNode::GetNodeTitleColor() const
{
	if (Kind == EControlFlowBPDebugNodeKind::Start)
	{
		return FLinearColor(0.55f, 0.07f, 0.07f);
	}

	return UE::ControlFlowBP::DebugGraph::TitleColor(GetDisplayState(), Step.IsValid() ? Step->Type : EControlFlowBPStepType::Function);
}

FLinearColor UControlFlowBPDebugGraphNode::GetNodeBodyTintColor() const
{
	if (bPreview)
	{
		return FLinearColor(1.f, 1.f, 1.f, 0.3f);
	}

	switch (GetDisplayState())
	{
	case EControlFlowBPStepState::Pending: return FLinearColor(1.f, 1.f, 1.f, 0.6f);
	case EControlFlowBPStepState::Skipped: return FLinearColor(1.f, 1.f, 1.f, 0.35f);
	default:                               return FLinearColor::White;
	}
}

FText UControlFlowBPDebugGraphNode::GetTooltipText() const
{
	if (Kind != EControlFlowBPDebugNodeKind::Step)
	{
		if (!Flow.IsValid())
		{
			return FText::GetEmpty();
		}

		FString Text = FString::Printf(TEXT("%s (#%d) - %s\nOwner: %s"), *Flow->Name, Flow->Id, *Flow->DescribeState(), *Flow->OwnerName);
		if (Flow->CreateSite.IsSet())
		{
			Text += FString::Printf(TEXT("\nCreated at %s - double-click to go there"), *Flow->CreateSite.Describe());
		}
		return FText::FromString(Text);
	}

	if (!Step.IsValid())
	{
		return FText::GetEmpty();
	}

	if (bPreview)
	{
		return FText::Format(LOCTEXT("PreviewTooltip",
			"{0}\n{1} - {2}\nRead from the Blueprint, so the step that runs may differ. Double-click to go to its Queue node."),
			FText::FromString(Step->Path), FText::FromString(FControlFlowBPStepRecord::LexType(Step->Type)), FText::FromString(Step->Info));
	}

	FString Text = FString::Printf(TEXT("%s\n%s - %s"), *Step->Path, FControlFlowBPStepRecord::LexType(Step->Type), *Step->DescribeState());
	if (!Step->Info.IsEmpty())
	{
		Text += FString::Printf(TEXT("\nQueued as: %s"), *Step->Info);
	}
	if (Step->QueueSite.IsSet())
	{
		Text += FString::Printf(TEXT("\nQueued at %s - double-click to go there"), *Step->QueueSite.Describe());
	}
	return FText::FromString(Text);
}

FSlateIcon UControlFlowBPDebugGraphNode::GetIconAndTint(FLinearColor& OutColor) const
{
	OutColor = FLinearColor::White;

	const TCHAR* Icon = Kind == EControlFlowBPDebugNodeKind::Step && Step.IsValid()
		? UE::ControlFlowBP::DebugGraph::TypeIcon(Step->Type)
		: TEXT("GraphEditor.Event_16x");

	return FSlateIcon(FAppStyle::GetAppStyleSetName(), Icon);
}

FText UControlFlowBPDebugGraphNode::GetPinDisplayName(const UEdGraphPin* Pin) const
{
	if (!Pin || Pin->PinName == PN_In || Pin->PinName == PN_Then)
	{
		return FText::GetEmpty();
	}

	const FText* Label = OutputLabels.Find(Pin->PinName);
	return Label ? *Label : FText::FromName(Pin->PinName);
}

TSharedPtr<SGraphNode> UControlFlowBPDebugGraphNode::CreateVisualWidget()
{
	return SNew(SControlFlowBPDebugGraphNode, this);
}

FControlFlowBPDebugGraphBuilder::FControlFlowBPDebugGraphBuilder(UEdGraph* InGraph)
	: Graph(InGraph)
{
}

UControlFlowBPDebugGraphNode* FControlFlowBPDebugGraphBuilder::FindNode(const FControlFlowBPStepRecord* Step) const
{
	UControlFlowBPDebugGraphNode* const* Found = StepNodes.Find(Step);
	return Found ? *Found : nullptr;
}

void FControlFlowBPDebugGraphBuilder::Clear()
{
	if (UEdGraph* GraphObj = Graph.Get())
	{
		const TArray<TObjectPtr<UEdGraphNode>> AllNodes = GraphObj->Nodes;
		for (UEdGraphNode* Node : AllNodes)
		{
			GraphObj->RemoveNode(Node);
		}
	}

	StepNodes.Reset();
	StartNode = nullptr;
	EndNode = nullptr;
	Comments.Reset();
	Signature = 0;
}

bool FControlFlowBPDebugGraphBuilder::Update(const TSharedPtr<FControlFlowBPFlowRecord>& InFlow, const TArray<FStepItemPtr>& RootItems,
	TArray<UEdGraphNode*>& OutRepinnedNodes)
{
	OutRepinnedNodes.Reset();

	UEdGraph* GraphObj = Graph.Get();
	if (!GraphObj)
	{
		return false;
	}

	if (InFlow != Flow)
	{
		const bool bHadNodes = GraphObj->Nodes.Num() > 0;
		Clear();
		Flow = InFlow;

		if (!Flow.IsValid())
		{
			return bHadNodes;
		}
	}

	if (!Flow.IsValid())
	{
		return false;
	}

	uint32 NewSignature = HashCombine(GetTypeHash(Flow->IsFinished()), HashLane(RootItems));
	NewSignature = HashCombine(NewSignature, PointerHash(Flow.Get()));
	if (StartNode && NewSignature == Signature)
	{
		return false;
	}

	Signature = NewSignature;
	Rebuild(RootItems);
	OutRepinnedNodes = MoveTemp(RepinnedNodes);
	return true;
}

void FControlFlowBPDebugGraphBuilder::RefreshLiveState()
{
	using namespace UE::ControlFlowBP::DebugGraph;

	for (const FComment& Comment : Comments)
	{
		if (Comment.Node)
		{
			Comment.Node->CommentColor = Comment.bPreview ? PreviewBoxColor : BoxColor(LaneState(Comment.Steps));
		}
	}
}

void FControlFlowBPDebugGraphBuilder::CollectChildLanes(const FControlFlowBPDebuggerStepItem& Item, TArray<FChildLane>& OutLanes, int32& OutNumHidden) const
{
	OutNumHidden = 0;

	TArray<FStepItemPtr> DirectSteps;
	TArray<FStepItemPtr> Groups;
	for (const FStepItemPtr& Child : Item.Children)
	{
		(Child->IsGroup() ? Groups : DirectSteps).Add(Child);
	}

	if (DirectSteps.Num() > 0)
	{
		FChildLane& Lane = OutLanes.AddDefaulted_GetRef();
		Lane.PinName = TEXT("Lane_Steps");
		Lane.PinLabel = LOCTEXT("StepsLane", "Steps");
		Lane.bPreview = DirectSteps[0]->bPreview;
		Lane.Label = Item.Step.IsValid()
			? FText::Format(Lane.bPreview ? LOCTEXT("PreviewSubFlowLane", "Steps of '{0}' (preview)") : LOCTEXT("SubFlowLane", "Steps of '{0}'"),
				FText::FromString(Item.Step->Name))
			: Lane.PinLabel;
		Lane.Steps = DirectSteps;
	}

	int32 FirstGroup = 0;
	if (Item.Step.IsValid() && Item.Step->Type == EControlFlowBPStepType::Loop && Groups.Num() > MaxIterationsShown)
	{
		FirstGroup = Groups.Num() - MaxIterationsShown;
		OutNumHidden = FirstGroup;
	}

	const bool bLoop = Item.Step.IsValid() && Item.Step->Type == EControlFlowBPStepType::Loop;
	for (int32 Index = FirstGroup; Index < Groups.Num(); ++Index)
	{
		const FControlFlowBPDebuggerStepItem& Group = *Groups[Index];

		FChildLane& Lane = OutLanes.AddDefaulted_GetRef();
		Lane.Label = UE::ControlFlowBP::DebuggerTab::ItemLabel(Group);
		Lane.bPreview = Group.bPreview;
		Lane.Steps = Group.Children;

		if (bLoop)
		{
			Lane.PinName = FName(*FString::Printf(TEXT("Lane_Iteration_%d"), Index - FirstGroup));
			Lane.PinLabel = Group.bPreview ? LOCTEXT("EachIterationPin", "Each iteration") : Lane.Label;
		}
		else
		{
			Lane.PinName = FName(*FString::Printf(TEXT("Lane_%s"), *Group.GroupName));
			Lane.PinLabel = FText::FromString(Group.GroupName);
		}
	}
}

uint32 FControlFlowBPDebugGraphBuilder::HashLane(const TArray<FStepItemPtr>& Steps) const
{
	uint32 Hash = GetTypeHash(Steps.Num());

	for (const FStepItemPtr& Item : Steps)
	{
		Hash = HashCombine(Hash, PointerHash(Item->Step.Get()));
		Hash = HashCombine(Hash, GetTypeHash(Item->bPreview));

		TArray<FChildLane> Lanes;
		int32 NumHidden = 0;
		CollectChildLanes(*Item, Lanes, NumHidden);

		Hash = HashCombine(Hash, GetTypeHash(NumHidden));
		for (const FChildLane& Lane : Lanes)
		{
			Hash = HashCombine(Hash, GetTypeHash(Lane.PinName));
			Hash = HashCombine(Hash, HashLane(Lane.Steps));
		}
	}

	return Hash;
}

UControlFlowBPDebugGraphNode* FControlFlowBPDebugGraphBuilder::AddNode(EControlFlowBPDebugNodeKind Kind)
{
	UEdGraph* GraphObj = Graph.Get();

	UControlFlowBPDebugGraphNode* Node = NewObject<UControlFlowBPDebugGraphNode>(GraphObj, NAME_None, RF_Transient);
	Node->Kind = Kind;
	Node->Flow = Flow;
	Node->CreateNewGuid();
	Node->AllocateDefaultPins();
	GraphObj->AddNode(Node, false, false);
	return Node;
}

void FControlFlowBPDebugGraphBuilder::Link(UEdGraphPin* From, UEdGraphPin* To)
{
	if (From && To)
	{
		From->MakeLinkTo(To);
	}
}

void FControlFlowBPDebugGraphBuilder::Rebuild(const TArray<FStepItemPtr>& RootItems)
{
	UEdGraph* GraphObj = Graph.Get();

	for (const FComment& Comment : Comments)
	{
		GraphObj->RemoveNode(Comment.Node);
	}
	Comments.Reset();

	for (UEdGraphNode* Node : GraphObj->Nodes)
	{
		Node->BreakAllNodeLinks();
	}

	Cells.Reset();
	LaneBoxes.Reset();
	RepinnedNodes.Reset();

	if (!StartNode)
	{
		StartNode = AddNode(EControlFlowBPDebugNodeKind::Start);
	}
	Cells.Add(StartNode, FIntPoint(0, 0));

	TSet<const FControlFlowBPStepRecord*> Shown;
	int32 MaxCol = 0;
	const FLaneResult Root = LayoutLane(RootItems, 1, 0, StartNode->GetContinuationPin(), 0, Shown, MaxCol);

	if (Flow->IsFinished())
	{
		if (!EndNode)
		{
			EndNode = AddNode(EControlFlowBPDebugNodeKind::End);
		}
		Cells.Add(EndNode, FIntPoint(Root.NextCol, 0));
		Link(Root.LastOut, EndNode->GetInputPin());
	}
	else if (EndNode)
	{
		GraphObj->RemoveNode(EndNode);
		EndNode = nullptr;
	}

	for (auto It = StepNodes.CreateIterator(); It; ++It)
	{
		if (!Shown.Contains(It.Key()))
		{
			GraphObj->RemoveNode(It.Value());
			It.RemoveCurrent();
		}
	}

	PlaceNodesAndBoxes();
}

FControlFlowBPDebugGraphBuilder::FLaneResult FControlFlowBPDebugGraphBuilder::LayoutLane(
	const TArray<FStepItemPtr>& Steps,
	int32 StartCol,
	int32 Row,
	UEdGraphPin* EnteringPin,
	int32 Depth,
	TSet<const FControlFlowBPStepRecord*>& OutShown,
	int32& InOutMaxCol)
{
	using namespace UE::ControlFlowBP::DebugGraph;

	int32 Col = StartCol;
	int32 NextFreeRow = Row + 1;
	int32 NestLevels = 0;
	UEdGraphPin* PreviousOut = EnteringPin;

	for (const FStepItemPtr& Item : Steps)
	{
		if (!Item.IsValid() || !Item->Step.IsValid())
		{
			continue;
		}

		const FControlFlowBPStepRecord* Record = Item->Step.Get();
		UControlFlowBPDebugGraphNode* Node = StepNodes.FindRef(Record);
		if (!Node)
		{
			Node = AddNode(EControlFlowBPDebugNodeKind::Step);
			StepNodes.Add(Record, Node);
		}
		Node->Step = Item->Step;
		Node->bPreview = Item->bPreview;
		Node->PreviewQueueNode = Item->PreviewNode;
		OutShown.Add(Record);

		TArray<FChildLane> Lanes;
		CollectChildLanes(*Item, Lanes, Node->NumHiddenIterations);

		TArray<FControlFlowBPDebugNodeOutput> Outputs;
		Outputs.Add(FControlFlowBPStepRecord::RunsFlows(Item->Step->Type)
			? FControlFlowBPDebugNodeOutput{ UControlFlowBPDebugGraphNode::PN_Completed, LOCTEXT("CompletedPin", "Completed") }
			: FControlFlowBPDebugNodeOutput{ UControlFlowBPDebugGraphNode::PN_Then, FText::GetEmpty() });
		for (const FChildLane& Lane : Lanes)
		{
			Outputs.Add({ Lane.PinName, Lane.PinLabel });
		}
		if (Node->SetOutputs(Outputs))
		{
			RepinnedNodes.AddUnique(Node);
		}

		Cells.Add(Node, FIntPoint(Col, Row));
		InOutMaxCol = FMath::Max(InOutMaxCol, Col);
		Link(PreviousOut, Node->GetInputPin());

		int32 ChildrenEndCol = Col;
		for (const FChildLane& Lane : Lanes)
		{
			const int32 LaneRow = NextFreeRow;
			int32 LaneMaxCol = Col + 1;

			const FLaneResult Child = LayoutLane(Lane.Steps, Col + 1, LaneRow, Node->FindPin(Lane.PinName, EGPD_Output), Depth + 1, OutShown, LaneMaxCol);

			FLaneBox& Box = LaneBoxes.AddDefaulted_GetRef();
			Box.Label = Lane.Label;
			Box.Steps = Lane.Steps;
			Box.bPreview = Lane.bPreview;
			Box.ColStart = Col + 1;
			Box.ColEnd = LaneMaxCol;
			Box.RowStart = LaneRow;
			Box.RowEnd = LaneRow + Child.RowsUsed;
			Box.Depth = Depth;
			Box.NestLevels = Child.NestLevels;

			NextFreeRow += Child.RowsUsed;
			NestLevels = FMath::Max(NestLevels, Child.NestLevels + 1);
			ChildrenEndCol = FMath::Max(ChildrenEndCol, LaneMaxCol);
			InOutMaxCol = FMath::Max(InOutMaxCol, LaneMaxCol);
		}

		PreviousOut = Node->GetContinuationPin();
		Col = ChildrenEndCol + 1;
	}

	FLaneResult Result;
	Result.RowsUsed = NextFreeRow - Row;
	Result.NextCol = Col;
	Result.LastOut = PreviousOut;
	Result.NestLevels = NestLevels;
	return Result;
}

void FControlFlowBPDebugGraphBuilder::PlaceNodesAndBoxes()
{
	using namespace UE::ControlFlowBP::DebugGraph;

	UEdGraph* GraphObj = Graph.Get();

	int32 NumRows = 1;
	for (const TPair<UControlFlowBPDebugGraphNode*, FIntPoint>& Cell : Cells)
	{
		NumRows = FMath::Max(NumRows, Cell.Value.Y + 1);
	}

	TArray<float> RowHeight;
	RowHeight.Init(0.f, NumRows);
	for (const TPair<UControlFlowBPDebugGraphNode*, FIntPoint>& Cell : Cells)
	{
		RowHeight[Cell.Value.Y] = FMath::Max(RowHeight[Cell.Value.Y], EstimateNodeHeight(*Cell.Key));
	}

	TArray<float> RowExtra;
	RowExtra.Init(0.f, NumRows);
	const auto LastRowOf = [NumRows](const FLaneBox& Box) { return FMath::Clamp(Box.RowEnd - 1, 0, NumRows - 1); };
	for (const FLaneBox& Box : LaneBoxes)
	{
		RowExtra[LastRowOf(Box)] = FMath::Max(RowExtra[LastRowOf(Box)], Box.NestLevels * BoxNestGrow);
	}

	TArray<float> RowTop;
	RowTop.SetNum(NumRows);
	float Y = 0.f;
	for (int32 Row = 0; Row < NumRows; ++Row)
	{
		RowTop[Row] = Y;
		Y += RowHeight[Row] + RowGap + RowExtra[Row];
	}

	for (const TPair<UControlFlowBPDebugGraphNode*, FIntPoint>& Cell : Cells)
	{
		Cell.Key->NodePosX = FMath::RoundToInt(Cell.Value.X * ColumnPitch);
		Cell.Key->NodePosY = FMath::RoundToInt(RowTop[Cell.Value.Y]);
	}

	for (const FLaneBox& Box : LaneBoxes)
	{
		UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(GraphObj, NAME_None, RF_Transient);
		Comment->CreateNewGuid();
		Comment->NodeComment = Box.Label.ToString();
		Comment->CommentColor = Box.bPreview ? PreviewBoxColor : BoxColor(LaneState(Box.Steps));
		Comment->FontSize = 14;
		Comment->CommentDepth = -1000 + Box.Depth;
		Comment->bCommentBubbleVisible = false;
		Comment->bCommentBubbleVisible_InDetailsPanel = false;

		const int32 RowStart = FMath::Clamp(Box.RowStart, 0, NumRows - 1);
		const int32 RowLast = FMath::Max(LastRowOf(Box), RowStart);

		const float Grow = Box.NestLevels * BoxNestGrow;
		const float Left = Box.ColStart * ColumnPitch - BoxPadX;
		const float Top = RowTop[RowStart] - BoxTitleSpace;
		const float Right = FMath::Max(Box.ColEnd, Box.ColStart) * ColumnPitch + NodeWidth + BoxPadX + Grow;
		const float Bottom = RowTop[RowLast] + RowHeight[RowLast] + BoxPadBottom + Grow;

		Comment->NodePosX = FMath::RoundToInt(Left);
		Comment->NodePosY = FMath::RoundToInt(Top);
		Comment->NodeWidth = FMath::RoundToInt(Right - Left);
		Comment->NodeHeight = FMath::RoundToInt(Bottom - Top);

		GraphObj->AddNode(Comment, false, false);
		Comments.Add({ Comment, Box.Steps, Box.bPreview });
	}
}

#undef LOCTEXT_NAMESPACE
