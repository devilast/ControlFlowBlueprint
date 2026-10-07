#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "KismetNodes/SGraphNodeK2Default.h"
#include "KismetNodes/SGraphNodeK2Event.h"

class SGraphNodeControlFlow : public SGraphNodeK2Default
{
public:
	virtual void GetNodeInfoPopups(FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups) const override;
};

/** The same for a custom event, which the engine draws with SGraphNodeK2Event. */
class SGraphNodeControlFlowEvent : public SGraphNodeK2Event
{
public:
	virtual void GetNodeInfoPopups(FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& OutPopups) const override;
};

class FControlFlowBPNodeFactory : public FGraphPanelNodeFactory
{
public:
	virtual TSharedPtr<class SGraphNode> CreateNode(class UEdGraphNode* Node) const override;
};
