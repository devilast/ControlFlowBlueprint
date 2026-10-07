#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPDebug.h"
#include "Logging/LogVerbosity.h"
#include "UObject/WeakObjectPtr.h"

class UEdGraphNode;
class UFunction;
class UToolMenu;
struct FGraphInformationPopupInfo;
struct FKismetNodeInfoContext;

class FControlFlowBPEditorDebugger
{
public:
	static void Startup();
	static void Shutdown();
	static FControlFlowBPEditorDebugger* Get();
	UEdGraphNode* ResolveNode(const FControlFlowBPCallSite& Site);
	void GetNodePopups(const UEdGraphNode* Node, const FKismetNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups);
	bool IsBreakEnabled(const FGuid& NodeGuid) const { return BreakNodes.Contains(NodeGuid); }
	void ToggleBreak(const FGuid& NodeGuid);
	bool TryJumpToCallSite(const FControlFlowBPCallSite& Site);
	UObject* FindHandlerDefinition(const FControlFlowBPStepRecord& Step) const;
	bool TryJumpToHandler(const FControlFlowBPStepRecord& Step);
	static void DebugInBlueprintEditor(UObject* Object);
	void ShowNodeInDebugger(const UEdGraphNode* Node);
	static bool IsQueueNode(const UEdGraphNode* Node, bool bRequireHandler);
	static bool IsFlowNode(const UEdGraphNode* Node);
	static bool IsStepHandlerNode(const UEdGraphNode* Node);

private:
	FControlFlowBPEditorDebugger();
	~FControlFlowBPEditorDebugger();

	void HandleIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPCallSite& Site, const FControlFlowBPStepRecord* Step);
	void HandleStepHandlerStarting(const FControlFlowBPStepRecord& Step, bool bMatchesBreakPattern);
	void HandleEndPIE(const bool bIsSimulating);

	void RegisterMenus();
	void FillNodeContextMenu(UToolMenu* Menu);
	void RebuildIndexIfStale();
	void ClearCaches();

	void AddStepPopups(const TArray<TSharedRef<FControlFlowBPStepRecord>>& Steps, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const;
	void AddFlowPopups(const TArray<TSharedRef<FControlFlowBPFlowRecord>>& Flows, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const;
	void AddHandlerPopups(const UEdGraphNode* Node, const UObject* DebugObject, TArray<FGraphInformationPopupInfo>& OutPopups) const;
	TMap<TPair<TWeakObjectPtr<UFunction>, int32>, TWeakObjectPtr<UEdGraphNode>> NodeCache;
	TSet<FGuid> BreakNodes;

	struct FNodeIndex
	{
		TMap<const UEdGraphNode*, TArray<TSharedRef<FControlFlowBPStepRecord>>> Steps;
		TMap<const UEdGraphNode*, TArray<TSharedRef<FControlFlowBPFlowRecord>>> Flows;
		TArray<TSharedRef<FControlFlowBPStepRecord>> RunningHandlers;
	};

	FNodeIndex Index;
	uint64 IndexFrame = MAX_uint64;
	uint64 IndexChangeCount = 0;

	FDelegateHandle IssueHandle;
	FDelegateHandle HandlerStartingHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle BlueprintCompiledHandle;
	FDelegateHandle MenusStartupHandle;

	static FControlFlowBPEditorDebugger* Instance;
};
