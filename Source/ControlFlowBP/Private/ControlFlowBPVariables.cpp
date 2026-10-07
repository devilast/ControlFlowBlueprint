#include "ControlFlowBP.h"

#include "ControlFlowBPDebug.h"
#include "ControlFlowBPPropertyUtils.h"

TWeakObjectPtr<UControlFlowBP> UControlFlowBP::RunningFlow;

FControlFlowBPCallbackScope::FControlFlowBPCallbackScope(UControlFlowBP& InRoot)
	: Root(&InRoot)
	, PreviousRunningFlow(UControlFlowBP::RunningFlow)
	, bPreviousInFlowCallback(InRoot.bInFlowCallback)
{
	InRoot.bInFlowCallback = true;
	UControlFlowBP::RunningFlow = &InRoot;
}

FControlFlowBPCallbackScope::~FControlFlowBPCallbackScope()
{
	if (UControlFlowBP* LiveRoot = Root.Get())
	{
		LiveRoot->bInFlowCallback = bPreviousInFlowCallback;
	}

	UControlFlowBP::RunningFlow = PreviousRunningFlow;
}

UControlFlowBP* UControlFlowBP::GetRunningFlow()
{
	return RunningFlow.Get();
}

UControlFlowBP* UControlFlowBP::SetStepFailurePolicy(EControlFlowFailurePolicy OnFailure, int32 Retries)
{
	UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		return this;
	}

	Root->FailurePolicy = OnFailure;
	Root->StepRetries = FMath::Max(0, Retries);

	if (FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord())
	{
		const TCHAR* const Then = OnFailure == EControlFlowFailurePolicy::StopFlow ? TEXT("stop the flow") : TEXT("carry on");
		Debug->StepFailurePolicy = Root->StepRetries > 0
			? FString::Printf(TEXT("retry %d time(s), then %s"), Root->StepRetries, Then)
			: FString(Then);
	}

	return this;
}

EControlFlowFailurePolicy UControlFlowBP::GetStepFailurePolicy() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->FailurePolicy : EControlFlowFailurePolicy::CarryOn;
}

int32 UControlFlowBP::GetStepRetries() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->StepRetries : 0;
}

bool UControlFlowBP::HasExecuted() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root && Root->bHasExecuted;
}

DEFINE_FUNCTION(UControlFlowBP::execSetFlowVariable)
{
	P_GET_PROPERTY(FNameProperty, Name);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	const void* ValueAddress = Stack.MostRecentPropertyAddress;

	P_FINISH;

	P_NATIVE_BEGIN;
	P_THIS->SetFlowVariableValue(Name, ValueProperty, ValueAddress);
	P_NATIVE_END;
}

DEFINE_FUNCTION(UControlFlowBP::execGetFlowVariable)
{
	P_GET_PROPERTY(FNameProperty, Name);

	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ValueProperty = Stack.MostRecentProperty;
	void* ValueAddress = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bFound = false;
	P_NATIVE_BEGIN;
	bFound = P_THIS->GetFlowVariableValue(Name, ValueProperty, ValueAddress);
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bFound;
}

bool UControlFlowBP::SetFlowVariableValue(FName Name, const FProperty* ValueProperty, const void* Value)
{
	using namespace UE::ControlFlowBP::PropertyUtils;

	UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Set Flow Variable '%s' on '%s': the flow no longer exists."), *Name.ToString(), *FlowPath));
		return false;
	}

	if (Name.IsNone())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Set Flow Variable on '%s': Name is empty."), *FlowPath));
		return false;
	}

	if (!ValueProperty || !Value)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Set Flow Variable '%s': the value could not be read."), *Name.ToString()));
		return false;
	}

	const FPropertyBagPropertyDesc NewDesc(Name, ValueProperty);
	if (NewDesc.ValueType == EPropertyBagPropertyType::None)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Set Flow Variable '%s': a %s cannot be stored in a flow variable."), *Name.ToString(), *DescribeType(*ValueProperty)));
		return false;
	}

	FInstancedPropertyBag& Bag = Root->Variables;
	const FPropertyBagPropertyDesc* Stored = Bag.FindPropertyDescByName(Name);
	if (Stored && Stored->CachedProperty && !Stored->CompatibleType(NewDesc))
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Set Flow Variable '%s': it held a %s and now holds a %s."),
			*Name.ToString(), *DescribeType(*Stored->CachedProperty), *DescribeType(*ValueProperty)));
		Bag.RemovePropertyByName(Name);
		Root->ReportedVariableMismatches.Remove(Name);
		Stored = nullptr;
	}

	if (!Stored)
	{
		Bag.AddProperties({ NewDesc });
		Stored = Bag.FindPropertyDescByName(Name);
	}

	if (!Stored || !Stored->CachedProperty)
	{
		return false;
	}

	CopyValue(*Stored->CachedProperty, Stored->CachedProperty->ContainerPtrToValuePtr<void>(Bag.GetMutableValue().GetMemory()), *ValueProperty, Value);
	return true;
}

bool UControlFlowBP::GetFlowVariableValue(FName Name, const FProperty* ValueProperty, void* OutValue) const
{
	using namespace UE::ControlFlowBP::PropertyUtils;

	const UControlFlowBP* Root = GetRoot();
	if (!Root || !ValueProperty || !OutValue)
	{
		return false;
	}

	const FPropertyBagPropertyDesc* Stored = Root->Variables.FindPropertyDescByName(Name);
	if (!Stored || !Stored->CachedProperty)
	{
		return false;
	}

	const void* StoredValue = Stored->CachedProperty->ContainerPtrToValuePtr<void>(Root->Variables.GetValue().GetMemory());
	if (!CanRead(*Stored->CachedProperty, StoredValue, *ValueProperty))
	{
		bool bAlreadyReported = false;
		Root->ReportedVariableMismatches.Add(Name, &bAlreadyReported);
		if (!bAlreadyReported)
		{
			FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
				TEXT("Get Flow Variable '%s': it holds a %s, which cannot be read as a %s."),
				*Name.ToString(), *DescribeType(*Stored->CachedProperty), *DescribeType(*ValueProperty)));
		}
		return false;
	}

	CopyValue(*ValueProperty, OutValue, *Stored->CachedProperty, StoredValue);
	return true;
}

bool UControlFlowBP::HasFlowVariable(FName Name) const
{
	const UControlFlowBP* Root = GetRoot();
	return Root && Root->Variables.FindPropertyDescByName(Name) != nullptr;
}

void UControlFlowBP::RemoveFlowVariable(FName Name)
{
	if (UControlFlowBP* Root = GetRoot())
	{
		Root->Variables.RemovePropertyByName(Name);
	}
}

const FInstancedPropertyBag* UControlFlowBP::GetFlowVariables() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? &Root->Variables : nullptr;
}

FString UControlFlowBP::DescribeFlowVariables() const
{
	using namespace UE::ControlFlowBP::PropertyUtils;

	const FInstancedPropertyBag* Bag = GetFlowVariables();
	const UPropertyBag* BagStruct = Bag ? Bag->GetPropertyBagStruct() : nullptr;
	if (!BagStruct)
	{
		return FString();
	}

	TArray<FString> Parts;
	const void* Memory = Bag->GetValue().GetMemory();
	for (const FPropertyBagPropertyDesc& Desc : BagStruct->GetPropertyDescs())
	{
		if (Desc.CachedProperty)
		{
			Parts.Add(FString::Printf(TEXT("%s = %s"), *Desc.Name.ToString(),
				*FormatValue(*Desc.CachedProperty, Desc.CachedProperty->ContainerPtrToValuePtr<void>(Memory))));
		}
	}

	return FString::Join(Parts, TEXT(", "));
}
