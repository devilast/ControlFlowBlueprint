#include "SGraphNodeControlFlow.h"

#include "ControlFlowBPEditorDebugger.h"

#include "K2Node.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "KismetNodes/KismetNodeInfoContext.h"

namespace UE::ControlFlowBP::NodeWidgets
{
	static void AddControlFlowPopups(const UEdGraphNode* Node, FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups)
	{
		if (FControlFlowBPEditorDebugger* Debugger = FControlFlowBPEditorDebugger::Get())
		{
			Debugger->GetNodePopups(Node, static_cast<FKismetNodeInfoContext*>(Context), OutPopups);
		}
	}
}

void SGraphNodeControlFlow::GetNodeInfoPopups(FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups) const
{
	SGraphNodeK2Default::GetNodeInfoPopups(Context, OutPopups);
	UE::ControlFlowBP::NodeWidgets::AddControlFlowPopups(GraphNode, Context, OutPopups);
}

void SGraphNodeControlFlowEvent::GetNodeInfoPopups(FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups) const
{
	SGraphNodeK2Event::GetNodeInfoPopups(Context, OutPopups);
	UE::ControlFlowBP::NodeWidgets::AddControlFlowPopups(GraphNode, Context, OutPopups);
}

TSharedPtr<SGraphNode> FControlFlowBPNodeFactory::CreateNode(UEdGraphNode* Node) const
{
	if (FControlFlowBPEditorDebugger::IsQueueNode(Node, false) || FControlFlowBPEditorDebugger::IsFlowNode(Node))
	{
		return SNew(SGraphNodeControlFlow, CastChecked<UK2Node>(Node));
	}

	UK2Node_CustomEvent* Event = Cast<UK2Node_CustomEvent>(Node);
	if (Event && FControlFlowBPEditorDebugger::IsStepHandlerNode(Event))
	{
		return SNew(SGraphNodeControlFlowEvent, Event);
	}

	UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
	if (Entry && Entry->GetClass() == UK2Node_FunctionEntry::StaticClass() && FControlFlowBPEditorDebugger::IsStepHandlerNode(Entry))
	{
		return SNew(SGraphNodeControlFlow, Entry);
	}

	return nullptr;
}
