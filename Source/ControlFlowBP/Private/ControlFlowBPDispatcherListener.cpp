#include "ControlFlowBPDispatcherListener.h"

#include "ControlFlowBPScopes.h"
#include "UObject/UnrealType.h"

const FMulticastDelegateProperty* UControlFlowDispatcherListener::FindDispatcher(const UObject* Target, FName DispatcherName)
{
	return (Target && !DispatcherName.IsNone()) ? FindFProperty<FMulticastDelegateProperty>(Target->GetClass(), DispatcherName) : nullptr;
}

FString UControlFlowDispatcherListener::DescribeMissingDispatcher(const UObject* Target, FName DispatcherName)
{
	if (!Target)
	{
		return TEXT("Target is null");
	}

	TArray<FString> Names;
	for (TFieldIterator<FMulticastDelegateProperty> It(Target->GetClass()); It; ++It)
	{
		Names.Add(It->GetName());
	}

	return FString::Printf(TEXT("%s has no event dispatcher named '%s'%s"), *Target->GetName(), *DispatcherName.ToString(),
		Names.IsEmpty() ? TEXT(", nor any other") : *FString::Printf(TEXT(" - it has: %s"), *FString::Join(Names, TEXT(", "))));
}

bool UControlFlowDispatcherListener::Bind(UObject* InTarget, FName InDispatcherName, UControlFlowStepHandle* InHandle, FString& OutError)
{
	const FMulticastDelegateProperty* Dispatcher = FindDispatcher(InTarget, InDispatcherName);
	if (!Dispatcher)
	{
		OutError = DescribeMissingDispatcher(InTarget, InDispatcherName);
		return false;
	}

	Target = InTarget;
	DispatcherName = InDispatcherName;
	Handle = InHandle;

	Dispatcher->AddDelegate(MakeBinding(), InTarget);
	bBound = true;
	return true;
}

void UControlFlowDispatcherListener::Unbind()
{
	if (!bBound)
	{
		return;
	}

	bBound = false;

	if (UObject* LiveTarget = Target.Get())
	{
		if (const FMulticastDelegateProperty* Dispatcher = FindDispatcher(LiveTarget, DispatcherName))
		{
			Dispatcher->RemoveDelegate(MakeBinding(), LiveTarget);
		}
	}
}

void UControlFlowDispatcherListener::HandleDispatcherFired()
{
	UControlFlowStepHandle* StepHandle = Handle.Get();
	Unbind();

	if (StepHandle && StepHandle->IsStepValid())
	{
		StepHandle->ContinueStep();
	}
}

FScriptDelegate UControlFlowDispatcherListener::MakeBinding()
{
	FScriptDelegate Binding;
	Binding.BindUFunction(this, GET_FUNCTION_NAME_CHECKED(UControlFlowDispatcherListener, HandleDispatcherFired));
	return Binding;
}
