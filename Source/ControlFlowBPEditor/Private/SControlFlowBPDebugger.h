#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPDebug.h"
#include "ControlFlowBPDebugPreview.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STreeView.h"

class FControlFlowBPDebugGraphBuilder;
class FMenuBuilder;
class ITableRow;
class SGraphEditor;
class STableViewBase;
class UEdGraph;
class UEdGraphNode;
class UEdGraphPin;
struct FActionMenuContent;

/** A flow in the debugger's list. Reused across refreshes, so selection and scrolling survive. */
struct FControlFlowBPDebuggerFlowItem
{
	explicit FControlFlowBPDebuggerFlowItem(const TSharedRef<FControlFlowBPFlowRecord>& InFlow)
		: Flow(InFlow)
	{
	}

	TSharedRef<FControlFlowBPFlowRecord> Flow;
};

struct FControlFlowBPDebuggerStepItem
{
	TSharedPtr<FControlFlowBPStepRecord> Step;
	TSharedPtr<FControlFlowBPStepRecord> GroupOwner;
	FString GroupName;

	TArray<TSharedPtr<FControlFlowBPDebuggerStepItem>> Children;
	TWeakPtr<FControlFlowBPDebuggerStepItem> ParentItem;
	bool bSubtreeRunning = false;
	bool bPreview = false;
	TWeakObjectPtr<const UEdGraphNode> PreviewNode;

	bool IsGroup() const { return !Step.IsValid(); }
};

class SControlFlowBPDebugger : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SControlFlowBPDebugger) {}
	SLATE_END_ARGS()

	static const FName TabId;

	static void RegisterTabSpawner();
	static void UnregisterTabSpawner();
	static void ShowInDebugger(const TSharedPtr<FControlFlowBPFlowRecord>& Flow, const TSharedPtr<FControlFlowBPStepRecord>& Step);

	void Construct(const FArguments& InArgs);
	virtual ~SControlFlowBPDebugger() override;

private:
	using FFlowItemPtr = TSharedPtr<FControlFlowBPDebuggerFlowItem>;
	using FStepItemPtr = TSharedPtr<FControlFlowBPDebuggerStepItem>;
	using FChildrenMap = TMap<const FControlFlowBPStepRecord*, TArray<TSharedRef<FControlFlowBPStepRecord>>>;

	EActiveTimerReturnType HandleRefreshTimer(double InCurrentTime, float InDeltaTime);
	void Refresh();
	void RefreshFlows();
	void RefreshSteps();

	FStepItemPtr BuildStepItem(const TSharedRef<FControlFlowBPStepRecord>& Step, const FStepItemPtr& ParentItem, const FChildrenMap& ChildrenOf,
		TMap<const FControlFlowBPStepRecord*, FStepItemPtr>& OutStepItems, TMap<FString, FStepItemPtr>& OutGroupItems);

	const TArray<FControlFlowBPPreviewLane>& GetPreviewLanes(const TSharedRef<FControlFlowBPStepRecord>& Step);
	void AddPreviewItems(const FStepItemPtr& OwnerItem, const TSharedRef<FControlFlowBPStepRecord>& Owner, const TArray<FControlFlowBPPreviewLane>& Lanes,
		TMap<const FControlFlowBPStepRecord*, FStepItemPtr>& OutStepItems, TMap<FString, FStepItemPtr>& OutGroupItems);

	bool IsPreview(const FControlFlowBPStepRecord& Step) const;
	const UEdGraphNode* FindQueueNode(const FControlFlowBPStepRecord& Step) const;

	void FollowRunningSteps(const TArray<FStepItemPtr>& Items);

	void SelectFlow(const TSharedPtr<FControlFlowBPFlowRecord>& Flow);
	void SelectStep(const TSharedPtr<FControlFlowBPStepRecord>& Step);
	void SyncFlowListSelection();

	TSharedRef<ITableRow> GenerateFlowRow(FFlowItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable) const;
	void HandleFlowSelectionChanged(FFlowItemPtr Item, ESelectInfo::Type SelectInfo);
	void HandleFlowDoubleClick(FFlowItemPtr Item) const;
	TSharedPtr<SWidget> MakeFlowContextMenu() const;

	TSharedRef<ITableRow> GenerateStepRow(FStepItemPtr Item, const TSharedRef<STableViewBase>& OwnerTable) const;
	void GetStepChildren(FStepItemPtr Item, TArray<FStepItemPtr>& OutChildren) const;
	void HandleStepSelectionChanged(FStepItemPtr Item, ESelectInfo::Type SelectInfo);
	void HandleStepDoubleClick(FStepItemPtr Item);
	TSharedPtr<SWidget> MakeStepContextMenu();
	void SelectStepInTree(const TSharedPtr<FControlFlowBPStepRecord>& Step);

	enum class EStepView : uint8
	{
		Graph,
		Tree
	};

	TSharedRef<SWidget> MakeStepGraph();
	void SetStepView(EStepView NewView);
	void UpdateGraph();
	void FollowRunningStepInGraph();
	void SelectStepInGraph(const TSharedPtr<FControlFlowBPStepRecord>& Step, bool bCenter);

	void HandleGraphSelectionChanged(const TSet<UObject*>& Selection);
	void HandleGraphNodeDoubleClicked(UEdGraphNode* Node) const;
	FActionMenuContent MakeGraphNodeMenu(UEdGraph* Graph, const UEdGraphNode* Node, const UEdGraphPin* Pin, FMenuBuilder* MenuBuilder, bool bIsDebugging);

	TSharedRef<SWidget> MakeToolbar();
	TSharedRef<SWidget> MakeFlowHeader();
	TSharedRef<SWidget> MakeViewBar();
	TSharedRef<SWidget> MakeStepDetails();

	FText GetFlowCounts() const;
	FText GetStepDetailsText() const;
	FString GetFlowVariablesText() const;

	TArray<FFlowItemPtr> FlowItems;
	TMap<int32, FFlowItemPtr> FlowItemsById;
	TSharedPtr<SListView<FFlowItemPtr>> FlowList;

	TArray<FStepItemPtr> RootStepItems;
	TMap<const FControlFlowBPStepRecord*, FStepItemPtr> StepItemsByRecord;
	TMap<FString, FStepItemPtr> GroupItemsByKey;

	/**
	 * The previews of the selected flow's pending steps. Kept, so their made-up records - and the
	 * graph's nodes for them - stay the same from one refresh to the next.
	 */
	struct FPreviewEntry
	{
		TWeakPtr<FControlFlowBPStepRecord> Step;
		TArray<FControlFlowBPPreviewLane> Lanes;
	};
	TMap<const FControlFlowBPStepRecord*, FPreviewEntry> Previews;
	TSet<FStepItemPtr> AutoExpandedItems;
	TSharedPtr<STreeView<FStepItemPtr>> StepTree;
	TStrongObjectPtr<UEdGraph> StepGraph;
	TSharedPtr<FControlFlowBPDebugGraphBuilder> GraphBuilder;
	TSharedPtr<SGraphEditor> GraphEditor;
	EStepView StepView = EStepView::Graph;
	bool bZoomToFitPending = false;
	double FollowBlockedUntil = 0.0;
	TWeakPtr<FControlFlowBPStepRecord> LastFollowedStep;
	bool bSyncingSelection = false;
	TSharedPtr<FControlFlowBPFlowRecord> SelectedFlow;
	TSharedPtr<FControlFlowBPStepRecord> SelectedStep;

	FString FlowFilter;
	bool bShowFinished = true;
	bool bFollowRunning = true;

	TArray<TSharedPtr<FString>> TraceLevelOptions;

	static TWeakPtr<SControlFlowBPDebugger> Instance;
};
