#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "StructUtils/PropertyBag.h"
#include "ControlFlowNode.h"
#include "ControlFlowBPClock.h"
#include "ControlFlowBPDebug.h"
#include "ControlFlowBPTypes.h"
#include "ControlFlowBPScopes.generated.h"

class FControlFlow;
class FControlFlowBranch;
class FConcurrentControlFlows;
class FConditionalLoop;
class UControlFlowBP;

/**
 * Handed to a Wait step's function. Resolve it exactly once, on every code path - a handle that is
 * never resolved parks the flow until the step's timeout fails it, or forever with a timeout of 0.
 * The timeout runs on the flow's clock, so it does not count time spent paused.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Control Flow Step Handle"))
class CONTROLFLOWBP_API UControlFlowStepHandle : public UObject
{
	GENERATED_BODY()

public:
	/** The step succeeded. The flow advances. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Step")
	void ContinueStep();

	/**
	 * The step failed. Broadcasts On Step Failed. By default the flow then carries on to the next
	 * step; the flow's Step Failure Policy can retry the step first, or stop the flow instead.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Step")
	void FailStep(FString Reason);

	/**
	 * Tears down this flow AND every flow above it. Use when the step's failure invalidates the
	 * whole operation.
	 *
	 * One exception worth knowing: inside a Parallel or Race track this only ends the track. A
	 * Parallel step still reports complete once its other tracks finish, and an aborted track never
	 * wins a Race - the Race waits for one of the others.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Step",
		meta = (DisplayName = "Abort Flow From This Step"))
	void AbortFlow();

	/** True when something upstream has asked this flow to cancel. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	bool IsCancelRequested() const;

	/** False once the step has been resolved, or if the flow moved on without it. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	bool IsStepValid() const;

	/** 1 the first time the step runs, then 2, 3... when the flow's Step Failure Policy retries it. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	int32 GetAttempt() const { return Attempt; }

	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	FInstancedStruct GetPayload() const { return Payload; }

	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	FString GetStepPath() const { return StepPath; }

	/** The flow this step belongs to - for its flow variables, or to cancel it. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Step")
	UControlFlowBP* GetFlow() const;

	void Init(const FControlFlowNodeRef& InNode, UControlFlowBP* InRoot, const FString& InStepPath, const FInstancedStruct& InPayload,
		const TSharedPtr<FControlFlowBPStepRecord>& InRecord, int32 InAttempt = 1);
	void ArmWatchdog(float Seconds);
	void StartPolling(const FControlFlowCondition& Condition, float Interval);
	void SetListener(UObject* InListener);
	TFunction<void(const FControlFlowNodeRef&, int32, const FInstancedStruct&)> RestartStep;
	void AbortWithReason(EControlFlowBPCancelCause Cause, const FString& Reason);
	void ForceInvalidate();

	TWeakPtr<FControlFlow> GetOwningFlow() const;

private:
	bool TryConsume(const TCHAR* Verb);
	void HandleFailure(const FString& Reason);
	void StopWaiting();
	void HandleTimeout();
	void Poll();

	FControlFlowNodePtr Node;

	TSharedPtr<FControlFlowBPStepRecord> Record;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowBP> Root;

	UPROPERTY()
	FInstancedStruct Payload;

	UPROPERTY()
	TObjectPtr<UObject> Listener;

	FString StepPath;
	FControlFlowBPTimer Watchdog;
	FControlFlowBPTimer Poller;
	FControlFlowCondition PollCondition;
	float PollInterval = 0.f;
	float TimeoutSeconds = 0.f;
	int32 Attempt = 1;
	bool bConsumed = false;
};

/**
 * Handed to a Switch step's function. Add the cases you might take, then pick one.
 * Only the selected case is populated into the flow; the others are discarded.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Control Flow Switch"))
class CONTROLFLOWBP_API UControlFlowBranchScope : public UObject
{
	GENERATED_BODY()

public:
	/** Adds (or returns) the case for Key. Queue steps onto the returned flow. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Switch", meta = (DisplayName = "Add Case"))
	UPARAM(DisplayName = "Case") UControlFlowBP* AddBranch(int32 Key, UPARAM(DisplayName = "Case Name") FString BranchName);

	/**
	 * Picks the case to run. Call once. If you select a key you never added, an empty case is
	 * created for it and the flow simply moves on.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Switch", meta = (DisplayName = "Select Case"))
	void SelectBranch(int32 Key);

	/** Picks the case to run by the name it was added with. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Switch")
	void SelectCaseByName(FString CaseName);

	UFUNCTION(BlueprintPure, Category = "Control Flow|Switch")
	FInstancedStruct GetPayload() const { return Payload; }

	/** The flow this switch belongs to - for its flow variables. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Switch")
	UControlFlowBP* GetFlow() const;

	void Init(const TSharedRef<FControlFlowBranch>& InBranch, UControlFlowBP* InRoot, const FString& InTaskPath, const FInstancedStruct& InPayload,
		const TSharedPtr<FControlFlowBPStepRecord>& InRecord);

	int32 CloseAndResolve();

private:
	TWeakPtr<FControlFlowBranch> BranchWeak;

	TSharedPtr<FControlFlowBPStepRecord> Record;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowBP> Root;

	UPROPERTY()
	TMap<int32, TObjectPtr<UControlFlowBP>> Cases;

	TMap<int32, FString> CaseNames;

	UPROPERTY()
	FInstancedStruct Payload;

	FString TaskPath;
	int32 SelectedKey = 0;
	bool bHasSelection = false;
	bool bClosed = false;
};

/**
 * Handed to a Parallel or Race step's function. Add the tracks; a Parallel step continues once
 * all of them finish, a Race once the first one does.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Control Flow Parallel"))
class CONTROLFLOWBP_API UControlFlowForkScope : public UObject
{
	GENERATED_BODY()

public:
	/** Adds (or returns) the track for Key. Queue steps onto the returned flow. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Parallel", meta = (DisplayName = "Add Track"))
	UPARAM(DisplayName = "Track") UControlFlowBP* AddProng(int32 Key, UPARAM(DisplayName = "Track Name") FString ProngName);

	UFUNCTION(BlueprintPure, Category = "Control Flow|Parallel")
	FInstancedStruct GetPayload() const { return Payload; }

	/** The flow this step belongs to - for its flow variables. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Parallel")
	UControlFlowBP* GetFlow() const;

	void Init(const TSharedRef<FConcurrentControlFlows>& InFork, UControlFlowBP* InRoot, const FString& InTaskPath, const FInstancedStruct& InPayload,
		const TSharedPtr<FControlFlowBPStepRecord>& InRecord, bool bInRace);
	int32 CloseAndCountProngs();

private:
	void ArmRace();

	TWeakPtr<FConcurrentControlFlows> ForkWeak;

	TSharedPtr<FControlFlowBPStepRecord> Record;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowBP> Root;

	UPROPERTY()
	TMap<int32, TObjectPtr<UControlFlowBP>> Prongs;

	UPROPERTY()
	FInstancedStruct Payload;

	FString TaskPath;
	bool bRace = false;
	bool bClosed = false;
};

/**
 * Handed to a Loop step's function, and to the Build Iteration event of Queue Repeat and
 * Queue For Each.
 *
 * A Loop step's function runs once per iteration: queue that iteration's steps onto Get Body, and
 * answer with Continue Looping whether to run another one.
 * In Build Iteration, only queue onto Get Body: Queue Repeat and Queue For Each decide how often
 * it runs.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Control Flow Loop"))
class CONTROLFLOWBP_API UControlFlowLoopScope : public UObject
{
	GENERATED_BODY()

public:
	/** The flow to queue this iteration's steps onto. Valid only while the loop's function or Build Iteration runs. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Loop")
	UPARAM(DisplayName = "Body") UControlFlowBP* GetBody() const;

	/** A Loop step's answer: run another iteration or stop. Call once each time the loop's function runs. */
	UFUNCTION(BlueprintCallable, Category = "Control Flow|Loop")
	void ContinueLooping(bool bRunAnotherIteration);

	/** Which iteration this is, counting from 0. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Loop")
	int32 GetIterationIndex() const { return bInBuildPhase ? IterationIndex - 1 : IterationIndex; }

	/**
	 * Queue For Each: the item this iteration runs for. False outside Build Iteration, or when Item's
	 * type is not the array's.
	 */
	UFUNCTION(BlueprintPure, CustomThunk, Category = "Control Flow|Loop", meta = (CustomStructureParam = "Item"))
	UPARAM(DisplayName = "Valid") bool GetCurrentItem(int32& Item) const;
	DECLARE_FUNCTION(execGetCurrentItem);

	UFUNCTION(BlueprintPure, Category = "Control Flow|Loop")
	FInstancedStruct GetPayload() const { return Payload; }

	/** The flow this loop belongs to - for its flow variables. */
	UFUNCTION(BlueprintPure, Category = "Control Flow|Loop")
	UControlFlowBP* GetFlow() const;

	void Init(UControlFlowBP* InRoot, UControlFlowBP* InBody, const FString& InTaskPath, const FInstancedStruct& InPayload,
		const TSharedPtr<FControlFlowBPStepRecord>& InRecord);

	void SetItems(const FInstancedPropertyBag& InItems) { Items = InItems; }
	bool CopyCurrentItem(const FProperty* ItemProperty, void* OutItem) const;
	UControlFlowBP* GetBodyBuilder() const { return Body; }

	void BeginConditionPhase();
	bool EndConditionPhase();
	void BeginBuildPhase();
	void EndBuildPhase();
	void BeginCombinedPhase();
	bool EndCombinedPhase();
	int32 GetIterationCount() const { return IterationIndex; }
	const FString& GetTaskPath() const { return TaskPath; }

private:
	void UpdateBodyPath();

	TSharedPtr<FControlFlowBPStepRecord> Record;

	UPROPERTY()
	TObjectPtr<UControlFlowBP> Body;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowBP> Root;

	UPROPERTY()
	FInstancedStruct Payload;

	UPROPERTY()
	FInstancedPropertyBag Items;

	FString TaskPath;
	int32 IterationIndex = 0;
	bool bInConditionPhase = false;
	bool bInBuildPhase = false;
	bool bAnswered = false;
	bool bRunAgain = false;
	mutable bool bReportedItemMismatch = false;
};
