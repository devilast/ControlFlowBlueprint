#pragma once

#include "CoreMinimal.h"
#include "K2Node_QueueControlFlowBase.h"
#include "K2Node_QueueControlFlowWaitUntil.generated.h"

/**
 * Queue Wait Until, with its Condition picked from a dropdown of this Blueprint's own functions
 * instead of wired up with a Create Event.
 *
 *   Queue Wait Until
 *     Target           -> the flow to queue onto
 *     Condition        -> a function that takes nothing and returns a bool - pure or not
 *     Check Interval, Timeout Seconds, Step Name
 *
 * It compiles to Queue Wait Until By Name, which binds Condition by name: Create Event would refuse
 * a pure function, or one whose output is not named ReturnValue.
 */
UCLASS(MinimalAPI)
class UK2Node_QueueControlFlowWaitUntil : public UK2Node_QueueControlFlowBase
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_ConditionFunction;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_CheckInterval;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_TimeoutSeconds;

	virtual FText GetTooltipText() const override;
	virtual FText GetKeywords() const override;

	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;

protected:
	virtual FName GetPickPinName() const override { return PN_ConditionFunction; }
	virtual FText GetBaseTitle() const override;
	virtual FText GetPickLabel() const override;
	virtual void CreateStepPins() override;
	virtual bool Fits(const UFunction* Function, FString* OutWhyNot) const override;
	virtual UFunction* GetPickSignature() const override;
	virtual bool MustBeFunction() const override { return true; }
	virtual FString GetNewFunctionName() const override { return TEXT("WaitCondition"); }
};
