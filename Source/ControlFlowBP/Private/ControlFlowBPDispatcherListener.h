#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "ControlFlowBPDispatcherListener.generated.h"

class FMulticastDelegateProperty;
class UControlFlowStepHandle;

/** Continues a Queue Wait For Event Dispatcher step when the dispatcher it is bound to fires. */
UCLASS(Transient)
class UControlFlowDispatcherListener : public UObject
{
	GENERATED_BODY()

public:
	bool Bind(UObject* InTarget, FName InDispatcherName, UControlFlowStepHandle* InHandle, FString& OutError);
	void Unbind();

	static const FMulticastDelegateProperty* FindDispatcher(const UObject* Target, FName DispatcherName);
	static FString DescribeMissingDispatcher(const UObject* Target, FName DispatcherName);

private:
	UFUNCTION()
	void HandleDispatcherFired();

	FScriptDelegate MakeBinding();

	UPROPERTY()
	TWeakObjectPtr<UObject> Target;

	UPROPERTY()
	TWeakObjectPtr<UControlFlowStepHandle> Handle;

	FName DispatcherName;
	bool bBound = false;
};
