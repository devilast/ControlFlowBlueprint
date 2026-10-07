#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPClock.h"
#include "Logging/LogVerbosity.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class AHUD;
class FDebugDisplayInfo;
class UCanvas;
class UControlFlowBP;
class UFunction;
class UObject;
class UWorld;

struct FControlFlowBPFlowRecord;
struct FControlFlowBPLane;

/**
 * Where a Blueprint called into this module: the Blueprint function on the script stack and the
 * bytecode offset of the call. The editor maps it back to the graph node with
 * FKismetDebugUtilities::FindSourceNodeForCodeLocation. Captured only with DO_BLUEPRINT_GUARD (the
 * editor and non-Shipping/Test builds); unset everywhere else.
 */
struct CONTROLFLOWBP_API FControlFlowBPCallSite
{
	TWeakObjectPtr<UObject> Context;
	TWeakObjectPtr<UFunction> Function;
	int32 CodeOffset = INDEX_NONE;

	bool IsSet() const { return CodeOffset != INDEX_NONE; }
	static FControlFlowBPCallSite Capture();
	FString Describe() const;
};

enum class EControlFlowBPStepType : uint8
{
	Function,
	Wait,
	Delay,
	SubFlow,
	Branch,
	Fork,
	Loop,
	If,
	Race,
	Setting,
	Internal
};

enum class EControlFlowBPStepState : uint8
{
	Pending,
	Running,
	Succeeded,
	Failed,

	Cancelled,
	Skipped
};

enum class EControlFlowBPFlowState : uint8
{
	Building,
	Running,
	Completed,
	Cancelled
};

/** Why a flow was cancelled. Decides how loudly the cancellation is reported. */
enum class EControlFlowBPCancelCause : uint8
{
	None,
	Requested,
	Aborted,

	OwnerDestroyed,
	WorldTornDown,
	NeverExecuted
};

struct CONTROLFLOWBP_API FControlFlowBPStepRecord
{
	FString Name;
	FString Path;

	EControlFlowBPStepType Type = EControlFlowBPStepType::Function;
	EControlFlowBPStepState State = EControlFlowBPStepState::Pending;
	int32 Depth = 0;
	FString Info;
	FString Detail;
	double QueuedTime = 0.0;
	double StartTime = 0.0;
	double EndTime = 0.0;

	FControlFlowBPCallSite QueueSite;
	TWeakObjectPtr<UObject> HandlerObject;
	FName HandlerFunction;
	float TimeoutSeconds = 0.f;
	int32 Iterations = 0;
	TArray<TSharedRef<FControlFlowBPLane>> ChildLanes;

	TWeakPtr<FControlFlowBPFlowRecord> Flow;
	TWeakPtr<FControlFlowBPStepRecord> Parent;
	FString Group;
	int32 Sequence = 0;
	uint64 RegionId = 0;

	bool bFailed = false;
	bool bHandlerMissing = false;
	bool bCancelRequested = false;
	bool bChildCancelled = false;

	bool IsInternal() const { return Type == EControlFlowBPStepType::Internal; }
	bool IsFinished() const { return State >= EControlFlowBPStepState::Succeeded; }
	double GetElapsed() const;
	FString DescribeState() const;

	static const TCHAR* LexType(EControlFlowBPStepType InType);
	static const TCHAR* LexState(EControlFlowBPStepState InState);
	static bool RunsFlows(EControlFlowBPStepType InType);
};

/** One FControlFlow, node for node, as this wrapper queued it. */
struct CONTROLFLOWBP_API FControlFlowBPLane
{
	FString Path;
	int32 Depth = 0;
	TWeakPtr<FControlFlowBPStepRecord> OwnerStep;
	TArray<TSharedRef<FControlFlowBPStepRecord>> Entries;
	TSharedPtr<FControlFlowBPStepRecord> GetRunningStep() const;
};

/** Everything known about one root flow and the tree of flows under it. */
struct CONTROLFLOWBP_API FControlFlowBPFlowRecord : public TSharedFromThis<FControlFlowBPFlowRecord>
{
	FControlFlowBPFlowRecord(const FString& InName, UControlFlowBP* InBuilder, UObject* InOwner, const FControlFlowBPClock& InClock);

	int32 Id = 0;
	FString Name;
	TWeakObjectPtr<UControlFlowBP> Builder;
	TWeakObjectPtr<UObject> Owner;
	FString OwnerName;

	TWeakObjectPtr<UWorld> World;
	FControlFlowBPCallSite CreateSite;

	FControlFlowBPCallSite ExecuteSite;
	double CreateTime = 0.0;
	double StartTime = 0.0;
	double EndTime = 0.0;

	EControlFlowBPFlowState State = EControlFlowBPFlowState::Building;
	EControlFlowBPCancelCause CancelCause = EControlFlowBPCancelCause::None;
	FString CancelReason;
	EControlFlowBPCancelCause InterruptCause = EControlFlowBPCancelCause::None;
	FString InterruptReason;
	FString StepFailurePolicy;
	FString FinalVariables;

	TSharedRef<FControlFlowBPLane> RootLane;
	TArray<TSharedRef<FControlFlowBPStepRecord>> History;
	int32 NumForgotten = 0;
	bool bTraceSteps = false;

	bool IsFinished() const { return State == EControlFlowBPFlowState::Completed || State == EControlFlowBPFlowState::Cancelled; }
	double GetTime() const;
	bool IsClockLive() const { return Clock.IsLive(); }
	double GetElapsed() const;
	void GetActiveSteps(TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, bool bLeavesOnly) const;
	void GetPendingSteps(TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, int32 MaxCount) const;
	void ForEachStep(TFunctionRef<void(const TSharedRef<FControlFlowBPStepRecord>&)> Visitor) const;
	FString DescribeState() const;
	void GetDumpLines(TArray<FString>& OutLines) const;
	void Dump(ELogVerbosity::Type Verbosity, const FString& Heading = FString()) const;

	bool IsTracing() const;

	TSharedRef<FControlFlowBPStepRecord> AddStep(FControlFlowBPLane& Lane, EControlFlowBPStepType Type, const FString& StepName,
		const FString& StepPath, const UObject* HandlerObject = nullptr, FName HandlerFunction = NAME_None);

	TSharedRef<FControlFlowBPLane> AddChildLane(const TSharedRef<FControlFlowBPStepRecord>& OwnerStep, const FString& LanePath);

	void NotifyFlowStarted(const FControlFlowBPCallSite& InExecuteSite);
	void NotifyStepStarted(FControlFlowBPStepRecord& Step);
	TSharedPtr<FControlFlowBPStepRecord> NotifyNodeCompleted(FControlFlowBPLane& Lane);
	void NotifyLaneCancelled(FControlFlowBPLane& Lane);
	void NotifyLaneCompleted(FControlFlowBPLane& Lane);
	void SkipLane(FControlFlowBPLane& Lane, const FString& Why);
	void MarkRunningStepsCancelRequested();
	void SetCancelReason(EControlFlowBPCancelCause Cause, const FString& Reason);

	void NotifyFlowFinished(bool bCancelled);
	void TraceStepDetail(const FControlFlowBPStepRecord& Step, const FString& Text) const;

private:
	void FinishStep(const TSharedRef<FControlFlowBPStepRecord>& Step, EControlFlowBPStepState NewState, const FString& NewDetail = FString());
	void SweepLane(FControlFlowBPLane& Lane, EControlFlowBPStepState RunningState, EControlFlowBPStepState PendingState, const FString& Why);
	void Trace(ELogVerbosity::Type Verbosity, int32 Depth, const FString& Text) const;
	void WarnLostTrack(const FControlFlowBPLane& Lane, const TCHAR* What);
	FString CancelledAt;

	FControlFlowBPClock Clock;
	double ClockAtCreate = 0.0;

	int32 NextSequence = 0;
	uint64 RegionId = 0;
	bool bWarnedLostTrack = false;
};

/** The flow registry and the debugging services built on it. */
class CONTROLFLOWBP_API FControlFlowBPDebug
{
public:
	static void RegisterFlow(const TSharedRef<FControlFlowBPFlowRecord>& Flow);
	static void RetainFinishedFlow(const TSharedRef<FControlFlowBPFlowRecord>& Flow);
	static void GetFlows(TArray<TSharedRef<FControlFlowBPFlowRecord>>& OutFlows);
	static void ClearFinishedFlows();
	static uint64 GetChangeCount();
	static bool IsTraceEnabled(int32 Level = 1);
	static int32 GetHistorySize();
	static bool ShouldDumpOnFailure();
	static bool MatchesBreakPattern(const FString& StepPath);
	static void ReportCallerIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPStepRecord* Step = nullptr);
	static void ReportStepIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPStepRecord& Step);
	static void ReportNeverExecuted(FControlFlowBPFlowRecord& Flow);
	static void NotifyHandlerStarting(const FControlFlowBPStepRecord& Step);
	static void DrawDebugHUD(AHUD* HUD, UCanvas* Canvas, const FDebugDisplayInfo& DisplayInfo, float& YL, float& YPos);
	static FString FormatSeconds(double Seconds);

	/**
	 * Fired right before a step's Blueprint handler runs. bMatchesBreakPattern is true when the step
	 * matches ControlFlowBP.BreakOnStep. The editor answers with a single-step request, which pauses
	 * the Blueprint debugger on the handler's first node.
	 */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStepHandlerStarting, const FControlFlowBPStepRecord&, bool);
	static FOnStepHandlerStarting OnStepHandlerStarting;

	/**
	 * A problem worth showing beyond the log. Site is where to send the author: the Blueprint call that
	 * misused the API, or else the Queue node of the step involved. The editor turns it into a PIE
	 * message log entry with a link to that node.
	 */
	DECLARE_MULTICAST_DELEGATE_FourParams(FOnIssue, ELogVerbosity::Type, const FString&, const FControlFlowBPCallSite&, const FControlFlowBPStepRecord*);
	static FOnIssue OnIssue;
};
