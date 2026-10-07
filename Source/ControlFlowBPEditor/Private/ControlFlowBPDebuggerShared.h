#pragma once

#include "CoreMinimal.h"
#include "ControlFlowBPDebug.h"

struct FControlFlowBPDebuggerStepItem;

namespace UE::ControlFlowBP::DebuggerTab
{
	FLinearColor StateColor(EControlFlowBPStepState State, EControlFlowBPStepType Type);
	EControlFlowBPStepState GetItemState(const FControlFlowBPDebuggerStepItem& Item);
	FText ItemLabel(const FControlFlowBPDebuggerStepItem& Item);
	void JumpToQueueNodeOrHandler(const FControlFlowBPStepRecord& Step);
}
