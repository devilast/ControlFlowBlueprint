#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPDebug.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UEdGraphNode;

struct FControlFlowBPPreviewStep;

/** One flow a step will run: a track, a case, or a sub-flow's or a loop iteration's steps. */
struct FControlFlowBPPreviewLane
{
	FString Name;
	TWeakObjectPtr<const UEdGraphNode> SourceNode;

	TArray<TSharedRef<FControlFlowBPPreviewStep>> Steps;
};

/** A step the flow has not queued yet, as its Blueprint says it will. */
struct FControlFlowBPPreviewStep
{
	TSharedRef<FControlFlowBPStepRecord> Record = MakeShared<FControlFlowBPStepRecord>();
	TWeakObjectPtr<const UEdGraphNode> QueueNode;

	TArray<FControlFlowBPPreviewLane> Lanes;
};

/**
 * Predicts, from its Blueprint, the flows a step will run once it starts: a Parallel's or Race's
 * tracks, a Switch's or an If's cases, a Sub Flow's or a loop iteration's steps. A step builds these
 * in its Define, Populate or Build Iteration event, which the engine calls only when the step
 * starts - until then they do not exist, and the Control Flow Debugger draws this preview instead.
 *
 * Read off the event's graph: its Add Track and Add Case calls, and the Queue nodes wired to the flows
 * those return, in the order the event runs them. It is a prediction. The event may add tracks in a
 * loop or behind a Branch, compute their names, or hand a flow to a function it calls, so a preview
 * can miss things or show more than will run - and a Switch shows every case, since which one runs
 * is decided when the step starts.
 */
class FControlFlowBPDebugPreview
{
public:
	static TArray<FControlFlowBPPreviewLane> PredictLanes(const FControlFlowBPStepRecord& Step);
};
