#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

CONTROLFLOWBP_API DECLARE_LOG_CATEGORY_EXTERN(LogControlFlowBP, Log, All);

class FControlFlowBPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FDelegateHandle ShowDebugHandle;
};
