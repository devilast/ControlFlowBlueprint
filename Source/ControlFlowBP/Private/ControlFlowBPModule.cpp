#include "ControlFlowBPModule.h"

#include "ControlFlowBPClock.h"
#include "ControlFlowBPDebug.h"
#include "GameFramework/HUD.h"

DEFINE_LOG_CATEGORY(LogControlFlowBP);

void FControlFlowBPModule::StartupModule()
{
#if !UE_BUILD_SHIPPING
	ShowDebugHandle = AHUD::OnShowDebugInfo.AddStatic(&FControlFlowBPDebug::DrawDebugHUD);
#endif
}

void FControlFlowBPModule::ShutdownModule()
{
	if (ShowDebugHandle.IsValid())
	{
		AHUD::OnShowDebugInfo.Remove(ShowDebugHandle);
		ShowDebugHandle.Reset();
	}

	FControlFlowBPTimer::StopAll();
}

IMPLEMENT_MODULE(FControlFlowBPModule, ControlFlowBP)
