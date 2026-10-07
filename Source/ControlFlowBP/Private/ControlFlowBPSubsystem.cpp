#include "ControlFlowBPSubsystem.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPDebug.h"
#include "ControlFlowBPModule.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

void UControlFlowBPSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &UControlFlowBPSubsystem::HandleWorldCleanup);

	OwnerCheckHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UControlFlowBPSubsystem::CancelFlowsOfDestroyedOwners));
}

void UControlFlowBPSubsystem::Deinitialize()
{
	bShuttingDown = true;

	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}

	if (OwnerCheckHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(OwnerCheckHandle);
		OwnerCheckHandle.Reset();
	}

	Retained.Reset();
	NamedIndex.Reset();

	Super::Deinitialize();
}

UControlFlowBPSubsystem* UControlFlowBPSubsystem::Get()
{
	return GEngine ? GEngine->GetEngineSubsystem<UControlFlowBPSubsystem>() : nullptr;
}

void UControlFlowBPSubsystem::Retain(UControlFlowBP* Flow)
{
	if (Flow && !bShuttingDown)
	{
		Retained.Add(Flow);
	}
}

void UControlFlowBPSubsystem::ReleaseFlow(UControlFlowBP* Flow)
{
	if (Flow)
	{
		Retained.Remove(Flow);
		UnregisterNamed(Flow);
	}
}

UControlFlowBP* UControlFlowBPSubsystem::FindNamed(const UObject* Owner, const FString& FlowId) const
{
	if (!Owner)
	{
		return nullptr;
	}

	if (const TMap<FString, TWeakObjectPtr<UControlFlowBP>>* ByName = NamedIndex.Find(FObjectKey(Owner)))
	{
		if (const TWeakObjectPtr<UControlFlowBP>* Found = ByName->Find(FlowId))
		{
			return Found->Get();
		}
	}

	return nullptr;
}

void UControlFlowBPSubsystem::RegisterNamed(const UObject* Owner, const FString& FlowId, UControlFlowBP* Flow)
{
	if (!Owner || !Flow || bShuttingDown)
	{
		return;
	}

	NamedIndex.FindOrAdd(FObjectKey(Owner)).Add(FlowId, Flow);

	Retained.Add(Flow);
}

void UControlFlowBPSubsystem::UnregisterNamed(const UControlFlowBP* Flow)
{
	if (!Flow)
	{
		return;
	}

	for (auto OwnerIt = NamedIndex.CreateIterator(); OwnerIt; ++OwnerIt)
	{
		for (auto NameIt = OwnerIt.Value().CreateIterator(); NameIt; ++NameIt)
		{
			if (NameIt.Value().Get() == Flow)
			{
				NameIt.RemoveCurrent();
			}
		}

		if (OwnerIt.Value().IsEmpty())
		{
			OwnerIt.RemoveCurrent();
		}
	}
}

void UControlFlowBPSubsystem::HandleWorldCleanup(UWorld* World, bool, bool)
{
	if (bShuttingDown || !World)
	{
		return;
	}

	TArray<TObjectPtr<UControlFlowBP>> ToCancel;
	TArray<TObjectPtr<UControlFlowBP>> OwnerGone;
	for (const TObjectPtr<UControlFlowBP>& Flow : Retained)
	{
		if (!Flow)
		{
			continue;
		}

		if (Flow->IsOwnerStale())
		{
			OwnerGone.Add(Flow);
			continue;
		}

		if (const UObject* Owner = Flow->GetOwner())
		{
			if (Owner->GetWorld() == World)
			{
				ToCancel.Add(Flow);
			}
		}
	}

	for (const TArray<TObjectPtr<UControlFlowBP>>* Flows : { &OwnerGone, &ToCancel })
	{
		for (const TObjectPtr<UControlFlowBP>& Flow : *Flows)
		{
			if (FControlFlowBPFlowRecord* Record = !Flow->HasExecuted() ? Flow->GetDebugRecord() : nullptr)
			{
				FControlFlowBPDebug::ReportNeverExecuted(*Record);
			}
		}
	}

	for (const TObjectPtr<UControlFlowBP>& Flow : OwnerGone)
	{
		UE_LOG(LogControlFlowBP, Verbose, TEXT("Cancelling flow '%s': its owner was destroyed."), *Flow->GetFlowDebugName());
		Flow->HandleOwningWorldCleanup(EControlFlowBPCancelCause::OwnerDestroyed, TEXT("its owner was destroyed"));
	}

	const FString WorldReason = FString::Printf(TEXT("its world '%s' was torn down"), *World->GetName());
	for (const TObjectPtr<UControlFlowBP>& Flow : ToCancel)
	{
		UE_LOG(LogControlFlowBP, Verbose, TEXT("Cancelling flow '%s': its world is being torn down."), *Flow->GetFlowDebugName());
		Flow->HandleOwningWorldCleanup(EControlFlowBPCancelCause::WorldTornDown, WorldReason);
	}

	PruneStale();
}

bool UControlFlowBPSubsystem::CancelFlowsOfDestroyedOwners(float)
{
	if (bShuttingDown)
	{
		return true;
	}

	TArray<TObjectPtr<UControlFlowBP>> OwnerGone;
	for (const TObjectPtr<UControlFlowBP>& Flow : Retained)
	{
		if (Flow && !Flow->IsFinished() && Flow->IsOwnerStale())
		{
			OwnerGone.Add(Flow);
		}
	}

	for (const TObjectPtr<UControlFlowBP>& Flow : OwnerGone)
	{
		if (FControlFlowBPFlowRecord* Record = !Flow->HasExecuted() ? Flow->GetDebugRecord() : nullptr)
		{
			FControlFlowBPDebug::ReportNeverExecuted(*Record);
		}

		UE_LOG(LogControlFlowBP, Verbose, TEXT("Cancelling flow '%s': its owner was destroyed."), *Flow->GetFlowDebugName());
		Flow->HandleOwningWorldCleanup(EControlFlowBPCancelCause::OwnerDestroyed, TEXT("its owner was destroyed"));
	}

	return true;
}

void UControlFlowBPSubsystem::PruneStale()
{
	for (auto OwnerIt = NamedIndex.CreateIterator(); OwnerIt; ++OwnerIt)
	{
		if (OwnerIt.Key().ResolveObjectPtr() == nullptr)
		{
			OwnerIt.RemoveCurrent();
			continue;
		}

		for (auto NameIt = OwnerIt.Value().CreateIterator(); NameIt; ++NameIt)
		{
			if (!NameIt.Value().IsValid())
			{
				NameIt.RemoveCurrent();
			}
		}

		if (OwnerIt.Value().IsEmpty())
		{
			OwnerIt.RemoveCurrent();
		}
	}
}
