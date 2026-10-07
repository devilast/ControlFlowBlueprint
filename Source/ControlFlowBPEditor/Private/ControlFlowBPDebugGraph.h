#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPDebug.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphSchema.h"
#include "ControlFlowBPDebugGraph.generated.h"

class UEdGraph;
class UEdGraphNode_Comment;
struct FControlFlowBPDebuggerStepItem;

enum class EControlFlowBPDebugNodeKind : uint8
{
	Start,
	Step,
	End
};

/** An output pin of a step node: one per flow the step runs, plus where execution carries on. */
struct FControlFlowBPDebugNodeOutput
{
	FName PinName;
	FText Label;
};

/**
 * Schema of the Control Flow Debugger's graph. Read-only: the graph is a picture of a running flow,
 * so nothing can be connected, and the wires are coloured by what has run (see
 * FControlFlowBPDebugConnectionPolicy).
 */
UCLASS()
class UControlFlowBPDebugGraphSchema : public UEdGraphSchema
{
	GENERATED_BODY()

public:
	static const FName PC_Flow;

	virtual FLinearColor GetPinTypeColor(const FEdGraphPinType& PinType) const override;
	virtual const FPinConnectionResponse CanCreateConnection(const UEdGraphPin* A, const UEdGraphPin* B) const override;
	virtual class FConnectionDrawingPolicy* CreateConnectionDrawingPolicy(int32 InBackLayerID, int32 InFrontLayerID, float InZoomFactor,
		const FSlateRect& InClippingRect, class FSlateWindowElementList& InDrawElements, class UEdGraph* InGraphObj) const override;

	virtual bool ShouldAlwaysPurgeOnModification() const override { return false; }
};

/**
 * A step in the Control Flow Debugger's graph, or the flow's start or end. Reads its record live,
 * so its colour, status line and wires follow the flow without the graph being rebuilt.
 */
UCLASS()
class UControlFlowBPDebugGraphNode : public UEdGraphNode
{
	GENERATED_BODY()

public:
	static const FName PN_In;
	static const FName PN_Then;
	static const FName PN_Completed;
	static constexpr float ContentWidth = 216.f;
	static constexpr int32 MaxPinLabelLength = 20;

	EControlFlowBPDebugNodeKind Kind = EControlFlowBPDebugNodeKind::Step;
	TSharedPtr<FControlFlowBPStepRecord> Step;
	TSharedPtr<FControlFlowBPFlowRecord> Flow;
	int32 NumHiddenIterations = 0;
	bool bPreview = false;
	TWeakObjectPtr<const UEdGraphNode> PreviewQueueNode;
	bool SetOutputs(const TArray<FControlFlowBPDebugNodeOutput>& Outputs);

	UEdGraphPin* GetInputPin() const;
	UEdGraphPin* GetContinuationPin() const;

	EControlFlowBPStepState GetDisplayState() const;
	FLinearColor GetStateColor() const;
	FText GetStatusText() const;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FLinearColor GetNodeBodyTintColor() const override;
	virtual FText GetTooltipText() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual bool ShowPaletteIconOnNode() const override { return true; }
	virtual FText GetPinDisplayName(const UEdGraphPin* Pin) const override;
	virtual TSharedPtr<SGraphNode> CreateVisualWidget() override;
	virtual bool CanUserDeleteNode() const override { return false; }
	virtual bool CanDuplicateNode() const override { return false; }
	virtual bool SupportsCommentBubble() const override { return false; }

private:
	TMap<FName, FText> OutputLabels;
};

/**
 * Lays a flow's step tree out as a Blueprint-style graph, left to right in the order things run, with
 * the flows a step runs in comment boxes below it:
 *
 *   [Encounter] -> [PlayIntro] -> [Waves]  Completed ------------------------> [Outro] -> [End]
 *                                          Iteration 2 -> [Spawn] -> [Fight]
 *                                          Iteration 3 -> [Spawn] -> [Fight]
 *
 * The graph is only rebuilt when its shape changes - a step queued, an iteration begun, the flow
 * ending. States, times and colours are read live by the nodes and wires.
 */
class FControlFlowBPDebugGraphBuilder
{
public:
	explicit FControlFlowBPDebugGraphBuilder(UEdGraph* InGraph);
	bool Update(const TSharedPtr<FControlFlowBPFlowRecord>& InFlow, const TArray<TSharedPtr<FControlFlowBPDebuggerStepItem>>& RootItems,
		TArray<UEdGraphNode*>& OutRepinnedNodes);

	void RefreshLiveState();

	UControlFlowBPDebugGraphNode* FindNode(const FControlFlowBPStepRecord* Step) const;
	static constexpr int32 MaxIterationsShown = 5;

private:
	using FStepItemPtr = TSharedPtr<FControlFlowBPDebuggerStepItem>;

	/** A flow run by a step: its sub-flow, one case, one parallel track, one loop iteration. */
	struct FChildLane
	{
		FName PinName;
		FText PinLabel;
		FText Label;
		TArray<FStepItemPtr> Steps;
		bool bPreview = false;
	};

	struct FLaneResult
	{
		int32 RowsUsed = 1;
		int32 NextCol = 0;
		UEdGraphPin* LastOut = nullptr;
		int32 NestLevels = 0;
	};

	/** A comment box around a child lane, in grid cells until the rows and columns are measured. */
	struct FLaneBox
	{
		FText Label;
		TArray<FStepItemPtr> Steps;
		int32 ColStart = 0;
		int32 ColEnd = 0;
		int32 RowStart = 0;
		int32 RowEnd = 0;
		int32 Depth = 0;
		int32 NestLevels = 0;
		bool bPreview = false;
	};

	void Clear();
	void Rebuild(const TArray<FStepItemPtr>& RootItems);
	FLaneResult LayoutLane(const TArray<FStepItemPtr>& Steps, int32 StartCol, int32 Row, UEdGraphPin* EnteringPin, int32 Depth,
		TSet<const FControlFlowBPStepRecord*>& OutShown, int32& InOutMaxCol);
	void PlaceNodesAndBoxes();

	void CollectChildLanes(const FControlFlowBPDebuggerStepItem& Item, TArray<FChildLane>& OutLanes, int32& OutNumHidden) const;
	uint32 HashLane(const TArray<FStepItemPtr>& Steps) const;

	UControlFlowBPDebugGraphNode* AddNode(EControlFlowBPDebugNodeKind Kind);
	static void Link(UEdGraphPin* From, UEdGraphPin* To);

	TWeakObjectPtr<UEdGraph> Graph;
	TSharedPtr<FControlFlowBPFlowRecord> Flow;
	TMap<const FControlFlowBPStepRecord*, UControlFlowBPDebugGraphNode*> StepNodes;
	UControlFlowBPDebugGraphNode* StartNode = nullptr;
	UControlFlowBPDebugGraphNode* EndNode = nullptr;

	struct FComment
	{
		UEdGraphNode_Comment* Node = nullptr;
		TArray<FStepItemPtr> Steps;
		bool bPreview = false;
	};
	TArray<FComment> Comments;

	uint32 Signature = 0;

	TMap<UControlFlowBPDebugGraphNode*, FIntPoint> Cells;
	TArray<FLaneBox> LaneBoxes;
	TArray<UEdGraphNode*> RepinnedNodes;
};
