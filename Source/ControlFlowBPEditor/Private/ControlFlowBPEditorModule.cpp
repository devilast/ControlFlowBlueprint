#include "BlueprintCompilationManager.h"
#include "ControlFlowBPCompilerChecks.h"
#include "ControlFlowBPEditorDebugger.h"
#include "ControlFlowFunctionPicker.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Engine/Blueprint.h"
#include "Modules/ModuleManager.h"
#include "SControlFlowBPDebugger.h"
#include "SGraphNodeControlFlow.h"
#include "SGraphPinControlFlowStepFunction.h"

/** Swaps the function pins of Queue Step and Queue If for the filtered function dropdown. */
class FControlFlowBPPinFactory : public FGraphPanelPinFactory
{
public:
	virtual TSharedPtr<SGraphPin> CreatePin(UEdGraphPin* Pin) const override
	{
		if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Name)
		{
			return nullptr;
		}

		const IControlFlowFunctionPicker* Picker = Cast<IControlFlowFunctionPicker>(Pin->GetOwningNodeUnchecked());
		if (Picker && Picker->IsFunctionPin(*Pin))
		{
			return SNew(SGraphPinControlFlowStepFunction, Pin);
		}

		return nullptr;
	}
};

class FControlFlowBPEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		PinFactory = MakeShared<FControlFlowBPPinFactory>();
		FEdGraphUtilities::RegisterVisualPinFactory(PinFactory);

		NodeFactory = MakeShared<FControlFlowBPNodeFactory>();
		FEdGraphUtilities::RegisterVisualNodeFactory(NodeFactory);

		FControlFlowBPEditorDebugger::Startup();
		SControlFlowBPDebugger::RegisterTabSpawner();

		FBlueprintCompilationManager::RegisterCompilerExtension(UBlueprint::StaticClass(), GetMutableDefault<UControlFlowBPCompilerChecks>());
	}

	virtual void ShutdownModule() override
	{
		SControlFlowBPDebugger::UnregisterTabSpawner();
		FControlFlowBPEditorDebugger::Shutdown();

		if (NodeFactory.IsValid())
		{
			FEdGraphUtilities::UnregisterVisualNodeFactory(NodeFactory);
			NodeFactory.Reset();
		}

		if (PinFactory.IsValid())
		{
			FEdGraphUtilities::UnregisterVisualPinFactory(PinFactory);
			PinFactory.Reset();
		}
	}

private:
	TSharedPtr<FControlFlowBPPinFactory> PinFactory;
	TSharedPtr<FControlFlowBPNodeFactory> NodeFactory;
};

IMPLEMENT_MODULE(FControlFlowBPEditorModule, ControlFlowBPEditor)
