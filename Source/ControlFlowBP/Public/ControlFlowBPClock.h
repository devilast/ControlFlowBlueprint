#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UWorld;

namespace UE::ControlFlowBP::Private
{
	struct FPendingWait;
}

/**
 * The clock a flow keeps time by: its delays, timeouts, Wait Until checks and loop iterations, and
 * every time the debugging tools show for it.
 *
 * A flow owned by something in a game world runs on that world's game time, like the owner's own
 * Delay nodes. It stands still while the game is paused - by Pause in Play In Editor too - and at a
 * Blueprint breakpoint, follows time dilation, and gains at most the world's Max Undilated Frame
 * Time per frame, however long a frame took. An owner that keeps ticking while the game is paused -
 * a widget, or an actor or component set to tick even when paused, such as a player controller -
 * keeps its flows running then too. Outside a game world, flows run on real time.
 */
class CONTROLFLOWBP_API FControlFlowBPClock
{
public:
	FControlFlowBPClock() = default;
	explicit FControlFlowBPClock(const UObject* Owner);
	double Now() const;
	bool IsLive() const;

private:
	TWeakObjectPtr<UWorld> World;
	mutable double LastReading = 0.0;
	bool bWorldTime = false;
	bool bRunsWhilePaused = false;
};

/**
 * Calls back once a flow's clock has gained the time asked for. Checked once per frame, so a wait of
 * 0 lasts until the clock next moves: the next frame, unless the game is paused.
 */
class CONTROLFLOWBP_API FControlFlowBPTimer
{
public:
	void Start(const FControlFlowBPClock& Clock, float Seconds, TFunction<void()> Callback);

	void Stop();
	bool IsActive() const;
	static void After(const FControlFlowBPClock& Clock, float Seconds, TFunction<void()> Callback);
	static void StopAll();

private:
	TSharedPtr<UE::ControlFlowBP::Private::FPendingWait> Pending;
};
