#include "ControlFlowBPDebug.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPModule.h"

#include "Blueprint/BlueprintExceptionInfo.h"
#include "DisplayDebugHelpers.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/HUD.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CoreMisc.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"

namespace UE::ControlFlowBP::Debug
{
	static int32 GTraceLevel = 0;
	static FAutoConsoleVariableRef CVarTrace(
		TEXT("ControlFlowBP.Trace"),
		GTraceLevel,
		TEXT("Logs control flow steps to LogControlFlowBP as they start and finish.\n")
		TEXT(" 0: off - Enable Step Trace still traces individual flows (default)\n")
		TEXT(" 1: every flow's steps\n")
		TEXT(" 2: also each step as it is queued, and steps that never ran"));

	static int32 GHistorySize = 256;
	static FAutoConsoleVariableRef CVarHistorySize(
		TEXT("ControlFlowBP.HistorySize"),
		GHistorySize,
		TEXT("How many finished steps each flow remembers for the Control Flow Debugger, Dump Flow and the editor's node bubbles. Default 256."));

	static int32 GRecentFlowCount = 32;
	static FAutoConsoleVariableRef CVarRecentFlows(
		TEXT("ControlFlowBP.RecentFlows"),
		GRecentFlowCount,
		TEXT("How many finished flows are kept for the Control Flow Debugger, the overlay and the editor once they end. Default 32."));

	static bool GbDumpOnFailure = true;
	static FAutoConsoleVariableRef CVarDumpOnFailure(
		TEXT("ControlFlowBP.DumpOnFailure"),
		GbDumpOnFailure,
		TEXT("Dump a flow to the log when one of its steps times out or hits Max Iterations, or aborts the flow. Default on."));

	static bool GbScreenErrors = true;
	static FAutoConsoleVariableRef CVarScreenErrors(
		TEXT("ControlFlowBP.ScreenErrors"),
		GbScreenErrors,
		TEXT("Also print control flow errors on screen. Default on. Never in Shipping."));

	static FString GBreakOnStep;
	static FAutoConsoleVariableRef CVarBreakOnStep(
		TEXT("ControlFlowBP.BreakOnStep"),
		GBreakOnStep,
		TEXT("Pause in the Blueprint debugger when a step whose path matches this wildcard is about to run its event,\n")
		TEXT("e.g. \"Encounter.Waves#*.Spawn*\". Separate several patterns with ';'. Empty turns it off."));

	static constexpr int32 MaxHistoryDumped = 32;

	static constexpr double OverlayLingerSeconds = 5.0;

	static TArray<TWeakPtr<FControlFlowBPFlowRecord>> GLiveFlows;
	static TArray<TSharedRef<FControlFlowBPFlowRecord>> GRecentFlows;
	static int32 GNextFlowId = 1;
	static uint64 GChangeCount = 0;

	static void LogLine(ELogVerbosity::Type Verbosity, const FString& Line)
	{
		switch (Verbosity & ELogVerbosity::VerbosityMask)
		{
		case ELogVerbosity::Fatal:
		case ELogVerbosity::Error:
			UE_LOG(LogControlFlowBP, Error, TEXT("%s"), *Line);
			break;
		case ELogVerbosity::Warning:
			UE_LOG(LogControlFlowBP, Warning, TEXT("%s"), *Line);
			break;
		case ELogVerbosity::Display:
			UE_LOG(LogControlFlowBP, Display, TEXT("%s"), *Line);
			break;
		case ELogVerbosity::Log:
			UE_LOG(LogControlFlowBP, Log, TEXT("%s"), *Line);
			break;
		default:
			UE_LOG(LogControlFlowBP, Verbose, TEXT("%s"), *Line);
			break;
		}
	}

	static void ShowOnScreen(ELogVerbosity::Type Verbosity, const FString& Message)
	{
#if !UE_BUILD_SHIPPING
		if (GbScreenErrors && Verbosity <= ELogVerbosity::Error && GEngine)
		{
			GEngine->AddOnScreenDebugMessage(INDEX_NONE, 10.f, FColor(255, 90, 80), FString::Printf(TEXT("Control Flow: %s"), *Message));
		}
#endif
	}

	static void ForEachLane(const FControlFlowBPLane& Lane, TFunctionRef<void(const FControlFlowBPLane&)> Visitor)
	{
		Visitor(Lane);

		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Lane.Entries)
		{
			for (const TSharedRef<FControlFlowBPLane>& Child : Step->ChildLanes)
			{
				ForEachLane(*Child, Visitor);
			}
		}
	}

	static void CollectActive(const FControlFlowBPLane& Lane, TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, bool bLeavesOnly)
	{
		const TSharedPtr<FControlFlowBPStepRecord> Running = Lane.GetRunningStep();
		if (!Running.IsValid())
		{
			return;
		}

		if (!bLeavesOnly)
		{
			OutSteps.Add(Running.ToSharedRef());
		}

		const int32 NumBeforeChildren = OutSteps.Num();
		for (const TSharedRef<FControlFlowBPLane>& Child : Running->ChildLanes)
		{
			CollectActive(*Child, OutSteps, bLeavesOnly);
		}

		if (bLeavesOnly && OutSteps.Num() == NumBeforeChildren)
		{
			OutSteps.Add(Running.ToSharedRef());
		}
	}

	static void CollectPending(const FControlFlowBPLane& Lane, TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, int32 MaxCount)
	{
		if (const TSharedPtr<FControlFlowBPStepRecord> Running = Lane.GetRunningStep())
		{
			for (const TSharedRef<FControlFlowBPLane>& Child : Running->ChildLanes)
			{
				CollectPending(*Child, OutSteps, MaxCount);
			}
		}

		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Lane.Entries)
		{
			if (OutSteps.Num() >= MaxCount)
			{
				return;
			}

			if (!Step->IsInternal() && Step->State == EControlFlowBPStepState::Pending)
			{
				OutSteps.Add(Step);
			}
		}
	}

	static FString DescribeSteps(const TArray<TSharedRef<FControlFlowBPStepRecord>>& Steps)
	{
		FString Text;
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Steps)
		{
			Text += FString::Printf(TEXT("%s%s (%s)"), Text.IsEmpty() ? TEXT("") : TEXT(", "), *Step->Path, *FControlFlowBPDebug::FormatSeconds(Step->GetElapsed()));
		}
		return Text;
	}

	static bool FlowMatchesFilter(const FControlFlowBPFlowRecord& Flow, const FString& Filter)
	{
		if (Filter.IsEmpty())
		{
			return true;
		}

		FString IdText = Filter;
		IdText.RemoveFromStart(TEXT("#"));
		if (IdText.IsNumeric())
		{
			return Flow.Id == FCString::Atoi(*IdText);
		}

		if (Filter.Contains(TEXT("*")) || Filter.Contains(TEXT("?")))
		{
			return Flow.Name.MatchesWildcard(Filter);
		}

		return Flow.Name.Contains(Filter);
	}

	static void ListFlows(const TArray<FString>& Args, FOutputDevice& Ar)
	{
		TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
		FControlFlowBPDebug::GetFlows(Flows);

		Ar.Logf(TEXT("%d control flow(s). ControlFlowBP.Dump <name or #id> for details."), Flows.Num());
		for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
		{
			Ar.Logf(TEXT("  #%d %s [%s] - %s"), Flow->Id, *Flow->Name, *Flow->OwnerName, *Flow->DescribeState());

			TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
			Flow->GetActiveSteps(Active, true);
			for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
			{
				Ar.Logf(TEXT("      at %s - %s"), *Step->Path, *Step->DescribeState());
			}
		}
	}

	static void DumpFlows(const TArray<FString>& Args, FOutputDevice& Ar)
	{
		const FString Filter = Args.Num() > 0 ? Args[0] : FString();

		TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
		FControlFlowBPDebug::GetFlows(Flows);

		int32 NumDumped = 0;
		for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
		{
			if (FlowMatchesFilter(*Flow, Filter))
			{
				TArray<FString> Lines;
				Flow->GetDumpLines(Lines);
				for (const FString& Line : Lines)
				{
					Ar.Log(Line);
				}
				++NumDumped;
			}
		}

		if (NumDumped == 0)
		{
			Ar.Logf(TEXT("No control flow matches '%s'. ControlFlowBP.List shows them all."), *Filter);
		}
	}

	static void CancelFlows(const TArray<FString>& Args, FOutputDevice& Ar)
	{
		if (Args.Num() == 0)
		{
			Ar.Log(TEXT("Usage: ControlFlowBP.Cancel <name, wildcard or #id>"));
			return;
		}

		TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
		FControlFlowBPDebug::GetFlows(Flows);

		for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
		{
			if (!Flow->IsFinished() && FlowMatchesFilter(*Flow, Args[0]))
			{
				if (UControlFlowBP* Builder = Flow->Builder.Get())
				{
					Ar.Logf(TEXT("Cancelling #%d %s."), Flow->Id, *Flow->Name);
					Builder->CancelWithReason(EControlFlowBPCancelCause::Requested, TEXT("cancelled from the console (ControlFlowBP.Cancel)"));
				}
			}
		}
	}

	static FAutoConsoleCommand CmdList(
		TEXT("ControlFlowBP.List"),
		TEXT("Lists control flows - running, and recently finished - with the step each one is on."),
		FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&ListFlows));

	static FAutoConsoleCommand CmdDump(
		TEXT("ControlFlowBP.Dump"),
		TEXT("ControlFlowBP.Dump [name, wildcard or #id]: the running steps, queued steps and recent history of matching flows (all when omitted)."),
		FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&DumpFlows));

	static FAutoConsoleCommand CmdCancel(
		TEXT("ControlFlowBP.Cancel"),
		TEXT("ControlFlowBP.Cancel <name, wildcard or #id>: cancels matching running flows."),
		FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&CancelFlows));
}

FControlFlowBPCallSite FControlFlowBPCallSite::Capture()
{
	FControlFlowBPCallSite Site;

#if DO_BLUEPRINT_GUARD
	if (!IsInGameThread())
	{
		return Site;
	}

	const FBlueprintContextTracker* Tracker = FBlueprintContextTracker::TryGet();
	if (!Tracker)
	{
		return Site;
	}

	const TArrayView<const FFrame* const> ScriptStack = Tracker->GetCurrentScriptStack();
	if (ScriptStack.Num() == 0)
	{
		return Site;
	}

	const FFrame* Frame = ScriptStack.Last();
	if (!Frame || !Frame->Node || !Frame->Code || Frame->Node->Script.Num() == 0)
	{
		return Site;
	}

	const int64 Offset = Frame->Code - Frame->Node->Script.GetData() - 1;
	if (Offset < 0 || Offset >= Frame->Node->Script.Num())
	{
		return Site;
	}

	Site.Context = Frame->Object;
	Site.Function = Frame->Node;
	Site.CodeOffset = static_cast<int32>(Offset);
#endif

	return Site;
}

FString FControlFlowBPCallSite::Describe() const
{
	const UFunction* ResolvedFunction = Function.Get();
	if (!IsSet() || !ResolvedFunction)
	{
		return FString();
	}

	FString ClassName = GetNameSafe(ResolvedFunction->GetOwnerClass());
	ClassName.RemoveFromEnd(TEXT("_C"));

	FString FunctionName = ResolvedFunction->GetName();
	if (FunctionName.StartsWith(TEXT("ExecuteUbergraph")))
	{
		FunctionName = TEXT("EventGraph");
	}

	return FString::Printf(TEXT("%s / %s"), *ClassName, *FunctionName);
}

double FControlFlowBPStepRecord::GetElapsed() const
{
	if (StartTime <= 0.0)
	{
		return 0.0;
	}

	double End = EndTime;
	if (!IsFinished())
	{
		const TSharedPtr<FControlFlowBPFlowRecord> PinnedFlow = Flow.Pin();
		End = PinnedFlow.IsValid() ? PinnedFlow->GetTime() : StartTime;
	}

	return FMath::Max(0.0, End - StartTime);
}

FString FControlFlowBPStepRecord::DescribeState() const
{
	const FString Elapsed = FControlFlowBPDebug::FormatSeconds(GetElapsed());
	const FString DetailSuffix = Detail.IsEmpty() ? FString() : FString::Printf(TEXT(": %s"), *Detail);

	switch (State)
	{
	case EControlFlowBPStepState::Pending:
		return TEXT("queued");

	case EControlFlowBPStepState::Running:
		switch (Type)
		{
		case EControlFlowBPStepType::Wait:
			if (TimeoutSeconds > 0.f)
			{
				return FString::Printf(TEXT("waiting %s, times out in %s"), *Elapsed,
					*FControlFlowBPDebug::FormatSeconds(TimeoutSeconds - GetElapsed()));
			}
			return FString::Printf(TEXT("waiting %s"), *Elapsed);

		case EControlFlowBPStepType::Delay:
			return FString::Printf(TEXT("delaying %s of %s"), *Elapsed, *Info);

		case EControlFlowBPStepType::Loop:
			return FString::Printf(TEXT("iteration %d, %s"), Iterations, *Elapsed);

		default:
			break;
		}
		return Detail.IsEmpty()
			? FString::Printf(TEXT("running %s"), *Elapsed)
			: FString::Printf(TEXT("running %s, %s"), *Elapsed, *Detail);

	case EControlFlowBPStepState::Succeeded:
		return FString::Printf(TEXT("done in %s%s"), *Elapsed, *DetailSuffix);

	case EControlFlowBPStepState::Failed:
		return FString::Printf(TEXT("FAILED after %s%s"), *Elapsed, *DetailSuffix);

	case EControlFlowBPStepState::Cancelled:
		return StartTime > 0.0
			? FString::Printf(TEXT("cancelled after %s%s"), *Elapsed, *DetailSuffix)
			: FString::Printf(TEXT("cancelled before it started%s"), *DetailSuffix);

	case EControlFlowBPStepState::Skipped:
		return FString::Printf(TEXT("skipped%s"), *DetailSuffix);

	default:
		return FString();
	}
}

const TCHAR* FControlFlowBPStepRecord::LexType(EControlFlowBPStepType InType)
{
	switch (InType)
	{
	case EControlFlowBPStepType::Function: return TEXT("Function");
	case EControlFlowBPStepType::Wait:     return TEXT("Wait");
	case EControlFlowBPStepType::Delay:    return TEXT("Delay");
	case EControlFlowBPStepType::SubFlow:  return TEXT("Sub Flow");
	case EControlFlowBPStepType::Branch:   return TEXT("Switch");
	case EControlFlowBPStepType::Fork:     return TEXT("Parallel");
	case EControlFlowBPStepType::Loop:     return TEXT("Loop");
	case EControlFlowBPStepType::If:       return TEXT("If");
	case EControlFlowBPStepType::Race:     return TEXT("Race");
	case EControlFlowBPStepType::Setting:  return TEXT("Setting");
	case EControlFlowBPStepType::Internal: return TEXT("Internal");
	default:                               return TEXT("?");
	}
}

const TCHAR* FControlFlowBPStepRecord::LexState(EControlFlowBPStepState InState)
{
	switch (InState)
	{
	case EControlFlowBPStepState::Pending:   return TEXT("queued");
	case EControlFlowBPStepState::Running:   return TEXT("running");
	case EControlFlowBPStepState::Succeeded: return TEXT("done");
	case EControlFlowBPStepState::Failed:    return TEXT("FAILED");
	case EControlFlowBPStepState::Cancelled: return TEXT("cancelled");
	case EControlFlowBPStepState::Skipped:   return TEXT("skipped");
	default:                                 return TEXT("?");
	}
}

bool FControlFlowBPStepRecord::RunsFlows(EControlFlowBPStepType InType)
{
	return InType == EControlFlowBPStepType::SubFlow
		|| InType == EControlFlowBPStepType::Branch
		|| InType == EControlFlowBPStepType::If
		|| InType == EControlFlowBPStepType::Fork
		|| InType == EControlFlowBPStepType::Race
		|| InType == EControlFlowBPStepType::Loop;
}

TSharedPtr<FControlFlowBPStepRecord> FControlFlowBPLane::GetRunningStep() const
{
	if (Entries.Num() > 0
		&& Entries[0]->State == EControlFlowBPStepState::Running
		&& !Entries[0]->IsInternal())
	{
		return Entries[0];
	}

	return nullptr;
}

FControlFlowBPFlowRecord::FControlFlowBPFlowRecord(const FString& InName, UControlFlowBP* InBuilder, UObject* InOwner, const FControlFlowBPClock& InClock)
	: Id(UE::ControlFlowBP::Debug::GNextFlowId++)
	, Name(InName)
	, Builder(InBuilder)
	, Owner(InOwner)
	, OwnerName(InOwner ? InOwner->GetName() : FString(TEXT("no owner")))
	, World(InOwner ? InOwner->GetWorld() : nullptr)
	, CreateTime(FPlatformTime::Seconds())
	, RootLane(MakeShared<FControlFlowBPLane>())
	, Clock(InClock)
	, ClockAtCreate(InClock.Now())
{
	RootLane->Path = InName;
}

double FControlFlowBPFlowRecord::GetTime() const
{
	return CreateTime + (Clock.Now() - ClockAtCreate);
}

double FControlFlowBPFlowRecord::GetElapsed() const
{
	switch (State)
	{
	case EControlFlowBPFlowState::Building:
		return GetTime() - CreateTime;

	case EControlFlowBPFlowState::Running:
		return GetTime() - StartTime;

	default:
		return EndTime - (StartTime > 0.0 ? StartTime : CreateTime);
	}
}

void FControlFlowBPFlowRecord::GetActiveSteps(TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, bool bLeavesOnly) const
{
	UE::ControlFlowBP::Debug::CollectActive(*RootLane, OutSteps, bLeavesOnly);
}

void FControlFlowBPFlowRecord::GetPendingSteps(TArray<TSharedRef<FControlFlowBPStepRecord>>& OutSteps, int32 MaxCount) const
{
	UE::ControlFlowBP::Debug::CollectPending(*RootLane, OutSteps, MaxCount);
}

void FControlFlowBPFlowRecord::ForEachStep(TFunctionRef<void(const TSharedRef<FControlFlowBPStepRecord>&)> Visitor) const
{
	UE::ControlFlowBP::Debug::ForEachLane(*RootLane, [&Visitor](const FControlFlowBPLane& Lane) -> void
	{
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Lane.Entries)
		{
			Visitor(Step);
		}
	});

	for (const TSharedRef<FControlFlowBPStepRecord>& Step : History)
	{
		Visitor(Step);
	}
}

FString FControlFlowBPFlowRecord::DescribeState() const
{
	const FString Elapsed = FControlFlowBPDebug::FormatSeconds(GetElapsed());

	switch (State)
	{
	case EControlFlowBPFlowState::Building:
		return FString::Printf(TEXT("built %s ago, Execute Flow not called yet"), *Elapsed);

	case EControlFlowBPFlowState::Running:
		return FString::Printf(TEXT("running %s"), *Elapsed);

	case EControlFlowBPFlowState::Completed:
		return FString::Printf(TEXT("completed in %s"), *Elapsed);

	case EControlFlowBPFlowState::Cancelled:
		return CancelCause == EControlFlowBPCancelCause::NeverExecuted
			? CancelReason
			: FString::Printf(TEXT("cancelled after %s: %s"), *Elapsed, *CancelReason);

	default:
		return FString();
	}
}

void FControlFlowBPFlowRecord::GetDumpLines(TArray<FString>& OutLines) const
{
	OutLines.Add(FString::Printf(TEXT("--- Control Flow '%s' (#%d) - %s ---"), *Name, Id, *DescribeState()));
	OutLines.Add(FString::Printf(TEXT("  owner: %s"), *OwnerName));

	if (CreateSite.IsSet())
	{
		OutLines.Add(FString::Printf(TEXT("  created at: %s"), *CreateSite.Describe()));
	}

	if (ExecuteSite.IsSet())
	{
		OutLines.Add(FString::Printf(TEXT("  executed at: %s"), *ExecuteSite.Describe()));
	}

	if (!StepFailurePolicy.IsEmpty())
	{
		OutLines.Add(FString::Printf(TEXT("  when a step fails: %s"), *StepFailurePolicy));
	}

	const UControlFlowBP* LiveBuilder = Builder.Get();
	const FString Variables = IsFinished() ? FinalVariables : (LiveBuilder ? LiveBuilder->DescribeFlowVariables() : FString());
	if (!Variables.IsEmpty())
	{
		OutLines.Add(FString::Printf(TEXT("  variables: %s"), *Variables));
	}

	TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
	GetActiveSteps(Active, false);
	if (Active.Num() > 0)
	{
		OutLines.Add(TEXT("  running:"));
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
		{
			OutLines.Add(FString::Printf(TEXT("    %s%s (%s) - %s%s"),
				*FString::ChrN(Step->Depth * 2, TEXT(' ')),
				*Step->Path,
				FControlFlowBPStepRecord::LexType(Step->Type),
				*Step->DescribeState(),
				Step->QueueSite.IsSet() ? *FString::Printf(TEXT("  [queued at %s]"), *Step->QueueSite.Describe()) : TEXT("")));
		}
	}

	constexpr int32 MaxPendingShown = 10;
	TArray<TSharedRef<FControlFlowBPStepRecord>> Pending;
	GetPendingSteps(Pending, MaxPendingShown + 1);
	if (Pending.Num() > 0)
	{
		OutLines.Add(TEXT("  queued next:"));
		for (int32 Index = 0; Index < FMath::Min(Pending.Num(), MaxPendingShown); ++Index)
		{
			OutLines.Add(FString::Printf(TEXT("    %s (%s)"), *Pending[Index]->Path, FControlFlowBPStepRecord::LexType(Pending[Index]->Type)));
		}

		if (Pending.Num() > MaxPendingShown)
		{
			OutLines.Add(TEXT("    ..."));
		}
	}

	if (History.Num() > 0)
	{
		using UE::ControlFlowBP::Debug::MaxHistoryDumped;
		const int32 FirstShown = FMath::Max(0, History.Num() - MaxHistoryDumped);
		const int32 NumEarlier = FirstShown + NumForgotten;

		OutLines.Add(FString::Printf(TEXT("  history (last %d, oldest first%s):"),
			History.Num() - FirstShown,
			NumEarlier > 0 ? *FString::Printf(TEXT("; %d earlier step(s) not shown"), NumEarlier) : TEXT("")));

		for (int32 Index = FirstShown; Index < History.Num(); ++Index)
		{
			OutLines.Add(FString::Printf(TEXT("    %s - %s"), *History[Index]->Path, *History[Index]->DescribeState()));
		}
	}

	if (Active.Num() == 0 && Pending.Num() == 0 && History.Num() == 0)
	{
		OutLines.Add(TEXT("  (no steps)"));
	}
}

void FControlFlowBPFlowRecord::Dump(ELogVerbosity::Type Verbosity, const FString& Heading) const
{
	if (!Heading.IsEmpty())
	{
		UE::ControlFlowBP::Debug::LogLine(Verbosity, Heading);
	}

	TArray<FString> Lines;
	GetDumpLines(Lines);
	for (const FString& Line : Lines)
	{
		UE::ControlFlowBP::Debug::LogLine(Verbosity, Line);
	}
}

bool FControlFlowBPFlowRecord::IsTracing() const
{
	return bTraceSteps || FControlFlowBPDebug::IsTraceEnabled(1);
}

TSharedRef<FControlFlowBPStepRecord> FControlFlowBPFlowRecord::AddStep(
	FControlFlowBPLane& Lane,
	EControlFlowBPStepType Type,
	const FString& StepName,
	const FString& StepPath,
	const UObject* HandlerObject,
	FName HandlerFunction)
{
	TSharedRef<FControlFlowBPStepRecord> Step = MakeShared<FControlFlowBPStepRecord>();
	Step->Name = StepName;
	Step->Path = StepPath;
	Step->Type = Type;
	Step->Depth = Lane.Depth;
	Step->QueuedTime = GetTime();
	Step->HandlerObject = const_cast<UObject*>(HandlerObject);
	Step->HandlerFunction = HandlerFunction;
	Step->Flow = AsShared();
	Step->Sequence = NextSequence++;

	if (const TSharedPtr<FControlFlowBPStepRecord> OwnerStep = Lane.OwnerStep.Pin())
	{
		Step->Parent = OwnerStep;

		FString RelativePath = Lane.Path;
		if (RelativePath.RemoveFromStart(OwnerStep->Path))
		{
			RelativePath.RemoveFromStart(TEXT("."));
			Step->Group = RelativePath;
		}
	}

	if (!Step->IsInternal())
	{
		Step->QueueSite = FControlFlowBPCallSite::Capture();
	}

	Lane.Entries.Add(Step);
	++UE::ControlFlowBP::Debug::GChangeCount;

	if (!Step->IsInternal() && FControlFlowBPDebug::IsTraceEnabled(2))
	{
		Trace(ELogVerbosity::Log, Step->Depth, FString::Printf(TEXT("queued    %s (%s)%s"),
			*Step->Path,
			FControlFlowBPStepRecord::LexType(Type),
			Step->QueueSite.IsSet() ? *FString::Printf(TEXT(" from %s"), *Step->QueueSite.Describe()) : TEXT("")));
	}

	return Step;
}

TSharedRef<FControlFlowBPLane> FControlFlowBPFlowRecord::AddChildLane(const TSharedRef<FControlFlowBPStepRecord>& OwnerStep, const FString& LanePath)
{
	TSharedRef<FControlFlowBPLane> Lane = MakeShared<FControlFlowBPLane>();
	Lane->Path = LanePath;
	Lane->Depth = OwnerStep->Depth + 1;
	Lane->OwnerStep = OwnerStep;

	OwnerStep->ChildLanes.Add(Lane);
	return Lane;
}

void FControlFlowBPFlowRecord::NotifyFlowStarted(const FControlFlowBPCallSite& InExecuteSite)
{
	State = EControlFlowBPFlowState::Running;
	StartTime = GetTime();
	ExecuteSite = InExecuteSite;
	++UE::ControlFlowBP::Debug::GChangeCount;

	RegionId = TRACE_BEGIN_REGION_WITH_ID(*FString::Printf(TEXT("Control Flow: %s"), *Name), TEXT("ControlFlowBP"));

	if (IsTracing())
	{
		int32 NumQueued = 0;
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : RootLane->Entries)
		{
			NumQueued += Step->IsInternal() ? 0 : 1;
		}

		Trace(ELogVerbosity::Log, 0, FString::Printf(TEXT("flow started, %d step(s) queued%s"),
			NumQueued,
			ExecuteSite.IsSet() ? *FString::Printf(TEXT(", from %s"), *ExecuteSite.Describe()) : TEXT("")));
	}
}

void FControlFlowBPFlowRecord::NotifyStepStarted(FControlFlowBPStepRecord& Step)
{
	if (Step.State != EControlFlowBPStepState::Pending)
	{
		return;
	}

	Step.State = EControlFlowBPStepState::Running;
	Step.StartTime = GetTime();
	++UE::ControlFlowBP::Debug::GChangeCount;

	if (Step.IsInternal())
	{
		return;
	}

	Step.RegionId = TRACE_BEGIN_REGION_WITH_ID(*Step.Path, TEXT("ControlFlowBP"));

	if (IsTracing())
	{
		FString Line = FString::Printf(TEXT("start     %s (%s"), *Step.Path, FControlFlowBPStepRecord::LexType(Step.Type));
		if (!Step.Info.IsEmpty())
		{
			Line += TEXT(", ") + Step.Info;
		}
		Line += TEXT(")");

		Trace(ELogVerbosity::Log, Step.Depth, Line);
	}
}

TSharedPtr<FControlFlowBPStepRecord> FControlFlowBPFlowRecord::NotifyNodeCompleted(FControlFlowBPLane& Lane)
{
	if (Lane.Entries.IsEmpty())
	{
		WarnLostTrack(Lane, TEXT("a node completed that was never queued through this wrapper"));
		return nullptr;
	}

	const TSharedRef<FControlFlowBPStepRecord> Step = Lane.Entries[0];
	Lane.Entries.RemoveAt(0);

	if (Step->IsFinished())
	{
		return nullptr;
	}

	if (Step->IsInternal())
	{
		FinishStep(Step, EControlFlowBPStepState::Succeeded);
		return Step;
	}

	if (Step->State == EControlFlowBPStepState::Pending)
	{
		FinishStep(Step, EControlFlowBPStepState::Skipped, TEXT("its node completed without running"));
		return Step;
	}

	const bool bForkCarriesOn = Step->Type == EControlFlowBPStepType::Fork || Step->Type == EControlFlowBPStepType::Race;
	if (Step->bCancelRequested || (Step->bChildCancelled && !bForkCarriesOn))
	{
		FinishStep(Step, EControlFlowBPStepState::Cancelled, TEXT("Set Cancelled Step As Complete is on, so the flow carried on"));
	}
	else if (Step->bFailed)
	{
		FinishStep(Step, EControlFlowBPStepState::Failed);
	}
	else if (Step->bHandlerMissing)
	{
		FinishStep(Step, EControlFlowBPStepState::Skipped);
	}
	else
	{
		FinishStep(Step, EControlFlowBPStepState::Succeeded,
			Step->Type == EControlFlowBPStepType::Loop ? FString::Printf(TEXT("%d iteration(s)"), Step->Iterations) : FString());
	}

	return Step;
}

void FControlFlowBPFlowRecord::NotifyLaneCancelled(FControlFlowBPLane& Lane)
{
	const FString Why =
		!InterruptReason.IsEmpty() ? InterruptReason
		: !CancelReason.IsEmpty() ? CancelReason
		: FString(TEXT("cancelled"));

	SweepLane(Lane, EControlFlowBPStepState::Cancelled, EControlFlowBPStepState::Cancelled, Why);

	if (const TSharedPtr<FControlFlowBPStepRecord> OwnerStep = Lane.OwnerStep.Pin())
	{
		OwnerStep->bChildCancelled = true;

		if (OwnerStep->Type == EControlFlowBPStepType::Fork && !OwnerStep->IsFinished())
		{
			const FString Note = FString::Printf(TEXT("track '%s' was cancelled"), *Lane.Path);
			OwnerStep->Detail = OwnerStep->Detail.IsEmpty() ? Note : OwnerStep->Detail + TEXT("; ") + Note;
		}
	}
}

void FControlFlowBPFlowRecord::NotifyLaneCompleted(FControlFlowBPLane& Lane)
{
	bool bAnyLeft = false;
	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Lane.Entries)
	{
		bAnyLeft |= !Step->IsFinished();
	}

	if (bAnyLeft)
	{
		WarnLostTrack(Lane, TEXT("its flow completed with steps still queued"));
		SweepLane(Lane, EControlFlowBPStepState::Skipped, EControlFlowBPStepState::Skipped, TEXT("its flow completed without reaching it"));
	}
}

void FControlFlowBPFlowRecord::SkipLane(FControlFlowBPLane& Lane, const FString& Why)
{
	SweepLane(Lane, EControlFlowBPStepState::Skipped, EControlFlowBPStepState::Skipped, Why);
}

void FControlFlowBPFlowRecord::MarkRunningStepsCancelRequested()
{
	TArray<TSharedRef<FControlFlowBPStepRecord>> Leaves;
	GetActiveSteps(Leaves, true);
	CancelledAt = UE::ControlFlowBP::Debug::DescribeSteps(Leaves);

	UE::ControlFlowBP::Debug::ForEachLane(*RootLane, [](const FControlFlowBPLane& Lane) -> void
	{
		if (const TSharedPtr<FControlFlowBPStepRecord> Running = Lane.GetRunningStep())
		{
			Running->bCancelRequested = true;
		}
	});
}

void FControlFlowBPFlowRecord::SetCancelReason(EControlFlowBPCancelCause Cause, const FString& Reason)
{
	if (CancelCause == EControlFlowBPCancelCause::None && !IsFinished())
	{
		CancelCause = Cause;
		CancelReason = Reason;
	}
}

void FControlFlowBPFlowRecord::NotifyFlowFinished(bool bCancelled)
{
	if (IsFinished())
	{
		return;
	}

	TArray<TSharedRef<FControlFlowBPStepRecord>> WasAt;
	GetActiveSteps(WasAt, true);

	if (bCancelled && CancelCause == EControlFlowBPCancelCause::None)
	{
		CancelCause = InterruptCause != EControlFlowBPCancelCause::None ? InterruptCause : EControlFlowBPCancelCause::Requested;
		CancelReason = !InterruptReason.IsEmpty() ? InterruptReason : FString(TEXT("cancelled"));
	}

	if (bCancelled)
	{
		SweepLane(*RootLane, EControlFlowBPStepState::Cancelled, EControlFlowBPStepState::Cancelled, CancelReason);
	}
	else
	{
		SweepLane(*RootLane, EControlFlowBPStepState::Skipped, EControlFlowBPStepState::Skipped, TEXT("the flow completed without reaching it"));
	}

	State = bCancelled ? EControlFlowBPFlowState::Cancelled : EControlFlowBPFlowState::Completed;
	EndTime = GetTime();
	++UE::ControlFlowBP::Debug::GChangeCount;

	if (RegionId != 0)
	{
		TRACE_END_REGION_WITH_ID(RegionId);
		RegionId = 0;
	}

	if (bCancelled)
	{
		TRACE_BOOKMARK(TEXT("Control flow '%s' cancelled: %s"), *Name, *CancelReason);

		const FString WasAtText = WasAt.Num() > 0 ? UE::ControlFlowBP::Debug::DescribeSteps(WasAt) : CancelledAt;

		const FString Summary = FString::Printf(TEXT("Flow '%s' cancelled after %s: %s.%s"),
			*Name,
			*FControlFlowBPDebug::FormatSeconds(GetElapsed()),
			*CancelReason,
			WasAtText.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" It was at: %s."), *WasAtText));

		const ELogVerbosity::Type Verbosity =
			CancelCause == EControlFlowBPCancelCause::Aborted ? ELogVerbosity::Warning
			: CancelCause == EControlFlowBPCancelCause::WorldTornDown || CancelCause == EControlFlowBPCancelCause::NeverExecuted ? ELogVerbosity::Verbose
			: ELogVerbosity::Log;

		UE::ControlFlowBP::Debug::LogLine(Verbosity, Summary);

		if (CancelCause == EControlFlowBPCancelCause::Aborted && FControlFlowBPDebug::ShouldDumpOnFailure())
		{
			Dump(ELogVerbosity::Warning);
		}
	}
	else if (IsTracing())
	{
		Trace(ELogVerbosity::Log, 0, TEXT("flow completed"));
	}

	FControlFlowBPDebug::RetainFinishedFlow(AsShared());
}

void FControlFlowBPFlowRecord::TraceStepDetail(const FControlFlowBPStepRecord& Step, const FString& Text) const
{
	if (IsTracing())
	{
		Trace(ELogVerbosity::Log, Step.Depth, FString::Printf(TEXT("          %s: %s"), *Step.Path, *Text));
	}
}

void FControlFlowBPFlowRecord::FinishStep(const TSharedRef<FControlFlowBPStepRecord>& Step, EControlFlowBPStepState NewState, const FString& NewDetail)
{
	if (Step->IsFinished())
	{
		return;
	}

	Step->State = NewState;
	Step->EndTime = GetTime();
	++UE::ControlFlowBP::Debug::GChangeCount;

	if (!NewDetail.IsEmpty())
	{
		Step->Detail = Step->Detail.IsEmpty() ? NewDetail : Step->Detail + TEXT("; ") + NewDetail;
	}

	if (Step->RegionId != 0)
	{
		TRACE_END_REGION_WITH_ID(Step->RegionId);
		Step->RegionId = 0;
	}

	const TArray<TSharedRef<FControlFlowBPLane>> Children = MoveTemp(Step->ChildLanes);
	Step->ChildLanes.Reset();
	for (const TSharedRef<FControlFlowBPLane>& Child : Children)
	{
		SweepLane(*Child, EControlFlowBPStepState::Cancelled, EControlFlowBPStepState::Cancelled, TEXT("the step running it ended"));
	}

	if (Step->IsInternal())
	{
		return;
	}

	History.Add(Step);
	const int32 MaxHistory = FControlFlowBPDebug::GetHistorySize();
	if (History.Num() > MaxHistory)
	{
		NumForgotten += History.Num() - MaxHistory;
		History.RemoveAt(0, History.Num() - MaxHistory);
	}

	const bool bNeverStarted = Step->StartTime <= 0.0;
	if (bNeverStarted ? FControlFlowBPDebug::IsTraceEnabled(2) : IsTracing())
	{
		const ELogVerbosity::Type Verbosity = NewState == EControlFlowBPStepState::Failed ? ELogVerbosity::Warning : ELogVerbosity::Log;
		Trace(Verbosity, Step->Depth, FString::Printf(TEXT("end       %s - %s"), *Step->Path, *Step->DescribeState()));
	}
}

void FControlFlowBPFlowRecord::SweepLane(FControlFlowBPLane& Lane, EControlFlowBPStepState RunningState, EControlFlowBPStepState PendingState, const FString& Why)
{
	const TArray<TSharedRef<FControlFlowBPStepRecord>> Entries = MoveTemp(Lane.Entries);
	Lane.Entries.Reset();

	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Entries)
	{
		if (Step->IsFinished())
		{
			continue;
		}

		const bool bWasRunning = Step->State == EControlFlowBPStepState::Running;

		if (bWasRunning && Step->bFailed)
		{
			FinishStep(Step, EControlFlowBPStepState::Failed);
			continue;
		}

		FinishStep(Step,
			bWasRunning ? RunningState : PendingState,
			bWasRunning ? Why : FString::Printf(TEXT("not reached - %s"), *Why));
	}
}

void FControlFlowBPFlowRecord::Trace(ELogVerbosity::Type Verbosity, int32 Depth, const FString& Text) const
{
	UE::ControlFlowBP::Debug::LogLine(Verbosity, FString::Printf(TEXT("[%s +%.3fs] %s%s"),
		*Name,
		GetElapsed(),
		*FString::ChrN(FMath::Max(0, Depth) * 2, TEXT(' ')),
		*Text));
}

void FControlFlowBPFlowRecord::WarnLostTrack(const FControlFlowBPLane& Lane, const TCHAR* What)
{
	if (bWarnedLostTrack)
	{
		return;
	}

	bWarnedLostTrack = true;
	UE_LOG(LogControlFlowBP, Warning,
		TEXT("Control flow debugging lost track of '%s' (%s). The flow itself is unaffected, but the step states shown for '%s' may be wrong from here on."),
		*Lane.Path, What, *Name);
}

FControlFlowBPDebug::FOnStepHandlerStarting FControlFlowBPDebug::OnStepHandlerStarting;
FControlFlowBPDebug::FOnIssue FControlFlowBPDebug::OnIssue;

void FControlFlowBPDebug::RegisterFlow(const TSharedRef<FControlFlowBPFlowRecord>& Flow)
{
	using namespace UE::ControlFlowBP::Debug;

	GLiveFlows.RemoveAll([](const TWeakPtr<FControlFlowBPFlowRecord>& Weak) { return !Weak.IsValid(); });
	GLiveFlows.Add(Flow);
	++GChangeCount;
}

void FControlFlowBPDebug::RetainFinishedFlow(const TSharedRef<FControlFlowBPFlowRecord>& Flow)
{
	using namespace UE::ControlFlowBP::Debug;

	GRecentFlows.Remove(Flow);
	GRecentFlows.Add(Flow);
	++GChangeCount;

	const int32 MaxRecentFlows = FMath::Max(0, GRecentFlowCount);
	if (GRecentFlows.Num() > MaxRecentFlows)
	{
		GRecentFlows.RemoveAt(0, GRecentFlows.Num() - MaxRecentFlows);
	}
}

void FControlFlowBPDebug::ClearFinishedFlows()
{
	using namespace UE::ControlFlowBP::Debug;

	GRecentFlows.Reset();
	++GChangeCount;
}

uint64 FControlFlowBPDebug::GetChangeCount()
{
	return UE::ControlFlowBP::Debug::GChangeCount;
}

void FControlFlowBPDebug::GetFlows(TArray<TSharedRef<FControlFlowBPFlowRecord>>& OutFlows)
{
	using namespace UE::ControlFlowBP::Debug;

	GLiveFlows.RemoveAll([](const TWeakPtr<FControlFlowBPFlowRecord>& Weak) { return !Weak.IsValid(); });

	for (const TWeakPtr<FControlFlowBPFlowRecord>& Weak : GLiveFlows)
	{
		if (const TSharedPtr<FControlFlowBPFlowRecord> Flow = Weak.Pin(); Flow.IsValid() && !Flow->IsFinished())
		{
			OutFlows.Add(Flow.ToSharedRef());
		}
	}

	OutFlows.Append(GRecentFlows);
}

bool FControlFlowBPDebug::IsTraceEnabled(int32 Level)
{
	return UE::ControlFlowBP::Debug::GTraceLevel >= Level;
}

int32 FControlFlowBPDebug::GetHistorySize()
{
	return FMath::Max(1, UE::ControlFlowBP::Debug::GHistorySize);
}

bool FControlFlowBPDebug::ShouldDumpOnFailure()
{
	return UE::ControlFlowBP::Debug::GbDumpOnFailure;
}

bool FControlFlowBPDebug::MatchesBreakPattern(const FString& StepPath)
{
	const FString& Patterns = UE::ControlFlowBP::Debug::GBreakOnStep;
	if (Patterns.IsEmpty())
	{
		return false;
	}

	TArray<FString> Split;
	Patterns.ParseIntoArray(Split, TEXT(";"), true);
	for (FString& Pattern : Split)
	{
		Pattern.TrimStartAndEndInline();
		if (!Pattern.IsEmpty() && StepPath.MatchesWildcard(Pattern))
		{
			return true;
		}
	}

	return false;
}

void FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPStepRecord* Step)
{
	using namespace UE::ControlFlowBP::Debug;

	const FControlFlowBPCallSite Site = FControlFlowBPCallSite::Capture();

	LogLine(Verbosity, Site.IsSet() ? FString::Printf(TEXT("%s [called from %s]"), *Message, *Site.Describe()) : Message);
	ShowOnScreen(Verbosity, Message);

#if WITH_EDITOR && DO_BLUEPRINT_GUARD
	if (Site.IsSet() && Verbosity <= ELogVerbosity::Error)
	{
		const TArrayView<FFrame* const> ScriptStack = FBlueprintContextTracker::Get().GetCurrentScriptStackWritable();
		if (ScriptStack.Num() > 0 && ScriptStack.Last())
		{
			FFrame& Frame = *ScriptStack.Last();
			FBlueprintCoreDelegates::ThrowScriptException(Frame.Object, Frame,
				FBlueprintExceptionInfo(EBlueprintExceptionType::UserRaisedError, FText::FromString(Message)));
			return;
		}
	}
#endif

	OnIssue.Broadcast(Verbosity, Message, Site.IsSet() ? Site : (Step ? Step->QueueSite : FControlFlowBPCallSite()), Step);
}

void FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Type Verbosity, const FString& Message, const FControlFlowBPStepRecord& Step)
{
	using namespace UE::ControlFlowBP::Debug;

	LogLine(Verbosity, Step.QueueSite.IsSet() ? FString::Printf(TEXT("%s [queued at %s]"), *Message, *Step.QueueSite.Describe()) : Message);
	ShowOnScreen(Verbosity, Message);

	OnIssue.Broadcast(Verbosity, Message, Step.QueueSite, &Step);
}

void FControlFlowBPDebug::ReportNeverExecuted(FControlFlowBPFlowRecord& Flow)
{
	using namespace UE::ControlFlowBP::Debug;

	if (Flow.State != EControlFlowBPFlowState::Building)
	{
		return;
	}

	int32 NumSteps = 0;
	Flow.ForEachStep([&NumSteps](const TSharedRef<FControlFlowBPStepRecord>& Step) -> void
	{
		NumSteps += Step->IsInternal() ? 0 : 1;
	});

	if (NumSteps == 0)
	{
		return;
	}

	const FString Message = FString::Printf(
		TEXT("Control flow '%s' (owner %s) had %d step(s) queued but was never executed. Call Execute Flow or Execute Flow Async on it."),
		*Flow.Name, *Flow.OwnerName, NumSteps);

	LogLine(ELogVerbosity::Warning, Flow.CreateSite.IsSet() ? FString::Printf(TEXT("%s [created at %s]"), *Message, *Flow.CreateSite.Describe()) : Message);
	OnIssue.Broadcast(ELogVerbosity::Warning, Message, Flow.CreateSite, nullptr);

	Flow.SetCancelReason(EControlFlowBPCancelCause::NeverExecuted, TEXT("never executed - Execute Flow was not called"));
	Flow.NotifyFlowFinished(true);
}

void FControlFlowBPDebug::NotifyHandlerStarting(const FControlFlowBPStepRecord& Step)
{
	const bool bMatches = MatchesBreakPattern(Step.Path);

	if (OnStepHandlerStarting.IsBound())
	{
		OnStepHandlerStarting.Broadcast(Step, bMatches);
	}
	else if (bMatches)
	{
		UE_LOG(LogControlFlowBP, Warning, TEXT("Break On Step: '%s' is about to run '%s'."), *Step.Path, *Step.HandlerFunction.ToString());

		if (FPlatformMisc::IsDebuggerPresent())
		{
			UE_DEBUG_BREAK();
		}
	}
}

FString FControlFlowBPDebug::FormatSeconds(double Seconds)
{
	Seconds = FMath::Max(0.0, Seconds);

	if (Seconds < 1.0)
	{
		return FString::Printf(TEXT("%d ms"), FMath::RoundToInt(Seconds * 1000.0));
	}

	if (Seconds < 60.0)
	{
		return FString::Printf(TEXT("%.2f s"), Seconds);
	}

	const int32 Minutes = FMath::FloorToInt(Seconds / 60.0);
	return FString::Printf(TEXT("%d m %02d s"), Minutes, FMath::FloorToInt(Seconds - Minutes * 60.0));
}

void FControlFlowBPDebug::DrawDebugHUD(AHUD* HUD, UCanvas* Canvas, const FDebugDisplayInfo& DisplayInfo, float& YL, float& YPos)
{
	using namespace UE::ControlFlowBP::Debug;

	static const FName NAME_ControlFlow(TEXT("ControlFlow"));
	if (!Canvas || !DisplayInfo.IsDisplayOn(NAME_ControlFlow))
	{
		return;
	}

	const UWorld* HUDWorld = HUD ? HUD->GetWorld() : nullptr;

	TArray<TSharedRef<FControlFlowBPFlowRecord>> Flows;
	GetFlows(Flows);

	Flows.RemoveAll([HUDWorld](const TSharedRef<FControlFlowBPFlowRecord>& Flow) -> bool
	{
		const bool bOtherWorld = !Flow->World.IsExplicitlyNull() && Flow->World.Get() != HUDWorld;
		const bool bLongFinished = Flow->IsFinished() && Flow->GetTime() - Flow->EndTime > OverlayLingerSeconds;
		return bOtherWorld || bLongFinished;
	});

	FDisplayDebugManager& Display = Canvas->DisplayDebugManager;

	int32 NumRunning = 0;
	for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
	{
		NumRunning += Flow->State == EControlFlowBPFlowState::Running ? 1 : 0;
	}

	Display.SetDrawColor(FColor::Yellow);
	Display.DrawString(FString::Printf(TEXT("CONTROL FLOWS - %d running   (ControlFlowBP.Dump <name> for history)"), NumRunning));

	if (Flows.IsEmpty())
	{
		Display.SetDrawColor(FColor(160, 160, 160));
		Display.DrawString(TEXT("  none in this world"), 4.f);
		return;
	}

	const auto StepColor = [](const FControlFlowBPStepRecord& Step) -> FColor
	{
		switch (Step.State)
		{
		case EControlFlowBPStepState::Running:   return Step.Type == EControlFlowBPStepType::Wait ? FColor(110, 200, 255) : FColor(120, 230, 120);
		case EControlFlowBPStepState::Failed:    return FColor(255, 90, 80);
		case EControlFlowBPStepState::Cancelled: return FColor(255, 170, 60);
		case EControlFlowBPStepState::Pending:
		case EControlFlowBPStepState::Skipped:   return FColor(150, 150, 150);
		default:                                 return FColor(200, 200, 200);
		}
	};

	for (const TSharedRef<FControlFlowBPFlowRecord>& Flow : Flows)
	{
		Display.SetDrawColor(
			Flow->State == EControlFlowBPFlowState::Running ? FColor::White
			: Flow->State == EControlFlowBPFlowState::Cancelled ? FColor(255, 170, 60)
			: FColor(170, 170, 170));
		Display.DrawString(FString::Printf(TEXT("#%d %s  [%s]  %s"), Flow->Id, *Flow->Name, *Flow->OwnerName, *Flow->DescribeState()), 4.f);

		TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
		Flow->GetActiveSteps(Active, false);
		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
		{
			Display.SetDrawColor(StepColor(*Step));
			Display.DrawString(FString::Printf(TEXT("%s (%s) - %s"), *Step->Path, FControlFlowBPStepRecord::LexType(Step->Type), *Step->DescribeState()),
				16.f + 12.f * Step->Depth);
		}

		constexpr int32 MaxNextShown = 3;
		TArray<TSharedRef<FControlFlowBPStepRecord>> Pending;
		Flow->GetPendingSteps(Pending, MaxNextShown + 1);
		if (Pending.Num() > 0)
		{
			FString NextText;
			for (int32 Index = 0; Index < FMath::Min(Pending.Num(), MaxNextShown); ++Index)
			{
				NextText += (Index > 0 ? TEXT(", ") : TEXT("")) + Pending[Index]->Name;
			}

			Display.SetDrawColor(FColor(150, 150, 150));
			Display.DrawString(FString::Printf(TEXT("next: %s%s"), *NextText, Pending.Num() > MaxNextShown ? TEXT(", ...") : TEXT("")), 16.f);
		}

		if (Flow->History.Num() > 0)
		{
			const FControlFlowBPStepRecord& Last = *Flow->History.Last();
			Display.SetDrawColor(StepColor(Last));
			Display.DrawString(FString::Printf(TEXT("last: %s - %s"), *Last.Path, *Last.DescribeState()), 16.f);
		}
	}
}
