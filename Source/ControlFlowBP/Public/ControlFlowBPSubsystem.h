#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/EngineSubsystem.h"
#include "UObject/ObjectKey.h"
#include "ControlFlowBPSubsystem.generated.h"

class UControlFlowBP;
class UWorld;

/**
 * Keeps running flows alive, cancels them when their owner is destroyed, and owns the named-flow
 * registry.
 *
 * A flow parked in a Queue Wait step has nothing referencing it: the FControlFlow lives on the
 * heap, the dynamic delegates bound to it hold only weak object pointers, and a Blueprint local
 * goes out of scope the moment the queueing function returns. Without a hard reference here the
 * builder is collected, every step shim reports unbound, steps silently skip and loops stall.
 *
 * This is an engine subsystem rather than a game-instance one so that flows also work in editor
 * utilities and commandlets. The tradeoff is that nothing tears it down on PIE stop, so it hooks
 * FWorldDelegates::OnWorldCleanup itself.
 */
UCLASS()
class CONTROLFLOWBP_API UControlFlowBPSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static UControlFlowBPSubsystem* Get();
	void Retain(UControlFlowBP* Flow);
	void ReleaseFlow(UControlFlowBP* Flow);

	UControlFlowBP* FindNamed(const UObject* Owner, const FString& FlowId) const;
	void RegisterNamed(const UObject* Owner, const FString& FlowId, UControlFlowBP* Flow);
	void UnregisterNamed(const UControlFlowBP* Flow);

	bool IsShuttingDown() const { return bShuttingDown; }

private:
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	void PruneStale();
	bool CancelFlowsOfDestroyedOwners(float DeltaTime);

	UPROPERTY(Transient)
	TSet<TObjectPtr<UControlFlowBP>> Retained;

	TMap<FObjectKey, TMap<FString, TWeakObjectPtr<UControlFlowBP>>> NamedIndex;

	FDelegateHandle WorldCleanupHandle;
	FTSTicker::FDelegateHandle OwnerCheckHandle;
	bool bShuttingDown = false;
};
