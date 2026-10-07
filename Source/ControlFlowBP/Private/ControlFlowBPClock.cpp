#include "ControlFlowBPClock.h"

#include "Blueprint/UserWidget.h"
#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace UE::ControlFlowBP::Private
{
	struct FPendingWait
	{
		FControlFlowBPClock Clock;
		double Start = 0.0;
		double Seconds = 0.0;
		TFunction<void()> Callback;
	};

	static TArray<TSharedRef<FPendingWait>> GWaits;
	static FTSTicker::FDelegateHandle GWaitTicker;

	static bool TickWaits(float)
	{
		TArray<TSharedRef<FPendingWait>> Due;
		GWaits.RemoveAll([&Due](const TSharedRef<FPendingWait>& Wait) -> bool
		{
			if (!Wait->Clock.IsLive())
			{
				return true;
			}

			const double Elapsed = Wait->Clock.Now() - Wait->Start;
			if (Elapsed > 0.0 && Elapsed >= Wait->Seconds)
			{
				Due.Add(Wait);
				return true;
			}

			return false;
		});

		for (const TSharedRef<FPendingWait>& Wait : Due)
		{
			if (Wait->Callback)
			{
				const TFunction<void()> Callback = MoveTemp(Wait->Callback);
				Callback();
			}
		}

		if (GWaits.IsEmpty())
		{
			GWaitTicker.Reset();
			return false;
		}

		return true;
	}

	static TSharedRef<FPendingWait> AddWait(const FControlFlowBPClock& Clock, float Seconds, TFunction<void()>&& Callback)
	{
		const TSharedRef<FPendingWait> Wait = MakeShared<FPendingWait>();
		Wait->Clock = Clock;
		Wait->Start = Clock.Now();
		Wait->Seconds = FMath::Max(0.f, Seconds);
		Wait->Callback = MoveTemp(Callback);
		GWaits.Add(Wait);

		if (!GWaitTicker.IsValid())
		{
			GWaitTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickWaits));
		}

		return Wait;
	}
}

FControlFlowBPClock::FControlFlowBPClock(const UObject* Owner)
{
	UWorld* OwnerWorld = Owner ? Owner->GetWorld() : nullptr;
	if (!OwnerWorld || !OwnerWorld->IsGameWorld())
	{
		return;
	}

	World = OwnerWorld;
	bWorldTime = true;

	if (const AActor* Actor = Cast<AActor>(Owner))
	{
		bRunsWhilePaused = Actor->PrimaryActorTick.bTickEvenWhenPaused;
	}
	else if (const UActorComponent* Component = Cast<UActorComponent>(Owner))
	{
		bRunsWhilePaused = Component->PrimaryComponentTick.bTickEvenWhenPaused;
	}
	else
	{
		bRunsWhilePaused = Owner->IsA<UUserWidget>();
	}

	LastReading = Now();
}

double FControlFlowBPClock::Now() const
{
	if (!bWorldTime)
	{
		return FPlatformTime::Seconds();
	}

	if (const UWorld* ClockWorld = World.Get())
	{
		LastReading = bRunsWhilePaused ? ClockWorld->GetUnpausedTimeSeconds() : ClockWorld->GetTimeSeconds();
	}

	return LastReading;
}

bool FControlFlowBPClock::IsLive() const
{
	return !bWorldTime || World.IsValid();
}

void FControlFlowBPTimer::Start(const FControlFlowBPClock& Clock, float Seconds, TFunction<void()> Callback)
{
	Stop();
	Pending = UE::ControlFlowBP::Private::AddWait(Clock, Seconds, MoveTemp(Callback));
}

void FControlFlowBPTimer::Stop()
{
	if (Pending.IsValid())
	{
		Pending->Callback.Reset();
		UE::ControlFlowBP::Private::GWaits.Remove(Pending.ToSharedRef());
		Pending.Reset();
	}
}

bool FControlFlowBPTimer::IsActive() const
{
	return Pending.IsValid() && Pending->Callback;
}

void FControlFlowBPTimer::After(const FControlFlowBPClock& Clock, float Seconds, TFunction<void()> Callback)
{
	UE::ControlFlowBP::Private::AddWait(Clock, Seconds, MoveTemp(Callback));
}

void FControlFlowBPTimer::StopAll()
{
	using namespace UE::ControlFlowBP::Private;

	GWaits.Reset();

	if (GWaitTicker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GWaitTicker);
		GWaitTicker.Reset();
	}
}
