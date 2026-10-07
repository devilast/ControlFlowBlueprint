#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "ControlFlowBPTypes.h"
#include "ControlFlowFunctionPicker.h"
#include "K2Node_QueueControlFlowStep.generated.h"

class UK2Node_CustomEvent;

/**
 * Queue Step, with the step's function picked from a dropdown of this Blueprint's own functions
 * and custom events instead of typed as a name.
 *
 *   Queue Step
 *     Target      -> the flow to queue onto
 *     Step Kind   -> Function / Wait / Sub Flow / Switch / Parallel / Loop / Race / Delay
 *     Function    -> dropdown, filtered to the functions whose signature fits Step Kind,
 *                    plus "[Create a matching ... event]"
 *     Step Name, Payload, and whatever the kind needs (Timeout, Mode, Max Iterations)
 *
 * It compiles to the TYPED call - Queue Wait, Queue Switch, ... - with a Create Event bound to
 * the chosen function. Unlike Queue Step By Name there is no runtime reflection, and a function
 * whose signature does not fit is a compile error.
 *
 * Delay is the one kind without a function: it hides Function and Payload, takes Seconds instead,
 * and compiles to Queue Delay.
 *
 * The function is the source of truth: add or remove the handle/scope parameter on the event
 * and the node follows it to the new kind, just as FControlFlow's QueueStep deduces the node kind
 * from the member function's signature. Renaming the function or event is tracked too.
 */
UCLASS(MinimalAPI)
class UK2Node_QueueControlFlowStep : public UK2Node, public IControlFlowFunctionPicker
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Target;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_StepKind;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Function;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_StepName;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Payload;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Flow;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_TimeoutSeconds;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_ForkMode;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_LoopMode;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_MaxIterations;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Seconds;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FText GetTooltipText() const override;
	virtual FText GetKeywords() const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	virtual UObject* GetJumpTargetForDoubleClick() const override;

	virtual bool IsNodePure() const override { return false; }
	virtual void ReconstructNode() override;
	virtual void PostReconstructNode() override;
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual void HandleFunctionRenamed(UBlueprint* InBlueprint, UClass* InFunctionClass, UEdGraph* InGraph, const FName& InOldFuncName, const FName& InNewFuncName) override;
	virtual void ClearCachedBlueprintData(UBlueprint* Blueprint) override;
	virtual bool ReferencesFunction(const FName& InFunctionName, const UStruct* InScope) const override;

	CONTROLFLOWBPUNCOOKED_API EControlFlowStepKind GetStepKind() const { return StepKind; }
	CONTROLFLOWBPUNCOOKED_API FName GetStepFunctionName() const { return FunctionName; }

	virtual bool IsFunctionPin(const UEdGraphPin& Pin) const override;
	virtual bool IsFunctionOptional(const UEdGraphPin&) const override { return false; }
	virtual TArray<FName> GetPickableFunctions(const UEdGraphPin& Pin) const override;
	virtual FText GetCreateFunctionLabel(const UEdGraphPin& Pin) const override;
	virtual UObject* CreateFunctionFor(const UEdGraphPin& Pin) override;
	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;
	virtual UObject* GetFunctionDefinition(const UEdGraphPin& Pin) const override;

private:
	TArray<FName> GetCandidateFunctions() const;
	bool CanCreateMatchingEvent() const;
	UK2Node_CustomEvent* CreateMatchingEvent();
	static FText GetSignatureHint(EControlFlowStepKind Kind);

	/** The typed Queue* call a step of this kind compiles to. */
	struct FQueueCall
	{
		FName Function;
		FName DelegateParam;
		FName NameParam;
		bool bTakesPayload = true;
	};

	static FQueueCall GetQueueCall(EControlFlowStepKind Kind, bool bFunctionTakesPayload);
	static EControlFlowStepKind ResolveKind(EControlFlowStepKind Deduced, EControlFlowStepKind Chosen);
	static const UFunction* GetDelegateSignature(EControlFlowStepKind Kind, bool bFunctionTakesPayload);
	static bool IsBindable(const UFunction* Function, EControlFlowStepKind Kind);
	UFunction* ResolveStepFunction(FName* OutResolvedName = nullptr) const;
	bool FindRenamedFunction(FName& OutNewName) const;
	void SetFunctionInternal(FName NewFunctionName);
	void FollowFunctionKind();

	void CreateKindSpecificPins(EControlFlowStepKind Kind);
	static bool IsKindSpecificPin(const UEdGraphPin* Pin);
	void SyncKindSpecificPins();
	void UpdateFunctionPinsVisibility();
	void MirrorPropertiesToPins();

	static FString KindToString(EControlFlowStepKind Kind);
	static EControlFlowStepKind KindFromString(const FString& Value);

private:
	UPROPERTY()
	EControlFlowStepKind StepKind = EControlFlowStepKind::Function;

	UPROPERTY()
	FName FunctionName;

	UPROPERTY()
	FGuid FunctionGuid;
};
