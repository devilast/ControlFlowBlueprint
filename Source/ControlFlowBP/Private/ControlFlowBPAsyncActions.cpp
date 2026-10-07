#include "ControlFlowBPAsyncActions.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPDebug.h"

UControlFlowExecuteAction* UControlFlowExecuteAction::ExecuteFlowAsync(UControlFlowBP* Flow)
{
	UControlFlowExecuteAction* Action = NewObject<UControlFlowExecuteAction>();
	Action->Flow = Flow;
	return Action;
}

void UControlFlowExecuteAction::Activate()
{
	UControlFlowBP* Root = Flow ? Flow->GetRoot() : nullptr;
	if (!Root)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, TEXT("Execute Flow Async: Flow is null, or its flow no longer exists."));
		FailToStart(TEXT("there was no flow to execute"));
		return;
	}

	if (Root->IsFinished())
	{
		const FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord();
		if (Debug && Debug->State == EControlFlowBPFlowState::Cancelled)
		{
			OnCancelled.Broadcast(Flow, FString(), Root->GetCancelReason());
		}
		else
		{
			OnCompleted.Broadcast(Flow, FString(), FString());
		}
		SetReadyToDestroy();
		return;
	}

	Root->Track(this);
	FinishedHandle = Root->OnFlowFinishedNative.AddUObject(this, &UControlFlowExecuteAction::HandleFlowFinished);
	StepFailedHandle = Root->OnStepFailedNative.AddUObject(this, &UControlFlowExecuteAction::HandleStepFailed);

	if (!Flow->HasExecuted())
	{
		Flow->ExecuteFlow();

		if (!Flow->HasExecuted() && !Root->IsFinished())
		{
			StopListening();
			Root->Release(this);
			FailToStart(TEXT("Execute Flow was refused; see the message log"));
		}
	}
}

void UControlFlowExecuteAction::HandleFlowFinished(bool bCancelled)
{
	StopListening();

	if (bCancelled)
	{
		const UControlFlowBP* Root = Flow ? Flow->GetRoot() : nullptr;
		OnCancelled.Broadcast(Flow, FString(), Root ? Root->GetCancelReason() : FString());
	}
	else
	{
		OnCompleted.Broadcast(Flow, FString(), FString());
	}

	SetReadyToDestroy();
}

void UControlFlowExecuteAction::HandleStepFailed(const FString& StepPath, const FString& Reason)
{
	OnStepFailed.Broadcast(Flow, StepPath, Reason);
}

void UControlFlowExecuteAction::FailToStart(const FString& Reason)
{
	OnCancelled.Broadcast(Flow, FString(), Reason);
	SetReadyToDestroy();
}

void UControlFlowExecuteAction::StopListening()
{
	if (UControlFlowBP* Root = Flow ? Flow->GetRoot() : nullptr)
	{
		Root->OnFlowFinishedNative.Remove(FinishedHandle);
		Root->OnStepFailedNative.Remove(StepFailedHandle);
	}

	FinishedHandle.Reset();
	StepFailedHandle.Reset();
}
