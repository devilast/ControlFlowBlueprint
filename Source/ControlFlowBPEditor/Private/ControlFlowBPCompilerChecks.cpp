#include "ControlFlowBPCompilerChecks.h"

#include "ControlFlowBPScopes.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "KismetCompiler.h"

namespace UE::ControlFlowBP::CompilerChecks
{
	/** An object an event receives, and the calls on it that the event exists to make. */
	struct FRule
	{
		const UClass* ParamClass = nullptr;
		TArray<FName> ResolvingFunctions;
		const TCHAR* Message = nullptr;
	};

	static const TArray<FRule>& GetRules()
	{
		static const TArray<FRule> Rules =
		{
			{
				UControlFlowStepHandle::StaticClass(),
				{
					GET_FUNCTION_NAME_CHECKED(UControlFlowStepHandle, ContinueStep),
					GET_FUNCTION_NAME_CHECKED(UControlFlowStepHandle, FailStep),
					GET_FUNCTION_NAME_CHECKED(UControlFlowStepHandle, AbortFlow),
				},
				TEXT("@@ never calls Continue Step, Fail Step or Abort Flow From This Step on its @@, so its step waits until it times out.")
			},
			{
				UControlFlowBranchScope::StaticClass(),
				{
					GET_FUNCTION_NAME_CHECKED(UControlFlowBranchScope, SelectBranch),
					GET_FUNCTION_NAME_CHECKED(UControlFlowBranchScope, SelectCaseByName),
				},
				TEXT("@@ never calls Select Case or Select Case By Name on its @@, so its switch always runs key 0.")
			},
		};
		return Rules;
	}

	static bool IsResolvedOrHandedOff(const UEdGraphPin& Source, const FRule& Rule, TSet<const UEdGraphPin*>& Visited)
	{
		for (const UEdGraphPin* Linked : Source.LinkedTo)
		{
			if (!Linked || Visited.Contains(Linked))
			{
				continue;
			}
			Visited.Add(Linked);

			if (const UK2Node_Knot* Knot = Cast<UK2Node_Knot>(Linked->GetOwningNode()))
			{
				if (Knot->GetOutputPin() && IsResolvedOrHandedOff(*Knot->GetOutputPin(), Rule, Visited))
				{
					return true;
				}
				continue;
			}

			const UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Linked->GetOwningNode());
			const UFunction* Function = Call ? Call->GetTargetFunction() : nullptr;
			if (!Function || Linked->PinName != UEdGraphSchema_K2::PN_Self || Rule.ResolvingFunctions.Contains(Function->GetFName()))
			{
				return true;
			}
		}

		return false;
	}

	static void CheckGraph(const UEdGraph& Graph, FCompilerResultsLog& MessageLog)
	{
		for (const UEdGraphNode* Node : Graph.Nodes)
		{
			const bool bIsEntry = Cast<UK2Node_Event>(Node) || Cast<UK2Node_FunctionEntry>(Node);
			if (!bIsEntry || !Node->IsNodeEnabled())
			{
				continue;
			}

			for (const UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Output || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Object || Pin->PinType.IsContainer())
				{
					continue;
				}

				for (const FRule& Rule : GetRules())
				{
					if (Pin->PinType.PinSubCategoryObject != Rule.ParamClass)
					{
						continue;
					}

					TSet<const UEdGraphPin*> Visited;
					bool bUsed = IsResolvedOrHandedOff(*Pin, Rule, Visited);

					if (!bUsed && Cast<UK2Node_FunctionEntry>(Node))
					{
						for (const UEdGraphNode* Other : Graph.Nodes)
						{
							const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Other);
							const UEdGraphPin* Value = Get && Get->GetVarName() == Pin->PinName ? Get->FindPin(Pin->PinName, EGPD_Output) : nullptr;
							if (Value && IsResolvedOrHandedOff(*Value, Rule, Visited))
							{
								bUsed = true;
								break;
							}
						}
					}

					if (!bUsed)
					{
						MessageLog.Warning(Rule.Message, Node, Pin);
					}
				}
			}
		}
	}
}

void UControlFlowBPCompilerChecks::ProcessBlueprintCompiled(const FKismetCompilerContext& CompilationContext, const FBlueprintCompiledData& Data)
{
	using namespace UE::ControlFlowBP::CompilerChecks;

	const UBlueprint* Blueprint = CompilationContext.Blueprint;
	if (!Blueprint)
	{
		return;
	}

	const auto CheckGraphs = [&CompilationContext](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> void
	{
		for (const UEdGraph* Graph : Graphs)
		{
			if (Graph)
			{
				CheckGraph(*Graph, CompilationContext.MessageLog);
			}
		}
	};

	CheckGraphs(Blueprint->UbergraphPages);
	CheckGraphs(Blueprint->FunctionGraphs);
	for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
	{
		CheckGraphs(Interface.Graphs);
	}
}
