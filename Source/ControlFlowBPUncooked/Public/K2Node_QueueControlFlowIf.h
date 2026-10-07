#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "ControlFlowFunctionPicker.h"
#include "K2Node_QueueControlFlowIf.generated.h"

/**
 * Queue If, with its Condition, Then and Else picked from dropdowns of this Blueprint's own
 * functions instead of wired up with Create Event nodes.
 *
 *   Queue If
 *     Target      -> the flow to queue onto
 *     Condition   -> a function that takes nothing and returns a bool - pure or not
 *     Then, Else  -> functions or custom events that take a Control Flow; either may be None
 *     Step Name
 *
 * It compiles to Queue If By Name, which binds the three by name. Create Event would refuse a
 * pure condition, or one whose output is not named ReturnValue, so the node checks the signatures
 * itself when the Blueprint compiles. Renaming a function or event is tracked.
 */
UCLASS(MinimalAPI)
class UK2Node_QueueControlFlowIf : public UK2Node, public IControlFlowFunctionPicker
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Target;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_ConditionFunction;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_ThenFunction;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_ElseFunction;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_StepName;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Flow;

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

	virtual bool IsFunctionPin(const UEdGraphPin& Pin) const override;
	virtual bool IsFunctionOptional(const UEdGraphPin& Pin) const override;
	virtual TArray<FName> GetPickableFunctions(const UEdGraphPin& Pin) const override;
	virtual FText GetCreateFunctionLabel(const UEdGraphPin& Pin) const override;
	virtual UObject* CreateFunctionFor(const UEdGraphPin& Pin) override;
	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;
	virtual UObject* GetFunctionDefinition(const UEdGraphPin& Pin) const override;

	CONTROLFLOWBPUNCOOKED_API FName GetConditionFunction() const { return Condition.Name; }
	CONTROLFLOWBPUNCOOKED_API FName GetThenFunction() const { return Then.Name; }
	CONTROLFLOWBPUNCOOKED_API FName GetElseFunction() const { return Else.Name; }

private:
	FControlFlowPickedFunction* FindPick(FName PinName);
	const FControlFlowPickedFunction* FindPick(FName PinName) const;
	UFunction* ResolvePick(const FControlFlowPickedFunction& Pick, FName* OutResolvedName = nullptr) const;
	FName FindRenamedName(const FControlFlowPickedFunction& Pick) const;

	bool HasRenamedPicks() const;
	void FollowRenamedPicks();
	static bool Fits(FName PinName, const UFunction* Function, FString* OutWhyNot = nullptr);
	static UFunction* GetPickSignature(FName PinName);
	void SetPick(FName PinName, FName FunctionName);
	void MirrorPropertiesToPins();

	UPROPERTY()
	FControlFlowPickedFunction Condition;

	UPROPERTY()
	FControlFlowPickedFunction Then;

	UPROPERTY()
	FControlFlowPickedFunction Else;
};
