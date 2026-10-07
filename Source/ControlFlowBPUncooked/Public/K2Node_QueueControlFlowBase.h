#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "ControlFlowFunctionPicker.h"
#include "K2Node_QueueControlFlowBase.generated.h"

class UK2Node_CallFunction;

/**
 * What Queue Wait Until, Queue Wait For Event Dispatcher, Queue Repeat and Queue For Each share:
 * one dropdown that picks a function or event dispatcher, the Target, Step Name and Flow pins that
 * chain the node with other Queue nodes, following the pick through a rename, and checking it when
 * the Blueprint compiles. The pick is a function unless a subclass says otherwise.
 */
UCLASS(Abstract, MinimalAPI)
class UK2Node_QueueControlFlowBase : public UK2Node, public IControlFlowFunctionPicker
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Target;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_StepName;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_Flow;

	virtual void AllocateDefaultPins() override;
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;
	virtual void PinDefaultValueChanged(UEdGraphPin* Pin) override;
	virtual void ValidateNodeDuringCompilation(FCompilerResultsLog& MessageLog) const override;
	virtual bool IsCompatibleWithGraph(const UEdGraph* TargetGraph) const override;
	virtual UObject* GetJumpTargetForDoubleClick() const override;

	virtual bool IsNodePure() const override { return false; }
	virtual void ReconstructNode() override;
	virtual void PostReconstructNode() override;
	virtual void GetMenuActions(FBlueprintActionDatabaseRegistrar& ActionRegistrar) const override;
	virtual FText GetMenuCategory() const override;
	virtual void HandleFunctionRenamed(UBlueprint* InBlueprint, UClass* InFunctionClass, UEdGraph* InGraph, const FName& InOldFuncName, const FName& InNewFuncName) override;
	virtual void ClearCachedBlueprintData(UBlueprint* Blueprint) override;
	virtual bool ReferencesFunction(const FName& InFunctionName, const UStruct* InScope) const override;

	virtual bool IsFunctionPin(const UEdGraphPin& Pin) const override;
	virtual bool IsFunctionOptional(const UEdGraphPin&) const override { return false; }
	virtual TArray<FName> GetPickableFunctions(const UEdGraphPin& Pin) const override;
	virtual FText GetCreateFunctionLabel(const UEdGraphPin& Pin) const override;
	virtual UObject* CreateFunctionFor(const UEdGraphPin& Pin) override;
	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;
	virtual UObject* GetFunctionDefinition(const UEdGraphPin& Pin) const override;

	CONTROLFLOWBPUNCOOKED_API FName GetPickedName() const { return Picked.Name; }
	virtual bool HasStepHandler() const { return true; }

protected:
	virtual FName GetPickPinName() const PURE_VIRTUAL(UK2Node_QueueControlFlowBase::GetPickPinName, return NAME_None;);
	virtual FText GetBaseTitle() const PURE_VIRTUAL(UK2Node_QueueControlFlowBase::GetBaseTitle, return FText::GetEmpty(););
	virtual FText GetPickLabel() const PURE_VIRTUAL(UK2Node_QueueControlFlowBase::GetPickLabel, return FText::GetEmpty(););
	virtual void CreateStepPins() PURE_VIRTUAL(UK2Node_QueueControlFlowBase::CreateStepPins, );

	UEdGraphPin* CreatePickPin(const FText& FriendlyName, const FText& Tooltip);

	virtual bool Fits(const UFunction*, FString*) const { return false; }
	virtual UFunction* GetPickSignature() const { return nullptr; }
	virtual bool MustBeFunction() const { return false; }
	virtual FString GetNewFunctionName() const { return TEXT("NewFunction"); }
	virtual FName ResolvePickedName() const;
	virtual bool PicksFunction() const { return true; }
	virtual FGuid FindPickGuid(FName Name) const;
	virtual void ValidatePick(FCompilerResultsLog& MessageLog) const;
	UFunction* ResolvePickedFunction(FName* OutResolvedName = nullptr) const;
	void SetPick(FName Name);
	void MirrorPropertiesToPins();

	bool CheckTargetConnected(FKismetCompilerContext& CompilerContext);
	UK2Node_CallFunction* SpawnQueueCall(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, FName QueueFunctionName,
		FName StepNameParam, bool& bSucceeded, bool bArrayFunction = false);

	void MoveToCall(FKismetCompilerContext& CompilerContext, UK2Node_CallFunction* CallNode, FName FromPinName, FName ToPinName, bool& bSucceeded);
	bool ConnectSelf(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* Pin);
	bool ConnectEvent(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph, UEdGraphPin* DelegatePin, FName FunctionName);
	void ReportExpansionFailed(FKismetCompilerContext& CompilerContext);

	static bool FitsIteration(const UFunction* Function, FString* OutWhyNot);
	static UFunction* GetIterationSignature();

	UPROPERTY()
	FControlFlowPickedFunction Picked;
};
