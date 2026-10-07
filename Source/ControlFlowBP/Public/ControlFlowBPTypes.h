#pragma once

#include "CoreMinimal.h"
#include "StructUtils/InstancedStruct.h"
#include "ControlFlowBPTypes.generated.h"

class UControlFlowBP;
class UControlFlowStepHandle;
class UControlFlowBranchScope;
class UControlFlowForkScope;
class UControlFlowLoopScope;

/**
 * The order a Parallel step starts its tracks in. Neither mode is preemptive: both run the tracks on
 * the calling thread. Tracks only genuinely overlap if they contain waits (or delays). Two tracks of
 * purely synchronous steps run one after the other.
 */
UENUM(BlueprintType)
enum class EControlFlowConcurrency : uint8
{
	Ordered		UMETA(DisplayName = "In Order"),
	Shuffled	UMETA(DisplayName = "Random Order")
};

/** The kind of step a function makes, deduced from its signature - except Race and Delay, which are picked on the Queue Step node. */
UENUM(BlueprintType)
enum class EControlFlowStepKind : uint8
{
	Invalid		UMETA(Hidden),
	Function,
	Wait,
	SubFlow,
	Branch		UMETA(DisplayName = "Switch"),
	Fork		UMETA(DisplayName = "Parallel"),
	Loop,
	Race,
	Delay
};

/** Whether a loop asks its condition before the first iteration, or only after it. */
UENUM(BlueprintType)
enum class EControlFlowLoopMode : uint8
{
	While		UMETA(DisplayName = "While (check first)"),
	DoWhile		UMETA(DisplayName = "Do-While (body first)")
};

/** What a flow does when one of its Wait steps fails - by Fail Step, or by timing out. */
UENUM(BlueprintType)
enum class EControlFlowFailurePolicy : uint8
{
	CarryOn		UMETA(DisplayName = "Carry On"),
	StopFlow	UMETA(DisplayName = "Stop Flow")
};

/** Synchronous step. The flow advances as soon as this returns. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowSyncStep, FInstancedStruct, Payload);

/**
 * Synchronous step with no payload. Not exposed as a pin - it exists so Queue Step can bind a
 * parameterless Blueprint event, which is the commonest shape by far.
 */
DECLARE_DYNAMIC_DELEGATE(FControlFlowSimpleStep);

/** Asynchronous step. The flow is parked until the handle is resolved exactly once. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowWaitStep, UControlFlowStepHandle*, Handle);

/**
 * Populates a sub-flow. Invoked when the step is reached, not when it is queued.
 * The payload is read back off the sub-flow with Get Step Payload, matching how the switch,
 * parallel and loop scopes expose theirs - which also keeps every definer signature to one
 * parameter, so Queue Step can deduce them all.
 */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowPopulate, UControlFlowBP*, SubFlow);

/** Defines the cases and picks one. Invoked when the step is reached. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowDefineBranch, UControlFlowBranchScope*, Branch);

/** Defines the tracks that run in parallel. Invoked when the step is reached. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowDefineFork, UControlFlowForkScope*, Fork);

/** Asked before every iteration that could run. Answer with Continue Looping. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowLoopCondition, UControlFlowLoopScope*, Loop);

/** Builds one iteration's body. Only invoked for iterations that will actually run. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FControlFlowBuildLoop, UControlFlowLoopScope*, Loop);

/**
 * Asked when a Queue If or Queue Wait Until step needs an answer. Bind a function that returns a
 * bool - a custom event cannot return a value.
 */
DECLARE_DYNAMIC_DELEGATE_RetVal(bool, FControlFlowCondition);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FControlFlowFinishedEvent, UControlFlowBP*, Flow);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FControlFlowStepEvent, UControlFlowBP*, Flow, const FString&, StepPath);

/** Execute Flow Async's outcome pins. Step Path and Reason are empty where they do not apply. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FControlFlowExecuteResult, UControlFlowBP*, Flow, const FString&, StepPath, const FString&, Reason);
