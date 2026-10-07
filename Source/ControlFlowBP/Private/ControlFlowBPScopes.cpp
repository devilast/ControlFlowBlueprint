#include "ControlFlowBPScopes.h"

#include "ControlFlowBP.h"
#include "ControlFlowBPDebug.h"
#include "ControlFlowBPDispatcherListener.h"
#include "ControlFlowBPModule.h"
#include "ControlFlowBPPropertyUtils.h"

#include "ControlFlow.h"
#include "ControlFlowBranch.h"
#include "ControlFlowConcurrency.h"

void UControlFlowStepHandle::Init(const FControlFlowNodeRef& InNode, UControlFlowBP* InRoot, const FString& InStepPath, const FInstancedStruct& InPayload,
	const TSharedPtr<FControlFlowBPStepRecord>& InRecord, int32 InAttempt)
{
	Node = InNode;
	Root = InRoot;
	StepPath = InStepPath;
	Payload = InPayload;
	Record = InRecord;
	Attempt = FMath::Max(1, InAttempt);
	bConsumed = false;
}

UControlFlowBP* UControlFlowStepHandle::GetFlow() const
{
	return Root.Get();
}

TWeakPtr<FControlFlow> UControlFlowStepHandle::GetOwningFlow() const
{
	return Node.IsValid() ? Node->GetParent() : TWeakPtr<FControlFlow>();
}

bool UControlFlowStepHandle::IsCancelRequested() const
{
	return Node.IsValid() && Node->HasCancelBeenRequested();
}

bool UControlFlowStepHandle::IsStepValid() const
{
	return !bConsumed && Node.IsValid() && Node->GetParent().IsValid();
}

void UControlFlowStepHandle::ContinueStep()
{
	if (!TryConsume(TEXT("Continue Step")))
	{
		return;
	}

	const FControlFlowNodePtr LocalNode = Node;
	Node.Reset();

	if (UControlFlowBP* Owner = Root.Get())
	{
		Owner->Release(this);
	}

	LocalNode->ContinueFlow();
}

void UControlFlowStepHandle::FailStep(FString Reason)
{
	if (!TryConsume(TEXT("Fail Step")))
	{
		return;
	}

	HandleFailure(Reason.IsEmpty() ? FString(TEXT("no reason given")) : Reason);
}

void UControlFlowStepHandle::HandleFailure(const FString& Reason)
{
	const FControlFlowNodePtr LocalNode = Node;
	Node.Reset();

	UControlFlowBP* Owner = Root.Get();
	if (Owner)
	{
		Owner->Release(this);
	}

	const int32 MaxAttempts = Owner ? Owner->GetStepRetries() + 1 : 1;
	if (Owner && RestartStep && LocalNode.IsValid() && Attempt < MaxAttempts)
	{
		if (Record.IsValid())
		{
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning, FString::Printf(
				TEXT("Step '%s' failed (%s); retrying, attempt %d of %d."), *StepPath, *Reason, Attempt + 1, MaxAttempts), *Record);
		}

		const TFunction<void(const FControlFlowNodeRef&, int32, const FInstancedStruct&)> Restart = MoveTemp(RestartStep);
		Restart(LocalNode.ToSharedRef(), Attempt + 1, Payload);
		return;
	}

	const FString FinalReason = Attempt > 1 ? FString::Printf(TEXT("%s (after %d attempts)"), *Reason, Attempt) : Reason;
	if (Owner && Record.IsValid())
	{
		Owner->ReportStepFailed(*Record, FinalReason);
	}

	if (Owner && Owner->GetStepFailurePolicy() == EControlFlowFailurePolicy::StopFlow)
	{
		Owner->CancelWithReason(EControlFlowBPCancelCause::Aborted, FString::Printf(
			TEXT("step '%s' failed (%s) and the flow's Step Failure Policy is Stop Flow"), *StepPath, *FinalReason));
		return;
	}

	if (LocalNode.IsValid())
	{
		LocalNode->ContinueFlow();
	}
}

void UControlFlowStepHandle::AbortFlow()
{
	const FControlFlowBPCallSite Site = FControlFlowBPCallSite::Capture();
	AbortWithReason(EControlFlowBPCancelCause::Aborted, Site.IsSet()
		? FString::Printf(TEXT("Abort Flow From This Step at '%s' (called from %s)"), *StepPath, *Site.Describe())
		: FString::Printf(TEXT("Abort Flow From This Step at '%s'"), *StepPath));
}

void UControlFlowStepHandle::AbortWithReason(EControlFlowBPCancelCause Cause, const FString& Reason)
{
	if (!TryConsume(TEXT("Abort Flow From This Step")))
	{
		return;
	}

	const FControlFlowNodePtr LocalNode = Node;
	Node.Reset();

	UControlFlowBP* Owner = Root.Get();
	if (Owner)
	{
		Owner->Release(this);
	}

	if (Record.IsValid())
	{
		Record->bCancelRequested = true;
	}

	TSharedPtr<FControlFlowBPFlowRecord> Debug;
	if (FControlFlowBPFlowRecord* FlowRecord = Owner ? Owner->GetDebugRecord() : nullptr)
	{
		Debug = FlowRecord->AsShared();
	}

	const EControlFlowBPCancelCause PreviousCause = Debug.IsValid() ? Debug->InterruptCause : EControlFlowBPCancelCause::None;
	const FString PreviousReason = Debug.IsValid() ? Debug->InterruptReason : FString();
	if (Debug.IsValid())
	{
		Debug->InterruptCause = Cause;
		Debug->InterruptReason = Reason;
	}

	LocalNode->CancelFlow();

	if (Debug.IsValid())
	{
		Debug->InterruptCause = PreviousCause;
		Debug->InterruptReason = PreviousReason;
	}
}

void UControlFlowStepHandle::ArmWatchdog(float Seconds)
{
	const UControlFlowBP* Owner = Root.Get();
	if (Seconds <= 0.f || bConsumed || !Owner)
	{
		return;
	}

	TimeoutSeconds = Seconds;

	const TWeakObjectPtr<UControlFlowStepHandle> WeakThis(this);
	Watchdog.Start(Owner->GetClock(), Seconds, [WeakThis]() -> void
	{
		if (UControlFlowStepHandle* Handle = WeakThis.Get())
		{
			Handle->HandleTimeout();
		}
	});
}

void UControlFlowStepHandle::StartPolling(const FControlFlowCondition& Condition, float Interval)
{
	PollCondition = Condition;
	PollInterval = FMath::Max(0.f, Interval);

	Poll();
}

void UControlFlowStepHandle::Poll()
{
	if (!IsStepValid())
	{
		return;
	}

	if (!PollCondition.IsBound())
	{
		if (Record.IsValid())
		{
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Wait Until '%s': the Condition function no longer exists; continuing."), *StepPath), *Record);
		}

		ContinueStep();
		return;
	}

	UControlFlowBP* Owner = Root.Get();
	if (!Owner)
	{
		return;
	}

	bool bMet = false;
	{
		const FControlFlowBPCallbackScope Callback(*Owner);
		bMet = PollCondition.Execute();
	}

	if (!IsStepValid())
	{
		return;
	}

	if (bMet)
	{
		ContinueStep();
		return;
	}

	const TWeakObjectPtr<UControlFlowStepHandle> WeakThis(this);
	Poller.Start(Owner->GetClock(), PollInterval, [WeakThis]() -> void
	{
		if (UControlFlowStepHandle* Handle = WeakThis.Get())
		{
			Handle->Poll();
		}
	});
}

void UControlFlowStepHandle::SetListener(UObject* InListener)
{
	Listener = InListener;
}

void UControlFlowStepHandle::ForceInvalidate()
{
	StopWaiting();
	bConsumed = true;
	Node.Reset();
}

void UControlFlowStepHandle::StopWaiting()
{
	Watchdog.Stop();
	Poller.Stop();

	if (UControlFlowDispatcherListener* DispatcherListener = Cast<UControlFlowDispatcherListener>(Listener))
	{
		DispatcherListener->Unbind();
	}
	Listener = nullptr;
}

void UControlFlowStepHandle::HandleTimeout()
{
	if (bConsumed || !Node.IsValid())
	{
		return;
	}

	const TCHAR* Advice = Poller.IsActive()
		? TEXT("Its condition never became true.")
		: Listener
			? TEXT("Its event dispatcher was never called.")
			: TEXT("Make sure every path through the event calls Continue Step, Fail Step or Abort Flow From This Step.");
	const FString Message = FString::Printf(
		TEXT("Step '%s' did not finish within %.1f s, so it failed. %s The limit is the step's Timeout Seconds, or the flow's Default Step Timeout on Create Control Flow."),
		*StepPath, TimeoutSeconds, Advice);

	if (Record.IsValid())
	{
		FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Error, Message, *Record);
	}
	else
	{
		UE_LOG(LogControlFlowBP, Error, TEXT("%s"), *Message);
	}

	if (FControlFlowBPDebug::ShouldDumpOnFailure())
	{
		if (const UControlFlowBP* Owner = Root.Get(); Owner && Owner->GetDebugRecord())
		{
			Owner->GetDebugRecord()->Dump(ELogVerbosity::Warning, FString::Printf(TEXT("Flow '%s' when '%s' timed out:"), *Owner->GetDebugRecord()->Name, *StepPath));
		}
	}

	FailStep(FString::Printf(TEXT("timed out after %.1f s"), TimeoutSeconds));
}

bool UControlFlowStepHandle::TryConsume(const TCHAR* Verb)
{
	if (bConsumed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("%s on step '%s': this step was already resolved. Resolve a wait handle exactly once."),
			Verb, *StepPath), Record.Get());
		return false;
	}

	bConsumed = true;
	StopWaiting();

	if (!Node.IsValid() || !Node->GetParent().IsValid())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("%s on step '%s': the flow has already moved on; ignoring."),
			Verb, *StepPath), Record.Get());

		Node.Reset();
		if (UControlFlowBP* Owner = Root.Get())
		{
			Owner->Release(this);
		}
		return false;
	}

	return true;
}

void UControlFlowBranchScope::Init(const TSharedRef<FControlFlowBranch>& InBranch, UControlFlowBP* InRoot, const FString& InTaskPath, const FInstancedStruct& InPayload,
	const TSharedPtr<FControlFlowBPStepRecord>& InRecord)
{
	BranchWeak = InBranch;
	Root = InRoot;
	TaskPath = InTaskPath;
	Payload = InPayload;
	Record = InRecord;
}

UControlFlowBP* UControlFlowBranchScope::GetFlow() const
{
	return Root.Get();
}

UControlFlowBP* UControlFlowBranchScope::AddBranch(int32 Key, FString BranchName)
{
	if (bClosed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Add Case on '%s': the step's function has already returned."), *TaskPath), Record.Get());
		return nullptr;
	}

	if (TObjectPtr<UControlFlowBP>* Existing = Cases.Find(Key))
	{
		return *Existing;
	}

	const TSharedPtr<FControlFlowBranch> Branch = BranchWeak.Pin();
	UControlFlowBP* Owner = Root.Get();
	if (!Branch.IsValid() || !Owner || !Record.IsValid())
	{
		return nullptr;
	}

	const FString CaseName = BranchName.IsEmpty() ? FString::Printf(TEXT("Case%d"), Key) : BranchName;
	FControlFlow& CaseFlow = Branch->AddOrGetBranch(Key, CaseName);

	UControlFlowBP* Child = UControlFlowBP::MakeChild(CaseFlow, Owner, TaskPath + TEXT(".") + CaseName, Record.ToSharedRef());
	if (Child)
	{
		Owner->ReleaseChildWhenFlowFinishes(Child, CaseFlow.AsShared());
		Cases.Add(Key, Child);
		CaseNames.Add(Key, CaseName);
	}

	return Child;
}

void UControlFlowBranchScope::SelectBranch(int32 Key)
{
	if (bClosed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Select Case on '%s': the step's function has already returned."), *TaskPath), Record.Get());
		return;
	}

	if (bHasSelection && SelectedKey != Key)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Select Case on '%s': called more than once (%d then %d); the last call wins."), *TaskPath, SelectedKey, Key), Record.Get());
	}

	SelectedKey = Key;
	bHasSelection = true;
}

void UControlFlowBranchScope::SelectCaseByName(FString CaseName)
{
	if (bClosed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Select Case By Name on '%s': the step's function has already returned."), *TaskPath), Record.Get());
		return;
	}

	for (const TPair<int32, FString>& Pair : CaseNames)
	{
		if (Pair.Value.Equals(CaseName, ESearchCase::IgnoreCase))
		{
			SelectBranch(Pair.Key);
			return;
		}
	}

	TArray<FString> Names;
	CaseNames.GenerateValueArray(Names);
	FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Select Case By Name on '%s': there is no case named '%s'%s."), *TaskPath, *CaseName,
		Names.IsEmpty() ? TEXT(" - add the cases with Add Case first") : *FString::Printf(TEXT(" - the cases are: %s"), *FString::Join(Names, TEXT(", ")))), Record.Get());
}

int32 UControlFlowBranchScope::CloseAndResolve()
{
	bClosed = true;

	if (!bHasSelection && Record.IsValid())
	{
		FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Switch '%s': no case was selected - call Select Case or Select Case By Name in the step's function. Defaulting to key 0."), *TaskPath), *Record);
	}

	bool bKeyWasAdded = true;
	if (const TSharedPtr<FControlFlowBranch> Branch = BranchWeak.Pin())
	{
		if (!Branch->Contains(SelectedKey))
		{
			bKeyWasAdded = false;

			if (Record.IsValid())
			{
				FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Error, FString::Printf(TEXT("Switch '%s': key %d was never added with Add Case; running an empty case."), *TaskPath, SelectedKey), *Record);
			}

			Branch->AddOrGetBranch(SelectedKey, FString::Printf(TEXT("%s.Case%d"), *TaskPath, SelectedKey));
		}
	}

	UControlFlowBP* Owner = Root.Get();
	FControlFlowBPFlowRecord* Debug = Owner ? Owner->GetDebugRecord() : nullptr;
	for (TPair<int32, TObjectPtr<UControlFlowBP>>& Pair : Cases)
	{
		if (UControlFlowBP* Case = Pair.Value)
		{
			Case->CloseScope();

			if (Pair.Key != SelectedKey)
			{
				if (Debug && Case->GetDebugLane().IsValid())
				{
					Debug->SkipLane(*Case->GetDebugLane(), TEXT("case not taken"));

					if (Record.IsValid())
					{
						Record->ChildLanes.Remove(Case->GetDebugLane().ToSharedRef());
					}
				}

				if (Owner)
				{
					Owner->Release(Case);
				}
			}
		}
	}

	if (Record.IsValid())
	{
		const FString* CaseName = CaseNames.Find(SelectedKey);
		Record->Detail = bKeyWasAdded && CaseName
			? FString::Printf(TEXT("took '%s' (key %d)"), **CaseName, SelectedKey)
			: FString::Printf(TEXT("took key %d, which was never added - an empty case"), SelectedKey);

		if (Debug)
		{
			Debug->TraceStepDetail(*Record, Record->Detail);
		}
	}

	Cases.Reset();
	CaseNames.Reset();
	BranchWeak.Reset();

	return SelectedKey;
}

void UControlFlowForkScope::Init(const TSharedRef<FConcurrentControlFlows>& InFork, UControlFlowBP* InRoot, const FString& InTaskPath, const FInstancedStruct& InPayload,
	const TSharedPtr<FControlFlowBPStepRecord>& InRecord, bool bInRace)
{
	ForkWeak = InFork;
	Root = InRoot;
	TaskPath = InTaskPath;
	Payload = InPayload;
	Record = InRecord;
	bRace = bInRace;
}

UControlFlowBP* UControlFlowForkScope::GetFlow() const
{
	return Root.Get();
}

UControlFlowBP* UControlFlowForkScope::AddProng(int32 Key, FString ProngName)
{
	if (bClosed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Add Track on '%s': the step's function has already returned."), *TaskPath), Record.Get());
		return nullptr;
	}

	if (TObjectPtr<UControlFlowBP>* Existing = Prongs.Find(Key))
	{
		return *Existing;
	}

	const TSharedPtr<FConcurrentControlFlows> Fork = ForkWeak.Pin();
	UControlFlowBP* Owner = Root.Get();
	if (!Fork.IsValid() || !Owner || !Record.IsValid())
	{
		return nullptr;
	}

	const FString Name = ProngName.IsEmpty() ? FString::Printf(TEXT("Track%d"), Key) : ProngName;
	FControlFlow& ProngFlow = Fork->AddOrGetFlow(Key, Name);

	UControlFlowBP* Child = UControlFlowBP::MakeChild(ProngFlow, Owner, TaskPath + TEXT(".") + Name, Record.ToSharedRef());
	if (Child)
	{
		Owner->ReleaseChildWhenFlowFinishes(Child, ProngFlow.AsShared());
		Prongs.Add(Key, Child);
	}

	return Child;
}

int32 UControlFlowForkScope::CloseAndCountProngs()
{
	bClosed = true;

	FString ProngList;
	const int32 Count = Prongs.Num();
	for (TPair<int32, TObjectPtr<UControlFlowBP>>& Pair : Prongs)
	{
		if (UControlFlowBP* Prong = Pair.Value)
		{
			Prong->CloseScope();

			FString ProngName = Prong->GetFlowPath();
			ProngName.RemoveFromStart(TaskPath + TEXT("."));
			ProngList += (ProngList.IsEmpty() ? TEXT("'") : TEXT(", '")) + ProngName + TEXT("'");
		}
	}

	if (Record.IsValid() && Count > 0)
	{
		if (const UControlFlowBP* Owner = Root.Get(); Owner && Owner->GetDebugRecord())
		{
			Owner->GetDebugRecord()->TraceStepDetail(*Record, FString::Printf(TEXT("%d track(s): %s"), Count, *ProngList));
		}
	}

	if (bRace && Count > 0)
	{
		ArmRace();
	}

	Prongs.Reset();
	ForkWeak.Reset();

	return Count;
}

void UControlFlowForkScope::ArmRace()
{
	struct FRace
	{
		TArray<TPair<FString, TWeakPtr<FControlFlow>>> Tracks;
		bool bDecided = false;
	};

	const TSharedRef<FRace> Race = MakeShared<FRace>();
	for (const TPair<int32, TObjectPtr<UControlFlowBP>>& Pair : Prongs)
	{
		const UControlFlowBP* Track = Pair.Value;
		if (const TSharedPtr<FControlFlow> TrackFlow = Track ? Track->PinFlow() : nullptr)
		{
			FString TrackName = Track->GetFlowPath();
			TrackName.RemoveFromStart(TaskPath + TEXT("."));
			Race->Tracks.Emplace(MoveTemp(TrackName), TrackFlow);
		}
	}

	const TWeakObjectPtr<UControlFlowBP> WeakRoot = Root;
	const TWeakPtr<FControlFlowBPStepRecord> WeakRecord = Record;

	for (int32 Index = 0; Index < Race->Tracks.Num(); ++Index)
	{
		const TSharedPtr<FControlFlow> TrackFlow = Race->Tracks[Index].Value.Pin();
		if (!TrackFlow.IsValid())
		{
			continue;
		}

		TrackFlow->OnFlowComplete().AddLambda([Race, Index, WeakRoot, WeakRecord]() -> void
		{
			if (Race->bDecided)
			{
				return;
			}
			Race->bDecided = true;

			const FString& Winner = Race->Tracks[Index].Key;
			if (const TSharedPtr<FControlFlowBPStepRecord> RaceRecord = WeakRecord.Pin())
			{
				RaceRecord->Detail = FString::Printf(TEXT("won by '%s'"), *Winner);

				if (const UControlFlowBP* Owner = WeakRoot.Get(); Owner && Owner->GetDebugRecord())
				{
					Owner->GetDebugRecord()->TraceStepDetail(*RaceRecord, RaceRecord->Detail);
				}
			}

			const UControlFlowBP* RaceRoot = WeakRoot.Get();
			FControlFlowBPTimer::After(RaceRoot ? RaceRoot->GetClock() : FControlFlowBPClock(), 0.f, [Race, Index, WeakRoot]() -> void
			{
				const FString Reason = FString::Printf(TEXT("lost the race to '%s'"), *Race->Tracks[Index].Key);

				TSharedPtr<FControlFlowBPFlowRecord> Debug;
				if (const UControlFlowBP* Owner = WeakRoot.Get(); Owner && Owner->GetDebugRecord())
				{
					Debug = Owner->GetDebugRecord()->AsShared();
				}

				for (int32 Other = 0; Other < Race->Tracks.Num(); ++Other)
				{
					const TSharedPtr<FControlFlow> Loser = Other != Index ? Race->Tracks[Other].Value.Pin() : nullptr;
					if (!Loser.IsValid() || !Loser->IsRunning())
					{
						continue;
					}

					const EControlFlowBPCancelCause PreviousCause = Debug.IsValid() ? Debug->InterruptCause : EControlFlowBPCancelCause::None;
					const FString PreviousReason = Debug.IsValid() ? Debug->InterruptReason : FString();
					if (Debug.IsValid())
					{
						Debug->InterruptCause = EControlFlowBPCancelCause::Requested;
						Debug->InterruptReason = Reason;
					}

					Loser->CancelFlow();

					if (Debug.IsValid())
					{
						Debug->InterruptCause = PreviousCause;
						Debug->InterruptReason = PreviousReason;
					}
				}
			});
		});
	}
}

void UControlFlowLoopScope::Init(UControlFlowBP* InRoot, UControlFlowBP* InBody, const FString& InTaskPath, const FInstancedStruct& InPayload,
	const TSharedPtr<FControlFlowBPStepRecord>& InRecord)
{
	Root = InRoot;
	Body = InBody;
	TaskPath = InTaskPath;
	Payload = InPayload;
	Record = InRecord;
}

UControlFlowBP* UControlFlowLoopScope::GetBody() const
{
	if (!bInBuildPhase)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Loop '%s': Get Body is only valid while the loop's function or Build Iteration event runs. Queue this iteration's steps there."),
			*TaskPath), Record.Get());
		return nullptr;
	}

	return Body;
}

UControlFlowBP* UControlFlowLoopScope::GetFlow() const
{
	return Root.Get();
}

DEFINE_FUNCTION(UControlFlowLoopScope::execGetCurrentItem)
{
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FProperty>(nullptr);
	const FProperty* ItemProperty = Stack.MostRecentProperty;
	void* ItemAddress = Stack.MostRecentPropertyAddress;

	P_FINISH;

	bool bValid = false;
	P_NATIVE_BEGIN;
	bValid = P_THIS->CopyCurrentItem(ItemProperty, ItemAddress);
	P_NATIVE_END;

	*static_cast<bool*>(RESULT_PARAM) = bValid;
}

bool UControlFlowLoopScope::CopyCurrentItem(const FProperty* ItemProperty, void* OutItem) const
{
	using namespace UE::ControlFlowBP::PropertyUtils;

	static const FName ItemsName(TEXT("Items"));
	const FPropertyBagPropertyDesc* ItemsDesc = Items.FindPropertyDescByName(ItemsName);
	const FArrayProperty* ArrayProperty = ItemsDesc ? CastField<FArrayProperty>(ItemsDesc->CachedProperty) : nullptr;
	if (!ArrayProperty)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Get Current Item on '%s': only Queue For Each has items. Use Get Iteration Index here."), *TaskPath), Record.Get());
		return false;
	}

	if (!bInBuildPhase)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Get Current Item on '%s': only valid inside Build Iteration."), *TaskPath), Record.Get());
		return false;
	}

	if (!ItemProperty || !OutItem)
	{
		return false;
	}

	FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Items.GetValue().GetMemory()));
	const int32 Index = GetIterationIndex();
	if (!Helper.IsValidIndex(Index))
	{
		return false;
	}

	const void* Element = Helper.GetRawPtr(Index);
	if (!CanRead(*ArrayProperty->Inner, Element, *ItemProperty))
	{
		if (!bReportedItemMismatch)
		{
			bReportedItemMismatch = true;
			FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
				TEXT("Get Current Item on '%s': the items are %s, which cannot be read as a %s."),
				*TaskPath, *DescribeType(*ArrayProperty->Inner), *DescribeType(*ItemProperty)), Record.Get());
		}
		return false;
	}

	CopyValue(*ItemProperty, OutItem, *ArrayProperty->Inner, Element);
	return true;
}

void UControlFlowLoopScope::ContinueLooping(bool bRunAnotherIteration)
{
	if (!bInConditionPhase)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Loop '%s': Continue Looping is only valid while a Loop step's function runs. Queue Repeat and Queue For Each decide on their own."), *TaskPath), Record.Get());
		return;
	}

	if (bAnswered && bRunAgain != bRunAnotherIteration)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Loop '%s': Continue Looping was called more than once; the last call wins."), *TaskPath), Record.Get());
	}

	bAnswered = true;
	bRunAgain = bRunAnotherIteration;
}

void UControlFlowLoopScope::UpdateBodyPath()
{
	if (Body)
	{
		Body->SetFlowPath(FString::Printf(TEXT("%s#%d"), *TaskPath, IterationIndex));
	}
}

void UControlFlowLoopScope::BeginConditionPhase()
{
	bInConditionPhase = true;
	bInBuildPhase = false;
	bAnswered = false;
	bRunAgain = false;

	if (Body)
	{
		Body->CloseScope();
	}
}

bool UControlFlowLoopScope::EndConditionPhase()
{
	bInConditionPhase = false;

	if (!bAnswered && Record.IsValid())
	{
		FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Loop '%s': the Condition event never called Continue Looping; stopping the loop."), *TaskPath), *Record);
	}

	return bRunAgain;
}

void UControlFlowLoopScope::BeginBuildPhase()
{
	++IterationIndex;
	bInBuildPhase = true;
	UpdateBodyPath();

	if (Body)
	{
		Body->OpenScope();
	}
}

void UControlFlowLoopScope::EndBuildPhase()
{
	bInBuildPhase = false;

	if (Body)
	{
		Body->CloseScope();
	}
}

void UControlFlowLoopScope::BeginCombinedPhase()
{
	++IterationIndex;
	bInConditionPhase = true;
	bInBuildPhase = true;
	bAnswered = false;
	bRunAgain = false;
	UpdateBodyPath();

	if (Body)
	{
		Body->OpenScope();
	}
}

bool UControlFlowLoopScope::EndCombinedPhase()
{
	bInConditionPhase = false;
	bInBuildPhase = false;

	if (Body)
	{
		Body->CloseScope();
	}

	if (!bAnswered && Record.IsValid())
	{
		FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning, FString::Printf(
			TEXT("Loop '%s': the loop's function never called Continue Looping; stopping the loop."), *TaskPath), *Record);
	}

	return bRunAgain;
}
