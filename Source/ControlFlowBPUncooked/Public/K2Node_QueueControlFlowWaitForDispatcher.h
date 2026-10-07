#pragma once

#include "CoreMinimal.h"
#include "K2Node_QueueControlFlowBase.h"
#include "K2Node_QueueControlFlowWaitForDispatcher.generated.h"

/**
 * Queue Wait For Event Dispatcher, with the dispatcher picked from a dropdown of the ones Object
 * has instead of typed as a name.
 *
 *   Queue Wait For Event Dispatcher
 *     Target            -> the flow to queue onto
 *     Object            -> whose dispatcher to wait for; self when unconnected
 *     Event Dispatcher  -> one of Object's type's dispatchers, Blueprint or C++
 *     Timeout Seconds, Step Name
 *
 * The dropdown and the compile-time check read Object's type off its connection, as the Bind
 * Event nodes do: a dispatcher of a subclass needs a cast first.
 */
UCLASS(MinimalAPI)
class UK2Node_QueueControlFlowWaitForDispatcher : public UK2Node_QueueControlFlowBase
{
	GENERATED_BODY()

public:
	CONTROLFLOWBPUNCOOKED_API static const FName PN_DispatcherName;
	CONTROLFLOWBPUNCOOKED_API static const FName PN_TimeoutSeconds;

	virtual FText GetTooltipText() const override;
	virtual FText GetKeywords() const override;
	virtual FSlateIcon GetIconAndTint(FLinearColor& OutColor) const override;

	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void HandleVariableRenamed(UBlueprint* InBlueprint, UClass* InVariableClass, UEdGraph* InGraph, const FName& InOldVarName, const FName& InNewVarName) override;
	virtual bool ReferencesVariable(const FName& InVarName, const UStruct* InScope) const override;

	virtual TArray<FName> GetPickableFunctions(const UEdGraphPin& Pin) const override;
	virtual FText GetCreateFunctionLabel(const UEdGraphPin& Pin) const override;
	virtual UObject* CreateFunctionFor(const UEdGraphPin& Pin) override;
	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const override;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const override;
	virtual UObject* GetFunctionDefinition(const UEdGraphPin& Pin) const override;
	virtual FText GetPickPrompt(const UEdGraphPin& Pin) const override;

	virtual bool HasStepHandler() const override { return false; }

protected:
	virtual FName GetPickPinName() const override { return PN_DispatcherName; }
	virtual FText GetBaseTitle() const override;
	virtual FText GetPickLabel() const override;
	virtual void CreateStepPins() override;
	virtual FName ResolvePickedName() const override;
	virtual FGuid FindPickGuid(FName Name) const override;
	virtual bool PicksFunction() const override { return false; }

	virtual void ValidatePick(FCompilerResultsLog&) const override {}

private:
	const UClass* GetObjectClass() const;
	bool IsObjectSelf() const;
};
