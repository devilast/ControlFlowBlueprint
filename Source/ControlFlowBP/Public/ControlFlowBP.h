#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StructUtils/PropertyBag.h"
#include "ControlFlow.h"
#include "ControlFlowNode.h"
#include "ControlFlowBPClock.h"
#include "ControlFlowBPDebug.h"
#include "ControlFlowBPTypes.h"
#include "ControlFlowBP.generated.h"

class UControlFlowLoopScope;
class UControlFlowStepHandle;

/** How QueueLoopInternal decides on and describes its iterations. */
struct FControlFlowBPLoopSetup
{
	const TCHAR* NodeLabel = TEXT("Queue Step");
	TFunction<bool(int32)> NativeCondition;
	FInstancedPropertyBag Items;
	FString Info;
};

/**
 * Blueprint-facing builder around the engine's FControlFlow.
 *
 * FControlFlow's own API is template-based: QueueStep deduces the node kind from a member-function
 * pointer's signature, payloads are variadic, and the sugar (Loop/BranchFlow/ForkFlow) takes
 * lambdas. None of that has a reflected form, so this wrapper inverts the binding direction: you
 * name the node kind explicitly and hand it a dynamic delegate the graph implements.
 *
 * Usage:
 *
 *   Create Control Flow (Owner = self, Flow Debug Name = "Encounter")
 *     -> Queue Step (PlayIntro)       a Wait step: PlayIntro takes a Control Flow Step Handle
 *     -> Queue If   (IsBossLevel)     Then / Else populate the case that runs
 *     -> Queue Step (Cleanup)         a Function step
 *     -> Execute Flow Async           On Completed / On Cancelled / On Step Failed
 *
 * Every queue node returns the flow on a pin named "Flow", so they chain. Steps share data through
 * Set / Get Flow Variable.
 *
 * Lifetime: a running flow is kept alive by UControlFlowBPSubsystem for the duration of the run.
 * You do NOT need to store it in a variable, though you will want to if you intend to cancel it.
 *
 * Time: delays, timeouts and Wait Until checks run on the owner's game time, like its Delay nodes -
 * they stand still while the game is paused, by Pause in Play In Editor too, unless the owner ticks
 * while paused (a widget, a player controller). See FControlFlowBPClock.
 *
 * Debugging (see ControlFlowBPDebug.h):
 *   Control Flow Debugger tab      Tools > Debug, or ControlFlowBP.Debugger: every flow as a live graph or tree of its steps
 *   ControlFlowBP.Trace 1          log every step as it starts and ends (or Enable Step Trace on one flow)
 *   showdebug ControlFlow          on-screen overlay of the running flows
 *   ControlFlowBP.List / .Dump     what each flow is doing, what is queued, what already happened
 *   ControlFlowBP.BreakOnStep      pause the Blueprint debugger when a matching step runs
 *   Unreal Insights                every step is a timing region (-trace=default,region)
 * In the editor, Queue nodes show what their step is doing during PIE, and right-clicking one
 * offers "Show in Control Flow Debugger" and "Break When This Step Runs".
 *
 * Threading: game thread only.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Control Flow"))
class CONTROLFLOWBP_API UControlFlowBP : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Creates a new, empty flow. Queue steps onto it, then call Execute Flow or Execute Flow Async.
	 *
	 * @param Owner               Cancels the flow when it dies. Usually self.
	 * @param FlowDebugName       Shows up in logs, the Control Flow Debugger and Get Current Step Path.
	 * @param DefaultStepTimeout  Seconds a Wait step may take before it is failed. 0 lets waits run
	 *                            forever. A Wait step's own Timeout Seconds overrides it. Game time:
	 *                            time spent paused does not count.
	 *
	 * No WorldContext meta by design: GetHiddenPinsForFunction only hides and defaults a
	 * WorldContext pin when the calling Blueprint's native parent ImplementsGetWorld, so a
	 * WorldContext pin makes this node uncallable from e.g. a plain UObject Blueprint.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow",
		meta = (DisplayName = "Create Control Flow", DefaultToSelf = "Owner", Keywords = "new make start sequence"))
	static UControlFlowBP* CreateControlFlow(UObject* Owner, FString FlowDebugName, float DefaultStepTimeout = 30.f);

	/**
	 * Returns the flow registered under (Owner, FlowId), creating it if there isn't one.
	 * Lets one Blueprint hand a flow to another without a shared variable.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow", meta = (DefaultToSelf = "Owner"))
	static UControlFlowBP* FindOrCreateNamedFlow(UObject* Owner, FString FlowId, float DefaultStepTimeout = 30.f);

	/** Returns the flow registered under (Owner, FlowId), or null. */
	UFUNCTION(BlueprintPure, Category = "Control Flow", meta = (DefaultToSelf = "Owner"))
	static UControlFlowBP* FindNamedFlow(UObject* Owner, FString FlowId);

	UFUNCTION(BlueprintPure, Category = "Control Flow", meta = (DefaultToSelf = "Owner"))
	static bool IsNamedFlowRunning(UObject* Owner, FString FlowId);

	/** Cancels the named flow if it is running, and forgets it. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow", meta = (DefaultToSelf = "Owner"))
	static void StopNamedFlow(UObject* Owner, FString FlowId);

	/**
	 * For a function on this Blueprint, use Queue Step instead: it picks the function from a dropdown,
	 * so a wrong signature is a compile error. This node is for when the target or the function name
	 * is only known at runtime.
	 *
	 * Queues a step and works out what KIND of step it is from the named function's signature. The
	 * function must return nothing, have no output parameters, and take one of:
	 *
	 *   (nothing)                       -> synchronous step
	 *   (Instanced Struct Payload)      -> synchronous step, with the payload
	 *   (Control Flow Step Handle)      -> async step; resolve the handle exactly once
	 *   (Control Flow)                  -> sub-flow; queue onto the flow you are handed
	 *   (Control Flow Switch)           -> switch; Add Case, then Select Case
	 *   (Control Flow Parallel)         -> parallel tracks; Add Track
	 *   (Control Flow Loop)             -> loop; Get Body to populate, Continue Looping to answer
	 *
	 * A typo is reported when the step is QUEUED, not when it runs, with the shapes above. A loop
	 * function both populates the body and answers, so the body is populated on the final,
	 * terminating call too and then discarded. Queued from here, a loop always runs Do-While and a
	 * parallel step starts its tracks in order; Queue Step has pins for both.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue|Advanced",
		meta = (DisplayName = "Queue Step By Name", DefaultToSelf = "Target", AutoCreateRefTerm = "Payload",
				AdvancedDisplay = "Payload,TimeoutSeconds", Keywords = "dynamic reflection function name"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueStep(
		UObject* Target,
		FName FunctionName,
		FString StepName,
		const FInstancedStruct& Payload,
		float TimeoutSeconds = -1.f);

	/**
	 * What Queue Step would deduce for this function, without queueing anything.
	 * Returns Invalid if the function does not exist or its signature matches no known shape.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Queue|Advanced", meta = (DefaultToSelf = "Target"))
	static EControlFlowStepKind GetStepKind(UObject* Target, FName FunctionName);

	/**
	 * Queues a step that flips how THIS flow treats a cancelled step from here on: when true, a
	 * step that cancels is treated as complete and the flow carries on instead of tearing down.
	 * Sticky: queue it again with false to restore.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (DeprecatedFunction, DeprecationMessage = "Use Set Step Failure Policy, or Fail Step on the step's handle, instead."))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueSetCancelledStepAsComplete(bool bCancelledStepIsComplete, FString StepName);

	/**
	 * Parks the flow for a number of seconds of game time: like a Delay node, it stands still while
	 * the game is paused. 0 yields to the next frame. Queue Step's Delay kind compiles to this.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueDelay(float Seconds, FString StepName);

	/** Synchronous step. The flow advances as soon as the event returns. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueFunction(
		UPARAM(DisplayName = "Do") FControlFlowSyncStep Step,
		FString StepName,
		const FInstancedStruct& Payload);

	/**
	 * Asynchronous step. The flow parks until the handle is resolved with Continue Step,
	 * Fail Step or Abort Flow From This Step - exactly once, on every code path.
	 *
	 * @param TimeoutSeconds  Fails the step if it is not resolved in time - game time, so time spent
	 *                        paused does not count. -1 uses the flow's Default Step Timeout (Create
	 *                        Control Flow, 30 s unless changed); 0 waits forever.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueWait(
		UPARAM(DisplayName = "Wait") FControlFlowWaitStep Step,
		FString StepName,
		const FInstancedStruct& Payload,
		float TimeoutSeconds = -1.f);

	/**
	 * Waits until Condition returns true. Condition is checked when the step is reached, then every
	 * Check Interval seconds (0: every frame) - but not while the game is paused. Bind it to a
	 * function that returns a bool.
	 *
	 * @param TimeoutSeconds  -1 uses the flow's Default Step Timeout; 0 waits forever.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AdvancedDisplay = "CheckInterval"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueWaitUntil(
		UPARAM(DisplayName = "Condition") FControlFlowCondition Condition,
		FString StepName,
		float CheckInterval = 0.f,
		float TimeoutSeconds = -1.f);

	/**
	 * Waits until Target's event dispatcher named Dispatcher Name is called - a Blueprint event
	 * dispatcher, or a C++ one such as On Destroyed. The dispatcher's parameters are not read.
	 *
	 * @param TimeoutSeconds  -1 uses the flow's Default Step Timeout; 0 waits forever.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueWaitForEventDispatcher(
		UObject* Target,
		FName DispatcherName,
		FString StepName,
		float TimeoutSeconds = -1.f);

	/**
	 * A named group of steps, populated when the step is reached rather than when it is queued.
	 * Purely organisational - it gives the group a name in logs and in Get Current Step Path.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueSubFlow(
		UPARAM(DisplayName = "Populate") FControlFlowPopulate Populate,
		FString TaskName,
		const FInstancedStruct& Payload);

	/**
	 * Runs Then's steps when Condition returns true, Else's otherwise. Condition is asked when the
	 * step is reached; bind it to a function that returns a bool. Leave Else unconnected to skip.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", DisplayName = "Queue If by Event"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueIf(
		UPARAM(DisplayName = "Condition") FControlFlowCondition Condition,
		UPARAM(DisplayName = "Then") FControlFlowPopulate Then,
		UPARAM(DisplayName = "Else") FControlFlowPopulate Else,
		FString TaskName);

	/**
	 * Picks one of several sub-flows. The Define event adds the cases with Add Case and picks one
	 * with Select Case or Select Case By Name. Only the selected case runs.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", DisplayName = "Queue Switch", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueBranch(
		UPARAM(DisplayName = "Define") FControlFlowDefineBranch Define,
		FString TaskName,
		const FInstancedStruct& Payload);

	/**
	 * Runs several sub-flows side by side and continues once all of them have finished. The Define
	 * event adds the tracks with Add Track.
	 *
	 * "Side by side" is cooperative, not threaded - see Control Flow Concurrency. Tracks only
	 * overlap while they wait or delay.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", DisplayName = "Queue Parallel", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueFork(
		UPARAM(DisplayName = "Define") FControlFlowDefineFork Define,
		EControlFlowConcurrency Mode,
		FString TaskName,
		const FInstancedStruct& Payload);

	/**
	 * Like Queue Parallel, but the first track to finish wins: the others are cancelled and the flow
	 * carries on. The Define event adds the tracks with Add Track.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueRace(
		UPARAM(DisplayName = "Define") FControlFlowDefineFork Define,
		FString TaskName,
		const FInstancedStruct& Payload);

	/**
	 * Repeats a body while a condition holds. Condition is asked first and answers with Continue
	 * Looping; Build Iteration then populates the body, and only runs for iterations that will happen.
	 *
	 * @param MaxIterations  Stops the loop with an error when it asks for more iterations than this.
	 *                       0, the default, never stops it.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload", AdvancedDisplay = "Payload,MaxIterations"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueLoop(
		UPARAM(DisplayName = "Condition") FControlFlowLoopCondition Condition,
		UPARAM(DisplayName = "Build Iteration") FControlFlowBuildLoop BuildIteration,
		EControlFlowLoopMode Mode,
		FString TaskName,
		const FInstancedStruct& Payload,
		int32 MaxIterations = 0);

	/** Runs Build Iteration's steps Count times. Get Iteration Index on the loop counts from 0. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueRepeat(
		int32 Count,
		UPARAM(DisplayName = "Build Iteration") FControlFlowBuildLoop BuildIteration,
		FString TaskName);

	/**
	 * Runs Build Iteration's steps once for every item of Items. In Build Iteration, Get Current Item
	 * on the loop returns the item and Get Iteration Index its index. Items is copied when the step
	 * is queued.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true", ArrayParm = "Items"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueForEach(
		const TArray<int32>& Items,
		UPARAM(DisplayName = "Build Iteration") FControlFlowBuildLoop BuildIteration,
		FString TaskName);
	DECLARE_FUNCTION(execQueueForEach);

	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueSimpleFunction(FControlFlowSimpleStep Step, FString StepName);

	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue",
		meta = (BlueprintInternalUseOnly = "true", AutoCreateRefTerm = "Payload"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueCombinedLoop(
		FControlFlowBuildLoop BuildIteration,
		EControlFlowLoopMode Mode,
		FString TaskName,
		const FInstancedStruct& Payload,
		int32 MaxIterations = 0);

	/**
	 * Expansion target for the Queue If node: Queue If with the three bound by name to Owner's
	 * functions. Condition may be pure, and its bool output may have any name - a Create Event
	 * takes neither (EdGraphSchema_K2.cpp, KismetCompiler.cpp). Then and Else may be None.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueIfByName(UObject* Owner, FName ConditionFunction, FName ThenFunction, FName ElseFunction, FString TaskName);

	/** Expansion target for the Queue Wait Until node: Queue Wait Until with Condition bound by name, as Queue If By Name binds it. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Queue", meta = (BlueprintInternalUseOnly = "true"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* QueueWaitUntilByName(UObject* Owner, FName ConditionFunction, FString StepName, float CheckInterval, float TimeoutSeconds);

	/**
	 * How the flow treats a Wait step that fails - by Fail Step or by timing out. Applies to the
	 * whole flow, from the next failure on.
	 *
	 * @param OnFailure  Carry On (the default) moves on to the next step; Stop Flow cancels the flow.
	 * @param Retries    Runs a failed step's event again, up to this many times, before giving up.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow", meta = (Keywords = "fail failure retry error policy"))
	UPARAM(DisplayName = "Flow") UControlFlowBP* SetStepFailurePolicy(EControlFlowFailurePolicy OnFailure, int32 Retries = 0);

	/**
	 * Stores Value under Name for the whole flow: every step, sub-flow and case reads the same
	 * variables. Use it to hand data from one step to the next.
	 */
	UFUNCTION(BlueprintCallable, CustomThunk, Category = "Control Flow|Variables",
		meta = (CustomStructureParam = "Value", Keywords = "set store variable data blackboard"))
	void SetFlowVariable(FName Name, const int32& Value);
	DECLARE_FUNCTION(execSetFlowVariable);

	/** Reads a variable stored with Set Flow Variable. False when there is none of that name and type. */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "Control Flow|Variables",
		meta = (CustomStructureParam = "Value", Keywords = "get read variable data blackboard"))
	UPARAM(DisplayName = "Found") bool GetFlowVariable(FName Name, int32& Value) const;
	DECLARE_FUNCTION(execGetFlowVariable);

	UFUNCTION(BlueprintPure, Category = "Control Flow|Variables")
	bool HasFlowVariable(FName Name) const;

	UFUNCTION(BlueprintCallable, Category = "Control Flow|Variables")
	void RemoveFlowVariable(FName Name);

	/**
	 * The flow whose step is running right now - from inside a step's event, the way to reach its flow
	 * variables or to cancel it. Null outside a step.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow", meta = (Keywords = "current this flow self"))
	static UControlFlowBP* GetRunningFlow();

	/** Routes this flow's progress into the engine's FTrackedActivity (status bar / log). */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Debug")
	UPARAM(DisplayName = "Flow") UControlFlowBP* EnableActivityTracking();

	/**
	 * Logs every step of this flow to LogControlFlowBP as it starts and ends, with timings and
	 * outcomes - whatever ControlFlowBP.Trace is set to. Can be called at any time, on any flow in
	 * the tree; it applies to the whole tree.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Debug")
	UPARAM(DisplayName = "Flow") UControlFlowBP* EnableStepTrace();

	/**
	 * Starts the flow. Call once, on the flow returned by Create Control Flow. Execute Flow Async
	 * does the same and also tells you how the flow ended.
	 *
	 * Never call this on a sub-flow, case, track or loop body: the engine starts those for you,
	 * and starting one yourself would hang the flow, so it is refused.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow", meta = (Keywords = "run start execute"))
	void ExecuteFlow();

	/**
	 * Cancels this flow and everything under it. Safe to call from inside a step - it defers to
	 * the next tick rather than re-entering the flow.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow", meta = (DisplayName = "Cancel Flow", Keywords = "stop abort"))
	void CancelEntireFlow();

	UFUNCTION(BlueprintPure, Category = "Control Flow")
	bool IsRunning() const;

	/** True once the flow has completed or been cancelled. A finished flow cannot be restarted. */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	bool IsFinished() const;

	/** Steps still queued on THIS flow (not counting anything nested). */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	int32 NumInQueue() const;

	UFUNCTION(BlueprintPure, Category = "Control Flow")
	FString GetFlowDebugName() const;

	/**
	 * e.g. "Encounter.Waves#3.WaveKind.Case1" - this flow's position in the tree. "#3" is the third
	 * loop iteration, the one for which Get Iteration Index returns 2.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	FString GetFlowPath() const;

	/**
	 * e.g. "Encounter.Waves#3.WaveKind.Case1.BossFight" - the step running right now. Empty when
	 * nothing is running.
	 *
	 * While parallel tracks overlap, several steps run at once and this returns the one that started
	 * most recently. Get Active Step Paths returns all of them.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	FString GetCurrentStepPath() const;

	/**
	 * Every step running right now, one per parallel track. A sub-flow, switch, parallel or loop step
	 * is left out while a step inside it is running - its path is a prefix of that step's anyway.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Debug")
	TArray<FString> GetActiveStepPaths() const;

	/**
	 * Why the flow was cancelled, e.g. "its owner was destroyed" or "Abort Flow From This Step at
	 * 'Encounter.PlayIntro'". Empty unless the flow was cancelled. Useful in On Flow Cancel.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Debug")
	FString GetCancelReason() const;

	/** The object passed to Create Control Flow. Null once it has been destroyed. */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	UObject* GetOwner() const;

	/**
	 * The payload given to the Queue Step that queued this sub-flow. Only meaningful on the flow
	 * handed to a Sub Flow step's function; empty everywhere else.
	 */
	UFUNCTION(BlueprintPure, Category = "Control Flow")
	FInstancedStruct GetStepPayload() const { return StepPayload; }

	/**
	 * Logs the whole flow to LogControlFlowBP: what is running and for how long, what is queued
	 * next, and the recent history with each step's outcome. Same as ControlFlowBP.Dump.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Debug")
	void DumpFlow() const;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowStepEvent OnStepComplete;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowStepEvent OnStepFailed;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowFinishedEvent OnFlowComplete;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowFinishedEvent OnFlowCancel;

	/** C++ counterparts of On Flow Complete / On Flow Cancel and On Step Failed. Root only. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnFlowFinishedNative, bool);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnStepFailedNative, const FString&, const FString&);
	FOnFlowFinishedNative OnFlowFinishedNative;
	FOnStepFailedNative OnStepFailedNative;

	virtual void BeginDestroy() override;
	void InitAsRoot(const FString& InDebugName, UObject* InOwner, float InDefaultStepTimeout);
	void InitAsChild(const TSharedRef<FControlFlow>& InFlow, UControlFlowBP* InRoot, const FString& InPathSegment, const TSharedRef<FControlFlowBPLane>& InLane);
	static UControlFlowBP* MakeChild(FControlFlow& InFlow, UControlFlowBP* InRoot, const FString& InPathSegment, const TSharedRef<FControlFlowBPStepRecord>& OwnerStep);

	UControlFlowBP* GetRoot();
	const UControlFlowBP* GetRoot() const;

	TSharedPtr<FControlFlow> PinFlow() const { return FlowWeak.Pin(); }
	void OpenScope() { bScopeClosed = false; }
	void CloseScope() { bScopeClosed = true; }
	bool IsScopeOpen() const { return !bScopeClosed; }

	bool IsInFlowCallback() const;
	float GetDefaultStepTimeout() const;
	const FControlFlowBPClock& GetClock() const;
	bool HasExecuted() const;

	EControlFlowFailurePolicy GetStepFailurePolicy() const;
	int32 GetStepRetries() const;
	void Track(UObject* Object);
	void Release(UObject* Object);
	int32 StorePayload(const FInstancedStruct& InPayload);
	FInstancedStruct GetStoredPayload(int32 PayloadId) const;
	void ReleasePayload(int32 PayloadId);
	bool SetFlowVariableValue(FName Name, const FProperty* ValueProperty, const void* Value);
	bool GetFlowVariableValue(FName Name, const FProperty* ValueProperty, void* OutValue) const;
	const FInstancedPropertyBag* GetFlowVariables() const;
	FString DescribeFlowVariables() const;

	void NotifyStepStarted(FControlFlowBPStepRecord& Step);
	void ReportStepFailed(FControlFlowBPStepRecord& Step, const FString& Reason);
	bool IsOwnerStale() const;
	void HandleOwningWorldCleanup(EControlFlowBPCancelCause Cause, const FString& Reason);
	void CancelWithReason(EControlFlowBPCancelCause Cause, const FString& Reason);
	FControlFlowBPFlowRecord* GetDebugRecord() const;
	const TSharedPtr<FControlFlowBPLane>& GetDebugLane() const { return Lane; }
	TSharedRef<FControlFlowBPStepRecord> RecordStep(EControlFlowBPStepType Type, const FString& StepName, const FString& StepPath,
		const UObject* HandlerObject = nullptr, FName HandlerFunction = NAME_None);

	void SetFlowPath(const FString& InFlowPath);
	void QueueLoopYield();
	void ReleaseChildWhenFlowFinishes(UControlFlowBP* Child, const TSharedRef<FControlFlow>& ChildFlow);

	void SetStepPayload(const FInstancedStruct& InPayload) { StepPayload = InPayload; }
	static EControlFlowStepKind DeduceStepKind(const UFunction* Function, FString& OutError);
	static bool IsConditionFunction(const UFunction* Function, FString& OutError);

private:
	static void StartWaitAttempt(UControlFlowBP* Root, const FControlFlowNodeRef& Node, const TSharedRef<FControlFlowBPStepRecord>& Record,
		const FInstancedStruct& Payload, float Timeout, int32 Attempt,
		const TSharedRef<TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>>& Begin);

	UControlFlowBP* QueueLoopInternal(
		const FControlFlowLoopCondition& Condition,
		const FControlFlowBuildLoop& BuildIteration,
		bool bCombinedEvent,
		EControlFlowLoopMode Mode,
		const FString& TaskName,
		const FInstancedStruct& Payload,
		int32 MaxIterations,
		FControlFlowBPLoopSetup&& Setup);

	UControlFlowBP* QueueForEachInternal(const FArrayProperty* ItemsProperty, const void* Items, const FControlFlowBuildLoop& BuildIteration, const FString& TaskName);
	UControlFlowBP* QueueWaitInternal(const TCHAR* NodeLabel, const FString& Name, float TimeoutSeconds, const FInstancedStruct& Payload,
		const UObject* HandlerObject, FName HandlerFunction, const FString& Info,
		TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>&& Begin);

	UControlFlowBP* QueueForkInternal(const FControlFlowDefineFork& Define, EControlFlowConcurrency Mode, const FString& TaskName, const FInstancedStruct& Payload, bool bRace);

	void BindFlowEvents(const TSharedRef<FControlFlow>& InFlow);
	void HandleNodeCompleted(const TWeakPtr<FControlFlow>& CompletedFlow, const TWeakPtr<FControlFlowBPLane>& CompletedLane);
	void HandleFinished(bool bCancelled);
	void InvalidateHandlesFor(const TWeakPtr<FControlFlow>& CompletedFlow);
	TSharedPtr<FControlFlow> BeginQueue(const TCHAR* NodeLabel) const;
	static FString MakeStepName(const FString& InStepName, FName BoundFunctionName, const TCHAR* Fallback);
	static TWeakObjectPtr<UControlFlowBP> RunningFlow;

private:
	TSharedPtr<FControlFlow> OwnedFlow;

	TWeakPtr<FControlFlow> FlowWeak;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowBP> RootFlow;

	UPROPERTY()
	TSet<TObjectPtr<UObject>> LiveObjects;

	UPROPERTY()
	TMap<int32, FInstancedStruct> PayloadStore;

	UPROPERTY(Transient)
	FInstancedPropertyBag Variables;

	mutable TSet<FName> ReportedVariableMismatches;

	UPROPERTY()
	TWeakObjectPtr<UObject> LifetimeOwner;

	FControlFlowBPClock Clock;

	UPROPERTY()
	FInstancedStruct StepPayload;

	FString DebugName;
	FString FlowPath;
	TSharedPtr<FControlFlowBPFlowRecord> DebugRecord;

	TSharedPtr<FControlFlowBPLane> Lane;

	int32 NextPayloadId = 1;
	float DefaultStepTimeout = 0.f;
	EControlFlowFailurePolicy FailurePolicy = EControlFlowFailurePolicy::CarryOn;
	int32 StepRetries = 0;

	bool bIsRoot = false;
	bool bScopeClosed = false;
	bool bHasExecuted = false;
	bool bFinished = false;
	bool bInFlowCallback = false;
	bool bHadOwner = false;

	friend class UControlFlowStepHandle;
	friend class UControlFlowBranchScope;
	friend class UControlFlowForkScope;
	friend class UControlFlowLoopScope;
	friend class UControlFlowBPSubsystem;
	friend struct FControlFlowBPCallbackScope;
};

/**
 * Held while a Blueprint callback driven by Root runs: a step's event, a definer, a condition, or the
 * flow's own events. Makes Cancel Flow defer to the next tick instead of re-entering the flow, and
 * makes Root what Get Running Flow returns.
 */
struct CONTROLFLOWBP_API FControlFlowBPCallbackScope
{
	explicit FControlFlowBPCallbackScope(UControlFlowBP& InRoot);
	~FControlFlowBPCallbackScope();

	FControlFlowBPCallbackScope(const FControlFlowBPCallbackScope&) = delete;
	FControlFlowBPCallbackScope& operator=(const FControlFlowBPCallbackScope&) = delete;

private:
	TWeakObjectPtr<UControlFlowBP> Root;
	TWeakObjectPtr<UControlFlowBP> PreviousRunningFlow;
	bool bPreviousInFlowCallback = false;
};
