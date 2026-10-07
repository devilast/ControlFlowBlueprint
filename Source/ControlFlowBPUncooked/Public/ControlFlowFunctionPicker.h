#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "ControlFlowFunctionPicker.generated.h"

class UEdGraphPin;

/**
 * What a node's dropdown picked: its name, and a GUID that follows it through a rename - a function
 * graph's GraphGuid, a custom event's NodeGuid, or an event dispatcher variable's VarGuid.
 */
USTRUCT()
struct FControlFlowPickedFunction
{
	GENERATED_BODY()

	UPROPERTY()
	FName Name;

	UPROPERTY()
	FGuid Guid;
};

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class UControlFlowFunctionPicker : public UInterface
{
	GENERATED_BODY()
};

/**
 * A node whose Name pins each pick one of this Blueprint's functions or custom events. The editor
 * draws those pins as a dropdown of the ones that fit, plus "[Create a matching ...]".
 */
class IControlFlowFunctionPicker
{
	GENERATED_BODY()

public:
	virtual bool IsFunctionPin(const UEdGraphPin& Pin) const = 0;
	virtual bool IsFunctionOptional(const UEdGraphPin& Pin) const = 0;
	virtual TArray<FName> GetPickableFunctions(const UEdGraphPin& Pin) const = 0;
	virtual FText GetCreateFunctionLabel(const UEdGraphPin& Pin) const = 0;
	virtual UObject* CreateFunctionFor(const UEdGraphPin& Pin) = 0;
	virtual FText GetNoFunctionsLabel(const UEdGraphPin& Pin) const = 0;
	virtual FText GetFunctionPinHint(const UEdGraphPin& Pin) const = 0;
	virtual UObject* GetFunctionDefinition(const UEdGraphPin& Pin) const = 0;

	virtual FText GetPickPrompt(const UEdGraphPin& Pin) const
	{
		return NSLOCTEXT("ControlFlowFunctionPicker", "SelectFunction", "Select Function...");
	}
};
