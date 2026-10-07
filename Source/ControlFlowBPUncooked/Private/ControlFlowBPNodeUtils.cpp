#include "ControlFlowBPNodeUtils.h"

#include "BlueprintEventNodeSpawner.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CustomEvent.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/UnrealType.h"

namespace UE::ControlFlowBP::NodeUtils
{
	static void GetBlueprintHierarchy(const UBlueprint* Blueprint, TArray<const UBlueprint*>& OutHierarchy)
	{
		for (const UBlueprint* It = Blueprint; It; It = UBlueprint::GetBlueprintFromClass(It->ParentClass))
		{
			OutHierarchy.Add(It);
		}
	}

	const UClass* GetSearchClass(const UBlueprint* Blueprint)
	{
		if (!Blueprint)
		{
			return nullptr;
		}

		return Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass.Get() : Blueprint->GeneratedClass.Get();
	}

	bool IsClassOrChildOf(const UClass* Class, const UStruct* Scope)
	{
		if (!Scope)
		{
			return true;
		}

		const UClass* ScopeClass = Cast<UClass>(Scope);
		if (!Class || !ScopeClass)
		{
			return false;
		}

		const UBlueprint* ScopeBlueprint = UBlueprint::GetBlueprintFromClass(ScopeClass);
		for (const UClass* It = Class; It; It = It->GetSuperClass())
		{
			if (It == ScopeClass || (ScopeBlueprint && UBlueprint::GetBlueprintFromClass(It) == ScopeBlueprint))
			{
				return true;
			}
		}

		return false;
	}

	FGuid FindFunctionGuid(const UBlueprint* Blueprint, FName FunctionName)
	{
		if (FunctionName.IsNone())
		{
			return FGuid();
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(Blueprint, Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (const UEdGraph* Graph : It->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FunctionName)
				{
					return Graph->GraphGuid;
				}
			}

			TArray<UK2Node_CustomEvent*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(It, Events);
			for (const UK2Node_CustomEvent* Event : Events)
			{
				if (Event && Event->CustomFunctionName == FunctionName)
				{
					return Event->NodeGuid;
				}
			}
		}

		return FGuid();
	}

	FName FindFunctionNameByGuid(const UBlueprint* Blueprint, const FGuid& Guid)
	{
		if (!Guid.IsValid())
		{
			return NAME_None;
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(Blueprint, Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (const UEdGraph* Graph : It->FunctionGraphs)
			{
				if (Graph && Graph->GraphGuid == Guid)
				{
					return Graph->GetFName();
				}
			}

			TArray<UK2Node_CustomEvent*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(It, Events);
			for (const UK2Node_CustomEvent* Event : Events)
			{
				if (Event && Event->NodeGuid == Guid)
				{
					return Event->CustomFunctionName;
				}
			}
		}

		return NAME_None;
	}

	UFunction* ResolveFunction(const UBlueprint* Blueprint, FName Name, const FGuid& Guid, FName* OutResolvedName)
	{
		const UClass* Class = GetSearchClass(Blueprint);
		UFunction* Function = (Class && !Name.IsNone()) ? Class->FindFunctionByName(Name) : nullptr;

		if (Class && !Function && Guid.IsValid())
		{
			const FName Renamed = FindFunctionNameByGuid(Blueprint, Guid);
			if (!Renamed.IsNone())
			{
				Name = Renamed;
				Function = Class->FindFunctionByName(Renamed);
			}
		}

		if (OutResolvedName)
		{
			*OutResolvedName = Name;
		}

		return Function;
	}

	UObject* FindFunctionDefinition(const UBlueprint* Blueprint, FName FunctionName)
	{
		if (FunctionName.IsNone())
		{
			return nullptr;
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(Blueprint, Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (UEdGraph* Graph : It->FunctionGraphs)
			{
				if (Graph && Graph->GetFName() == FunctionName && !Graph->HasAnyFlags(RF_Transient))
				{
					return Graph;
				}
			}

			for (const FBPInterfaceDescription& Interface : It->ImplementedInterfaces)
			{
				for (UEdGraph* Graph : Interface.Graphs)
				{
					if (Graph && Graph->GetFName() == FunctionName && !Graph->HasAnyFlags(RF_Transient))
					{
						return Graph;
					}
				}
			}

			TArray<UK2Node_CustomEvent*> Events;
			FBlueprintEditorUtils::GetAllNodesOfClass(It, Events);
			for (UK2Node_CustomEvent* Event : Events)
			{
				if (Event && Event->CustomFunctionName == FunctionName)
				{
					return Event;
				}
			}
		}

		return nullptr;
	}

	bool IsPickableFunction(const UFunction* Function, const UClass* SelfClass)
	{
		if (!Function || !UEdGraphSchema_K2::CanUserKismetCallFunction(Function))
		{
			return false;
		}

		if (Function->HasAnyFunctionFlags(FUNC_Delegate | FUNC_Static))
		{
			return false;
		}

		if (Function->GetFName() == UEdGraphSchema_K2::FN_UserConstructionScript
			|| Function->GetName().StartsWith(UEdGraphSchema_K2::FN_ExecuteUbergraphBase.ToString()))
		{
			return false;
		}

		const UClass* Owner = Function->GetOwnerClass();
		if (!UBlueprint::GetBlueprintFromClass(Owner))
		{
			return false;
		}

		const UFunction* Root = Function;
		while (const UFunction* SuperFunction = Root->GetSuperFunction())
		{
			Root = SuperFunction;
		}

		if (Root != Function && !UBlueprint::GetBlueprintFromClass(Root->GetOwnerClass()))
		{
			return false;
		}

		if (Function->HasAnyFunctionFlags(FUNC_Private) && Owner != SelfClass)
		{
			return false;
		}

		return true;
	}

	UK2Node_CustomEvent* AddMatchingEvent(UBlueprint* Blueprint, const UFunction* Signature, const FString& BaseName)
	{
		UEdGraph* EventGraph = Blueprint ? FBlueprintEditorUtils::FindEventGraph(Blueprint) : nullptr;
		if (!EventGraph || !Signature)
		{
			return nullptr;
		}

		Blueprint->Modify();
		EventGraph->Modify();

		const FName EventName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, BaseName);

		UBlueprintEventNodeSpawner* Spawner = UBlueprintEventNodeSpawner::Create(UK2Node_CustomEvent::StaticClass(), EventName);
		const FVector2f SpawnPosition = EventGraph->GetGoodPlaceForNewNode();
		UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Spawner->Invoke(EventGraph, IBlueprintNodeBinder::FBindingSet(), FVector2D(SpawnPosition)));
		if (!Event)
		{
			return nullptr;
		}

		Event->SetDelegateSignature(Signature);
		Event->ReconstructNode();
		Event->bIsEditable = true;
		return Event;
	}

	UEdGraph* AddMatchingFunction(UBlueprint* Blueprint, UFunction* Signature, const FString& BaseName, const UEdGraph* SourceGraph)
	{
		if (!Blueprint || !Signature || !SourceGraph)
		{
			return nullptr;
		}

		Blueprint->Modify();

		const FName FunctionName = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, BaseName);
		const UEdGraphSchema* SourceSchema = SourceGraph->GetSchema();
		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, FunctionName, SourceGraph->GetClass(),
			SourceSchema ? SourceSchema->GetClass() : UEdGraphSchema_K2::StaticClass());
		if (!Graph)
		{
			return nullptr;
		}

		FBlueprintEditorUtils::AddFunctionGraph<UFunction>(Blueprint, Graph, true, Signature);
		return Graph;
	}

	TArray<FName> GetEventDispatchers(const UClass* Class)
	{
		TArray<FName> Names;
		if (!Class)
		{
			return Names;
		}

		for (TFieldIterator<FMulticastDelegateProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (UEdGraphSchema_K2::CanUserKismetAccessVariable(*It, Class, UEdGraphSchema_K2::MustBeDelegate))
			{
				Names.AddUnique(It->GetFName());
			}
		}

		Names.Sort(FNameLexicalLess());
		return Names;
	}

	FGuid FindDispatcherGuid(const UClass* Class, FName DispatcherName)
	{
		if (DispatcherName.IsNone())
		{
			return FGuid();
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(UBlueprint::GetBlueprintFromClass(Class), Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (const FBPVariableDescription& Variable : It->NewVariables)
			{
				if (Variable.VarName == DispatcherName && Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					return Variable.VarGuid;
				}
			}
		}

		return FGuid();
	}

	FName FindDispatcherNameByGuid(const UClass* Class, const FGuid& Guid)
	{
		if (!Guid.IsValid())
		{
			return NAME_None;
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(UBlueprint::GetBlueprintFromClass(Class), Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (const FBPVariableDescription& Variable : It->NewVariables)
			{
				if (Variable.VarGuid == Guid && Variable.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				{
					return Variable.VarName;
				}
			}
		}

		return NAME_None;
	}

	UEdGraph* FindDispatcherGraph(const UClass* Class, FName DispatcherName)
	{
		if (DispatcherName.IsNone())
		{
			return nullptr;
		}

		TArray<const UBlueprint*> Hierarchy;
		GetBlueprintHierarchy(UBlueprint::GetBlueprintFromClass(Class), Hierarchy);

		for (const UBlueprint* It : Hierarchy)
		{
			for (UEdGraph* Graph : It->DelegateSignatureGraphs)
			{
				if (Graph && Graph->GetFName() == DispatcherName)
				{
					return Graph;
				}
			}
		}

		return nullptr;
	}

	FName AddEventDispatcher(UBlueprint* Blueprint, const FString& BaseName)
	{
		if (!Blueprint)
		{
			return NAME_None;
		}

		const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
		const FName Name = FBlueprintEditorUtils::FindUniqueKismetName(Blueprint, BaseName);

		Blueprint->Modify();

		FEdGraphPinType DelegateType;
		DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
		if (!FBlueprintEditorUtils::AddMemberVariable(Blueprint, Name, DelegateType))
		{
			return NAME_None;
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(Blueprint, Name, UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!Graph)
		{
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, Name);
			return NAME_None;
		}

		Graph->bEditable = false;
		Schema->CreateDefaultNodesForGraph(*Graph);
		Schema->CreateFunctionGraphTerminators(*Graph, static_cast<UClass*>(nullptr));
		Schema->AddExtraFunctionFlags(Graph, FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public);
		Schema->MarkFunctionEntryAsEditable(Graph, true);

		Blueprint->DelegateSignatureGraphs.Add(Graph);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		return Name;
	}
}
