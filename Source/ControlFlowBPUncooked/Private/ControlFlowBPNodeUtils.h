#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UEdGraph;
class UK2Node_CustomEvent;

namespace UE::ControlFlowBP::NodeUtils
{
	const UClass* GetSearchClass(const UBlueprint* Blueprint);
	bool IsClassOrChildOf(const UClass* Class, const UStruct* Scope);
	FGuid FindFunctionGuid(const UBlueprint* Blueprint, FName FunctionName);

	FName FindFunctionNameByGuid(const UBlueprint* Blueprint, const FGuid& Guid);
	UFunction* ResolveFunction(const UBlueprint* Blueprint, FName Name, const FGuid& Guid, FName* OutResolvedName = nullptr);
	UObject* FindFunctionDefinition(const UBlueprint* Blueprint, FName FunctionName);
	bool IsPickableFunction(const UFunction* Function, const UClass* SelfClass);
	UK2Node_CustomEvent* AddMatchingEvent(UBlueprint* Blueprint, const UFunction* Signature, const FString& BaseName);
	UEdGraph* AddMatchingFunction(UBlueprint* Blueprint, UFunction* Signature, const FString& BaseName, const UEdGraph* SourceGraph);
	TArray<FName> GetEventDispatchers(const UClass* Class);
	FGuid FindDispatcherGuid(const UClass* Class, FName DispatcherName);

	FName FindDispatcherNameByGuid(const UClass* Class, const FGuid& Guid);
	UEdGraph* FindDispatcherGraph(const UClass* Class, FName DispatcherName);
	FName AddEventDispatcher(UBlueprint* Blueprint, const FString& BaseName);
}
