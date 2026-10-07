#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "ControlFlowBPTypes.h"
#include "ControlFlowBPAsyncActions.generated.h"

class UControlFlowBP;

/** The Execute Flow Async node: Execute Flow with the outcome as exec pins instead of events to bind. */
UCLASS()
class CONTROLFLOWBP_API UControlFlowExecuteAction : public UBlueprintAsyncActionBase
{
	GENERATED_BODY()

public:
	/**
	 * Starts the flow, like Execute Flow, then fires On Completed or On Cancelled when it ends - no
	 * event binding needed. On Step Failed fires for every step that fails along the way. On a flow
	 * that is already running, it only waits for the end.
	 */
	UFUNCTION(BlueprintCallable, Category = "Control Flow",
		meta = (BlueprintInternalUseOnly = "true", DisplayName = "Execute Flow Async", Keywords = "run start execute async completed cancelled"))
	static UControlFlowExecuteAction* ExecuteFlowAsync(UControlFlowBP* Flow);

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowExecuteResult OnCompleted;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowExecuteResult OnCancelled;

	UPROPERTY(BlueprintAssignable, Category = "Control Flow")
	FControlFlowExecuteResult OnStepFailed;

	virtual void Activate() override;

private:
	void HandleFlowFinished(bool bCancelled);
	void HandleStepFailed(const FString& StepPath, const FString& Reason);
	void FailToStart(const FString& Reason);
	void StopListening();

	UPROPERTY()
	TObjectPtr<UControlFlowBP> Flow;

	FDelegateHandle FinishedHandle;
	FDelegateHandle StepFailedHandle;
};
