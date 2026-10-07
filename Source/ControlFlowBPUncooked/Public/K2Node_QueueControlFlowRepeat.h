#pragma once

#include "CoreMinimal.h"
#include "K2Node_QueueControlFlowBase.h"
#include "K2Node_QueueControlFlowRepeat.generated.h"

/**
 * Queue Repeat, with the function that builds each iteration picked from a dropdown of this
 * Blueprint's own functions and custom events instead of wired up with a Create Event.
 *
 *   Queue Repeat
 *     Target      -> the flow to queue onto
 *     Count       -> how many iterations
 *     Iteration   -> a function or custom event that takes a Control Flow Loop
 *     Step Name
 *
 * It compiles to Queue Repeat with a Create Event bound to the pick, as Queue Step compiles a loop.
 */
UCLASS(MinimalAPI)
class UK2Node_QueueControlFlowRepeat : public UK2Node_QueueControlFlowBase
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Count;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_IterationFunction;

	virtual FText GetTooltipText() const override;
	virtual FText GetKeywords() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;

	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;

	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;

protected:
	virtual FName GetPickPinName() const override { return PN_IterationFunction; }
	virtual FText GetBaseTitle() const override;
	virtual FText GetPickLabel() const override;
	virtual void CreateStepPins() override;
	virtual bool Fits(const UFunction* Function, FString* OutWhyNot) const override { return FitsIteration(Function, OutWhyNot); }
	virtual UFunction* GetPickSignature() const override { return GetIterationSignature(); }
	virtual FString GetNewFunctionName() const override { return TEXT("RepeatIteration"); }
};
