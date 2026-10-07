#include "ControlFlowBP.h"

#include "ControlFlowBPDebug.h"
#include "ControlFlowBPDispatcherListener.h"
#include "ControlFlowBPModule.h"
#include "ControlFlowBPPropertyUtils.h"
#include "ControlFlowBPScopes.h"
#include "ControlFlowBPSubsystem.h"

#include "ControlFlowBranch.h"
#include "ControlFlowConcurrency.h"
#include "ControlFlowConditionalLoop.h"

#include "Containers/Ticker.h"
#include "Misc/TrackedActivity.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UE::ControlFlowBP::Private
{
	static UControlFlowBP* MakeRootFlow(UObject* Owner, const FString& DebugName, float Timeout)
	{
		UControlFlowBP* Flow = NewObject<UControlFlowBP>(GetTransientPackage());
		Flow->InitAsRoot(DebugName, Owner, Timeout);
		return Flow;
	}

	static void ContinueAfter(const UControlFlowBP& Root, const FControlFlowNodeRef& Node, float Seconds)
	{
		FControlFlowBPTimer::After(Root.GetClock(), Seconds, [Node]() -> void
		{
			if (Node->GetParent().IsValid() && !Node->HasCancelBeenRequested())
			{
				Node->ContinueFlow();
			}
		});
	}
}

UControlFlowBP* UControlFlowBP::CreateControlFlow(UObject* Owner, FString FlowDebugName, float DefaultStepTimeout)
{
	if (FlowDebugName.IsEmpty())
	{
		FlowDebugName = TEXT("ControlFlow");
	}

	return UE::ControlFlowBP::Private::MakeRootFlow(Owner, FlowDebugName, DefaultStepTimeout);
}

UControlFlowBP* UControlFlowBP::FindOrCreateNamedFlow(UObject* Owner, FString FlowId, float DefaultStepTimeout)
{
	if (!Owner)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, TEXT("Find Or Create Named Flow: Owner is null."));
		return nullptr;
	}

	if (FlowId.IsEmpty())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, TEXT("Find Or Create Named Flow: Flow Id must not be empty."));
		return nullptr;
	}

	UControlFlowBPSubsystem* Subsystem = UControlFlowBPSubsystem::Get();
	if (!Subsystem)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, TEXT("Find Or Create Named Flow: the ControlFlowBP subsystem is unavailable."));
		return nullptr;
	}

	if (UControlFlowBP* Existing = Subsystem->FindNamed(Owner, FlowId))
	{
		if (!Existing->IsFinished())
		{
			return Existing;
		}

		Subsystem->ReleaseFlow(Existing);
	}

	UControlFlowBP* NewFlow = UE::ControlFlowBP::Private::MakeRootFlow(Owner, FlowId, DefaultStepTimeout);
	Subsystem->RegisterNamed(Owner, FlowId, NewFlow);
	return NewFlow;
}

UControlFlowBP* UControlFlowBP::FindNamedFlow(UObject* Owner, FString FlowId)
{
	UControlFlowBPSubsystem* Subsystem = UControlFlowBPSubsystem::Get();
	return Subsystem ? Subsystem->FindNamed(Owner, FlowId) : nullptr;
}

bool UControlFlowBP::IsNamedFlowRunning(UObject* Owner, FString FlowId)
{
	const UControlFlowBP* Flow = FindNamedFlow(Owner, FlowId);
	return Flow && Flow->IsRunning();
}

void UControlFlowBP::StopNamedFlow(UObject* Owner, FString FlowId)
{
	if (UControlFlowBP* Flow = FindNamedFlow(Owner, FlowId))
	{
		const FControlFlowBPCallSite Site = FControlFlowBPCallSite::Capture();
		Flow->CancelWithReason(EControlFlowBPCancelCause::Requested, Site.IsSet()
			? FString::Printf(TEXT("Stop Named Flow was called from %s"), *Site.Describe())
			: FString(TEXT("Stop Named Flow was called")));

		if (UControlFlowBPSubsystem* Subsystem = UControlFlowBPSubsystem::Get())
		{
			Subsystem->UnregisterNamed(Flow);
		}
	}
}

void UControlFlowBP::InitAsRoot(const FString& InDebugName, UObject* InOwner, float InDefaultStepTimeout)
{
	bIsRoot = true;
	RootFlow = this;
	DebugName = InDebugName;
	FlowPath = InDebugName;
	DefaultStepTimeout = FMath::Max(0.f, InDefaultStepTimeout);
	LifetimeOwner = InOwner;
	bHadOwner = (InOwner != nullptr);

	OwnedFlow = MakeShared<FControlFlow>(InDebugName);
	FlowWeak = OwnedFlow;
	Clock = FControlFlowBPClock(InOwner);

	DebugRecord = MakeShared<FControlFlowBPFlowRecord>(InDebugName, this, InOwner, Clock);
	DebugRecord->CreateSite = FControlFlowBPCallSite::Capture();
	Lane = DebugRecord->RootLane;
	FControlFlowBPDebug::RegisterFlow(DebugRecord.ToSharedRef());

	BindFlowEvents(OwnedFlow.ToSharedRef());
}

void UControlFlowBP::InitAsChild(const TSharedRef<FControlFlow>& InFlow, UControlFlowBP* InRoot, const FString& InPathSegment, const TSharedRef<FControlFlowBPLane>& InLane)
{
	bIsRoot = false;
	RootFlow = InRoot;
	FlowWeak = InFlow;
	DebugName = InFlow->GetDebugName();
	FlowPath = InPathSegment;
	Lane = InLane;

	BindFlowEvents(InFlow);
}

UControlFlowBP* UControlFlowBP::MakeChild(FControlFlow& InFlow, UControlFlowBP* InRoot, const FString& InPathSegment, const TSharedRef<FControlFlowBPStepRecord>& OwnerStep)
{
	if (!InRoot)
	{
		return nullptr;
	}

	FControlFlowBPFlowRecord* Debug = InRoot->GetDebugRecord();
	const TSharedRef<FControlFlowBPLane> ChildLane = Debug ? Debug->AddChildLane(OwnerStep, InPathSegment) : MakeShared<FControlFlowBPLane>();

	UControlFlowBP* Child = NewObject<UControlFlowBP>(GetTransientPackage());

	Child->InitAsChild(InFlow.AsShared(), InRoot, InPathSegment, ChildLane);
	InRoot->Track(Child);
	return Child;
}

void UControlFlowBP::BeginDestroy()
{
	if (bIsRoot && !bHasExecuted && DebugRecord.IsValid() && DebugRecord->State == EControlFlowBPFlowState::Building)
	{
		const TSharedRef<FControlFlowBPFlowRecord> Record = DebugRecord.ToSharedRef();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Record](float) -> bool
		{
			FControlFlowBPDebug::ReportNeverExecuted(*Record);
			return false;
		}), 0.f);
	}

	OwnedFlow.Reset();
	FlowWeak.Reset();

	Super::BeginDestroy();
}

UControlFlowBP* UControlFlowBP::GetRoot()
{
	return bIsRoot ? this : RootFlow.Get();
}

const UControlFlowBP* UControlFlowBP::GetRoot() const
{
	return bIsRoot ? this : RootFlow.Get();
}

void UControlFlowBP::BindFlowEvents(const TSharedRef<FControlFlow>& InFlow)
{
	UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		return;
	}

	const TWeakPtr<FControlFlow> WeakFlow = InFlow;
	const TWeakPtr<FControlFlowBPLane> WeakLane = Lane;
	InFlow->OnNodeComplete().AddWeakLambda(Root, [Root, WeakFlow, WeakLane]()
	{
		Root->HandleNodeCompleted(WeakFlow, WeakLane);
	});

	const bool bRootFlow = bIsRoot;

	InFlow->OnFlowComplete().AddWeakLambda(Root, [Root, WeakLane, bRootFlow]() -> void
	{
		const TSharedPtr<FControlFlowBPLane> PinnedLane = WeakLane.Pin();
		if (FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord(); Debug && PinnedLane.IsValid())
		{
			Debug->NotifyLaneCompleted(*PinnedLane);
		}

		if (bRootFlow)
		{
			Root->HandleFinished(false);
		}
	});

	InFlow->OnFlowCancel().AddWeakLambda(Root, [Root, WeakLane, bRootFlow]() -> void
	{
		const TSharedPtr<FControlFlowBPLane> PinnedLane = WeakLane.Pin();
		if (FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord(); Debug && PinnedLane.IsValid())
		{
			Debug->NotifyLaneCancelled(*PinnedLane);
		}

		if (bRootFlow)
		{
			Root->HandleFinished(true);
		}
	});
}

void UControlFlowBP::HandleNodeCompleted(const TWeakPtr<FControlFlow>& CompletedFlow, const TWeakPtr<FControlFlowBPLane>& CompletedLane)
{
	InvalidateHandlesFor(CompletedFlow);

	TSharedPtr<FControlFlowBPStepRecord> Completed;
	if (const TSharedPtr<FControlFlowBPLane> PinnedLane = CompletedLane.Pin(); PinnedLane.IsValid() && DebugRecord.IsValid())
	{
		Completed = DebugRecord->NotifyNodeCompleted(*PinnedLane);
	}

	if (!Completed.IsValid() || Completed->IsInternal())
	{
		return;
	}

	const FControlFlowBPCallbackScope Callback(*this);
	OnStepComplete.Broadcast(this, Completed->Path);
}

void UControlFlowBP::InvalidateHandlesFor(const TWeakPtr<FControlFlow>& CompletedFlow)
{
	if (!bIsRoot)
	{
		return;
	}

	const FControlFlow* Target = CompletedFlow.Pin().Get();
	if (!Target)
	{
		return;
	}

	TArray<UControlFlowStepHandle*> Stale;
	for (const TObjectPtr<UObject>& Object : LiveObjects)
	{
		if (UControlFlowStepHandle* Handle = Cast<UControlFlowStepHandle>(Object.Get()))
		{
			if (Handle->GetOwningFlow().Pin().Get() == Target)
			{
				Stale.Add(Handle);
			}
		}
	}

	for (UControlFlowStepHandle* Handle : Stale)
	{
		Handle->ForceInvalidate();
		Release(Handle);
	}
}

void UControlFlowBP::HandleFinished(bool bCancelled)
{
	if (!bIsRoot || bFinished)
	{
		return;
	}

	bFinished = true;

	if (DebugRecord.IsValid())
	{
		DebugRecord->FinalVariables = DescribeFlowVariables();
		DebugRecord->NotifyFlowFinished(bCancelled);
	}

	TArray<TObjectPtr<UObject>> Live = LiveObjects.Array();
	LiveObjects.Reset();
	PayloadStore.Reset();

	for (const TObjectPtr<UObject>& Object : Live)
	{
		if (UControlFlowStepHandle* Handle = Cast<UControlFlowStepHandle>(Object.Get()))
		{
			Handle->ForceInvalidate();
		}
		else if (UControlFlowBP* Child = Cast<UControlFlowBP>(Object.Get()))
		{
			Child->CloseScope();
		}
	}

	{
		const FControlFlowBPCallbackScope Callback(*this);
		if (bCancelled)
		{
			OnFlowCancel.Broadcast(this);
		}
		else
		{
			OnFlowComplete.Broadcast(this);
		}

		OnFlowFinishedNative.Broadcast(bCancelled);
	}

	if (UControlFlowBPSubsystem* Subsystem = UControlFlowBPSubsystem::Get())
	{
		Subsystem->ReleaseFlow(this);
	}
}

void UControlFlowBP::HandleOwningWorldCleanup(EControlFlowBPCancelCause Cause, const FString& Reason)
{
	if (!bIsRoot || bFinished)
	{
		return;
	}

	if (DebugRecord.IsValid())
	{
		DebugRecord->SetCancelReason(Cause, Reason);
	}

	if (bHasExecuted && OwnedFlow.IsValid() && OwnedFlow->IsRunning())
	{
		if (DebugRecord.IsValid())
		{
			DebugRecord->MarkRunningStepsCancelRequested();
		}

		OwnedFlow->CancelFlow();
	}

	if (!bFinished)
	{
		HandleFinished(true);
	}
}

void UControlFlowBP::ReleaseChildWhenFlowFinishes(UControlFlowBP* Child, const TSharedRef<FControlFlow>& ChildFlow)
{
	UControlFlowBP* Root = GetRoot();
	if (!Root || !Child)
	{
		return;
	}

	const TWeakObjectPtr<UControlFlowBP> WeakChild(Child);
	const auto ReleaseIt = [Root, WeakChild]() -> void
	{
		if (UControlFlowBP* Live = WeakChild.Get())
		{
			Live->CloseScope();
			Root->Release(Live);
		}
	};

	ChildFlow->OnFlowComplete().AddWeakLambda(Root, ReleaseIt);
	ChildFlow->OnFlowCancel().AddWeakLambda(Root, ReleaseIt);
}

void UControlFlowBP::Track(UObject* Object)
{
	if (!Object)
	{
		return;
	}

	if (UControlFlowBP* Root = GetRoot())
	{
		Root->LiveObjects.Add(Object);
	}
}

void UControlFlowBP::Release(UObject* Object)
{
	if (!Object)
	{
		return;
	}

	if (UControlFlowBP* Root = GetRoot())
	{
		Root->LiveObjects.Remove(Object);
	}
}

int32 UControlFlowBP::StorePayload(const FInstancedStruct& InPayload)
{
	if (!InPayload.IsValid())
	{
		return 0;
	}

	UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		return 0;
	}

	const int32 Id = Root->NextPayloadId++;
	Root->PayloadStore.Add(Id, InPayload);
	return Id;
}

FInstancedStruct UControlFlowBP::GetStoredPayload(int32 PayloadId) const
{
	if (PayloadId == 0)
	{
		return FInstancedStruct();
	}

	if (const UControlFlowBP* Root = GetRoot())
	{
		if (const FInstancedStruct* Found = Root->PayloadStore.Find(PayloadId))
		{
			return *Found;
		}
	}

	return FInstancedStruct();
}

void UControlFlowBP::ReleasePayload(int32 PayloadId)
{
	if (PayloadId == 0)
	{
		return;
	}

	if (UControlFlowBP* Root = GetRoot())
	{
		Root->PayloadStore.Remove(PayloadId);
	}
}

void UControlFlowBP::NotifyStepStarted(FControlFlowBPStepRecord& Step)
{
	if (FControlFlowBPFlowRecord* Debug = GetDebugRecord())
	{
		Debug->NotifyStepStarted(Step);
	}
}

void UControlFlowBP::ReportStepFailed(FControlFlowBPStepRecord& Step, const FString& Reason)
{
	UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		return;
	}

	Step.bFailed = true;
	Step.Detail = Reason;

	UE_LOG(LogControlFlowBP, Warning, TEXT("Step '%s' failed: %s"), *Step.Path, *Reason);

	const FControlFlowBPCallbackScope Callback(*Root);
	Root->OnStepFailed.Broadcast(Root, Step.Path);
	Root->OnStepFailedNative.Broadcast(Step.Path, Reason);
}

bool UControlFlowBP::IsInFlowCallback() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root && Root->bInFlowCallback;
}

float UControlFlowBP::GetDefaultStepTimeout() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->DefaultStepTimeout : 0.f;
}

const FControlFlowBPClock& UControlFlowBP::GetClock() const
{
	static const FControlFlowBPClock RealTime;

	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->Clock : RealTime;
}

bool UControlFlowBP::IsOwnerStale() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root && Root->bHadOwner && !Root->LifetimeOwner.IsValid();
}

UObject* UControlFlowBP::GetOwner() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->LifetimeOwner.Get() : nullptr;
}

FControlFlowBPFlowRecord* UControlFlowBP::GetDebugRecord() const
{
	const UControlFlowBP* Root = GetRoot();
	return Root ? Root->DebugRecord.Get() : nullptr;
}

TSharedRef<FControlFlowBPStepRecord> UControlFlowBP::RecordStep(EControlFlowBPStepType Type, const FString& StepName, const FString& StepPath,
	const UObject* HandlerObject, FName HandlerFunction)
{
	FControlFlowBPFlowRecord* Debug = GetDebugRecord();
	if (!Debug || !Lane.IsValid())
	{
		TSharedRef<FControlFlowBPStepRecord> Detached = MakeShared<FControlFlowBPStepRecord>();
		Detached->Name = StepName;
		Detached->Path = StepPath;
		Detached->Type = Type;
		return Detached;
	}

	return Debug->AddStep(*Lane, Type, StepName, StepPath, HandlerObject, HandlerFunction);
}

void UControlFlowBP::SetFlowPath(const FString& InFlowPath)
{
	FlowPath = InFlowPath;

	if (Lane.IsValid())
	{
		Lane->Path = InFlowPath;
	}
}

void UControlFlowBP::QueueLoopYield()
{
	TSharedPtr<FControlFlow> Flow = FlowWeak.Pin();
	UControlFlowBP* Root = GetRoot();
	if (!Flow.IsValid() || !Root)
	{
		return;
	}

	RecordStep(EControlFlowBPStepType::Internal, TEXT("LoopYield"), FlowPath + TEXT(".LoopYield"));
	Flow->QueueWait(TEXT("LoopYield")).BindWeakLambda(Root, [Root](FControlFlowNodeRef Node) -> void
	{
		UE::ControlFlowBP::Private::ContinueAfter(*Root, Node, 0.f);
	});
}

FString UControlFlowBP::MakeStepName(const FString& InStepName, FName BoundFunctionName, const TCHAR* Fallback)
{
	if (!InStepName.IsEmpty())
	{
		return InStepName;
	}

	if (!BoundFunctionName.IsNone())
	{
		return BoundFunctionName.ToString();
	}

	return Fallback;
}

TSharedPtr<FControlFlow> UControlFlowBP::BeginQueue(const TCHAR* NodeLabel) const
{
	if (bScopeClosed)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("%s on '%s': this flow handle has expired. A flow handed to a step's function - a sub-flow, case, track or loop body - may only be queued onto while that function runs."),
			NodeLabel, *FlowPath));
		return nullptr;
	}

	const UControlFlowBP* Root = GetRoot();
	if (!Root)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("%s on '%s': the owning flow no longer exists."), NodeLabel, *FlowPath));
		return nullptr;
	}

	if (Root->bFinished)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("%s on '%s': the flow has already finished."), NodeLabel, *FlowPath));
		return nullptr;
	}

	if (bIsRoot && bHasExecuted)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("%s on '%s': steps cannot be added after Execute Flow. Use a Queue Step of kind Sub Flow or Loop to add work while the flow runs."),
			NodeLabel, *FlowPath));
		return nullptr;
	}

	TSharedPtr<FControlFlow> Flow = FlowWeak.Pin();
	if (!Flow.IsValid())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("%s on '%s': the underlying flow no longer exists."), NodeLabel, *FlowPath));
		return nullptr;
	}

	return Flow;
}

EControlFlowStepKind UControlFlowBP::DeduceStepKind(const UFunction* Function, FString& OutError)
{
	if (!Function)
	{
		OutError = TEXT("no function of that name exists on the target");
		return EControlFlowStepKind::Invalid;
	}

	TArray<FProperty*> Params;
	for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		if (It->PropertyFlags & CPF_ReturnParm)
		{
			OutError = TEXT("it returns a value; a control flow step must return nothing");
			return EControlFlowStepKind::Invalid;
		}

		if ((It->PropertyFlags & CPF_OutParm) && !(It->PropertyFlags & CPF_ConstParm))
		{
			OutError = FString::Printf(TEXT("'%s' is an output parameter; a control flow step takes inputs only"), *It->GetName());
			return EControlFlowStepKind::Invalid;
		}

		Params.Add(*It);
	}

	if (Params.Num() == 0)
	{
		return EControlFlowStepKind::Function;
	}

	if (Params.Num() > 1)
	{
		OutError = FString::Printf(TEXT("it takes %d parameters; a control flow step takes at most one"), Params.Num());
		return EControlFlowStepKind::Invalid;
	}

	const FProperty* Param = Params[0];

	if (const FObjectPropertyBase* ObjectParam = CastField<FObjectPropertyBase>(Param))
	{
		const UClass* ParamClass = ObjectParam->PropertyClass;

		if (ParamClass == UControlFlowStepHandle::StaticClass())
		{
			return EControlFlowStepKind::Wait;
		}
		if (ParamClass == UControlFlowBP::StaticClass())
		{
			return EControlFlowStepKind::SubFlow;
		}
		if (ParamClass == UControlFlowBranchScope::StaticClass())
		{
			return EControlFlowStepKind::Branch;
		}
		if (ParamClass == UControlFlowForkScope::StaticClass())
		{
			return EControlFlowStepKind::Fork;
		}
		if (ParamClass == UControlFlowLoopScope::StaticClass())
		{
			return EControlFlowStepKind::Loop;
		}

		OutError = FString::Printf(
			TEXT("its parameter is of type '%s', which is not a control flow handle or scope (the type must match exactly, not a base class)"),
			*GetNameSafe(ParamClass));
		return EControlFlowStepKind::Invalid;
	}

	if (const FStructProperty* StructParam = CastField<FStructProperty>(Param))
	{
		if (StructParam->Struct == FInstancedStruct::StaticStruct())
		{
			return EControlFlowStepKind::Function;
		}

		OutError = FString::Printf(TEXT("its parameter is a '%s' struct; the only struct a step may take is Instanced Struct"), *GetNameSafe(StructParam->Struct));
		return EControlFlowStepKind::Invalid;
	}

	OutError = FString::Printf(TEXT("its parameter '%s' is neither an object reference nor an Instanced Struct"), *Param->GetName());
	return EControlFlowStepKind::Invalid;
}

bool UControlFlowBP::IsConditionFunction(const UFunction* Function, FString& OutError)
{
	if (!Function)
	{
		OutError = TEXT("no function of that name exists on the target");
		return false;
	}

	const FProperty* Answer = nullptr;
	for (TFieldIterator<FProperty> It(Function); It && (It->PropertyFlags & CPF_Parm); ++It)
	{
		const bool bOutput = It->HasAnyPropertyFlags(CPF_ReturnParm)
			|| (It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm));
		if (!bOutput)
		{
			OutError = FString::Printf(TEXT("it takes '%s' as an input; a condition takes nothing"), *It->GetName());
			return false;
		}

		if (Answer)
		{
			OutError = TEXT("it has more than one output; a condition returns a single bool");
			return false;
		}

		Answer = *It;
	}

	if (!Answer)
	{
		OutError = TEXT("it returns nothing; a condition returns a bool");
		return false;
	}

	if (!Answer->IsA<FBoolProperty>())
	{
		OutError = FString::Printf(TEXT("its output '%s' is not a bool"), *Answer->GetName());
		return false;
	}

	return true;
}

EControlFlowStepKind UControlFlowBP::GetStepKind(UObject* Target, FName FunctionName)
{
	if (!Target || FunctionName.IsNone())
	{
		return EControlFlowStepKind::Invalid;
	}

	FString UnusedError;
	return DeduceStepKind(Target->FindFunction(FunctionName), UnusedError);
}

UControlFlowBP* UControlFlowBP::QueueStep(UObject* Target, FName FunctionName, FString StepName, const FInstancedStruct& Payload, float TimeoutSeconds)
{
	if (!Target)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue Step on '%s': Target is null."), *FlowPath));
		return this;
	}

	if (FunctionName.IsNone())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue Step on '%s': Function Name is empty."), *FlowPath));
		return this;
	}

	UFunction* Function = Target->FindFunction(FunctionName);

	FString Error;
	const EControlFlowStepKind Kind = DeduceStepKind(Function, Error);
	if (Kind == EControlFlowStepKind::Invalid)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Queue Step on '%s': cannot use '%s' on %s - %s.\n")
			TEXT("A step must return nothing and take exactly one of:\n")
			TEXT("  ()                          -> synchronous step\n")
			TEXT("  (Instanced Struct)          -> synchronous step, with the payload\n")
			TEXT("  (Control Flow Step Handle)  -> async step\n")
			TEXT("  (Control Flow)              -> sub-flow\n")
			TEXT("  (Control Flow Switch)       -> switch\n")
			TEXT("  (Control Flow Parallel)     -> parallel tracks\n")
			TEXT("  (Control Flow Loop)         -> loop"),
			*FlowPath, *FunctionName.ToString(), *GetNameSafe(Target), *Error));
		return this;
	}

	const FString Name = StepName.IsEmpty() ? FunctionName.ToString() : StepName;

	switch (Kind)
	{
	case EControlFlowStepKind::Function:
		{
			if (Function->NumParms == 0)
			{
				FControlFlowSimpleStep Step;
				Step.BindUFunction(Target, FunctionName);
				return QueueSimpleFunction(Step, Name);
			}

			FControlFlowSyncStep Step;
			Step.BindUFunction(Target, FunctionName);
			return QueueFunction(Step, Name, Payload);
		}

	case EControlFlowStepKind::Wait:
		{
			FControlFlowWaitStep Step;
			Step.BindUFunction(Target, FunctionName);
			return QueueWait(Step, Name, Payload, TimeoutSeconds);
		}

	case EControlFlowStepKind::SubFlow:
		{
			FControlFlowPopulate Populate;
			Populate.BindUFunction(Target, FunctionName);
			return QueueSubFlow(Populate, Name, Payload);
		}

	case EControlFlowStepKind::Branch:
		{
			FControlFlowDefineBranch Define;
			Define.BindUFunction(Target, FunctionName);
			return QueueBranch(Define, Name, Payload);
		}

	case EControlFlowStepKind::Fork:
		{
			FControlFlowDefineFork Define;
			Define.BindUFunction(Target, FunctionName);
			return QueueFork(Define, EControlFlowConcurrency::Ordered, Name, Payload);
		}

	case EControlFlowStepKind::Loop:
		{
			FControlFlowBuildLoop BuildIteration;
			BuildIteration.BindUFunction(Target, FunctionName);
			return QueueCombinedLoop(BuildIteration, EControlFlowLoopMode::DoWhile, Name, Payload, 0);
		}

	default:
		return this;
	}
}

UControlFlowBP* UControlFlowBP::QueueCombinedLoop(FControlFlowBuildLoop BuildIteration, EControlFlowLoopMode Mode, FString TaskName, const FInstancedStruct& Payload, int32 MaxIterations)
{
	return QueueLoopInternal(FControlFlowLoopCondition(), BuildIteration, true, Mode, TaskName, Payload, MaxIterations, FControlFlowBPLoopSetup());
}

UControlFlowBP* UControlFlowBP::QueueSimpleFunction(FControlFlowSimpleStep Step, FString StepName)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(StepName, Step.GetFunctionName(), TEXT("Function"));
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Function, Name, FlowPath + TEXT(".") + Name, Step.GetUObject(), Step.GetFunctionName());

	Flow->QueueFunction(Name).BindWeakLambda(Root, [Root, Step, Record]() -> void
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);

		if (Root->IsOwnerStale())
		{
			UE_LOG(LogControlFlowBP, Warning, TEXT("Step '%s': the flow's owner was destroyed; cancelling."), *Record->Path);
			Root->CancelWithReason(EControlFlowBPCancelCause::OwnerDestroyed, FString::Printf(TEXT("its owner was destroyed (noticed at step '%s')"), *Record->Path));
			return;
		}

		if (!Step.IsBound())
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Step '%s': the handler object no longer exists; skipping."), *Record->Path), *Record);
			return;
		}

		FControlFlowBPDebug::NotifyHandlerStarting(*Record);
		Step.Execute();
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueFunction(FControlFlowSyncStep Step, FString StepName, const FInstancedStruct& Payload)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(StepName, Step.GetFunctionName(), TEXT("Function"));
	const int32 PayloadId = Root->StorePayload(Payload);
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Function, Name, FlowPath + TEXT(".") + Name, Step.GetUObject(), Step.GetFunctionName());

	Flow->QueueFunction(Name).BindWeakLambda(Root, [Root, Step, PayloadId, Record]() -> void
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);
		const FInstancedStruct Local = Root->GetStoredPayload(PayloadId);
		Root->ReleasePayload(PayloadId);

		if (Root->IsOwnerStale())
		{
			UE_LOG(LogControlFlowBP, Warning, TEXT("Step '%s': the flow's owner was destroyed; cancelling."), *Record->Path);
			Root->CancelWithReason(EControlFlowBPCancelCause::OwnerDestroyed, FString::Printf(TEXT("its owner was destroyed (noticed at step '%s')"), *Record->Path));
			return;
		}

		if (!Step.IsBound())
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Step '%s': the handler object no longer exists; skipping."), *Record->Path), *Record);
			return;
		}

		FControlFlowBPDebug::NotifyHandlerStarting(*Record);
		Step.Execute(Local);
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueWait(FControlFlowWaitStep Step, FString StepName, const FInstancedStruct& Payload, float TimeoutSeconds)
{
	const FString Name = MakeStepName(StepName, Step.GetFunctionName(), TEXT("Wait"));

	return QueueWaitInternal(TEXT("Queue Step"), Name, TimeoutSeconds, Payload, Step.GetUObject(), Step.GetFunctionName(), FString(),
		[Step](UControlFlowStepHandle& Handle, FControlFlowBPStepRecord& Record) -> void
		{
			if (!Step.IsBound())
			{
				Record.bHandlerMissing = true;
				FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
					FString::Printf(TEXT("Step '%s': the handler object no longer exists; continuing."), *Record.Path), Record);
				Handle.ContinueStep();
				return;
			}

			FControlFlowBPDebug::NotifyHandlerStarting(Record);
			Step.Execute(&Handle);
		});
}

UControlFlowBP* UControlFlowBP::QueueWaitUntil(FControlFlowCondition Condition, FString StepName, float CheckInterval, float TimeoutSeconds)
{
	const FString Name = MakeStepName(StepName, Condition.GetFunctionName(), TEXT("WaitUntil"));
	const float Interval = FMath::Max(0.f, CheckInterval);
	const FString Info = FString::Printf(TEXT("until %s is true"), Condition.GetFunctionName().IsNone() ? TEXT("?") : *Condition.GetFunctionName().ToString());

	return QueueWaitInternal(TEXT("Queue Wait Until"), Name, TimeoutSeconds, FInstancedStruct(), Condition.GetUObject(), Condition.GetFunctionName(), Info,
		[Condition, Interval](UControlFlowStepHandle& Handle, FControlFlowBPStepRecord& Record) -> void
		{
			if (!Condition.IsBound())
			{
				Record.bHandlerMissing = true;
				FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
					FString::Printf(TEXT("Wait Until '%s': the Condition function no longer exists; continuing."), *Record.Path), Record);
				Handle.ContinueStep();
				return;
			}

			FControlFlowBPDebug::NotifyHandlerStarting(Record);
			Handle.StartPolling(Condition, Interval);
		});
}

UControlFlowBP* UControlFlowBP::QueueWaitForEventDispatcher(UObject* Target, FName DispatcherName, FString StepName, float TimeoutSeconds)
{
	if (!UControlFlowDispatcherListener::FindDispatcher(Target, DispatcherName))
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue Wait For Event Dispatcher on '%s': %s."),
			*FlowPath, *UControlFlowDispatcherListener::DescribeMissingDispatcher(Target, DispatcherName)));
		return this;
	}

	const FString Name = MakeStepName(StepName, DispatcherName, TEXT("WaitForDispatcher"));
	const FString Info = FString::Printf(TEXT("until %s.%s is called"), *Target->GetName(), *DispatcherName.ToString());
	const TWeakObjectPtr<UObject> WeakTarget(Target);

	return QueueWaitInternal(TEXT("Queue Wait For Event Dispatcher"), Name, TimeoutSeconds, FInstancedStruct(), Target, NAME_None, Info,
		[WeakTarget, DispatcherName](UControlFlowStepHandle& Handle, FControlFlowBPStepRecord& Record) -> void
		{
			UControlFlowDispatcherListener* Listener = NewObject<UControlFlowDispatcherListener>(&Handle);

			FString Error;
			if (!Listener->Bind(WeakTarget.Get(), DispatcherName, &Handle, Error))
			{
				Handle.FailStep(Error);
				return;
			}

			Handle.SetListener(Listener);
		});
}

UControlFlowBP* UControlFlowBP::QueueWaitInternal(const TCHAR* NodeLabel, const FString& Name, float TimeoutSeconds, const FInstancedStruct& Payload,
	const UObject* HandlerObject, FName HandlerFunction, const FString& Info, TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>&& Begin)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(NodeLabel);
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const int32 PayloadId = Root->StorePayload(Payload);
	const float Timeout = (TimeoutSeconds >= 0.f) ? TimeoutSeconds : Root->GetDefaultStepTimeout();

	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Wait, Name, FlowPath + TEXT(".") + Name, HandlerObject, HandlerFunction);
	Record->TimeoutSeconds = FMath::Max(0.f, Timeout);

	const FString TimeoutInfo = Timeout > 0.f
		? FString::Printf(TEXT("times out after %s"), *FControlFlowBPDebug::FormatSeconds(Timeout))
		: FString(TEXT("no timeout"));
	Record->Info = Info.IsEmpty() ? TimeoutInfo : FString::Printf(TEXT("%s, %s"), *Info, *TimeoutInfo);

	const TSharedRef<TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>> SharedBegin =
		MakeShared<TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>>(MoveTemp(Begin));

	Flow->QueueWait(Name).BindWeakLambda(Root, [Root, PayloadId, Timeout, Record, SharedBegin](FControlFlowNodeRef NodeRef) -> void
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);

		const FInstancedStruct Local = Root->GetStoredPayload(PayloadId);
		Root->ReleasePayload(PayloadId);

		StartWaitAttempt(Root, NodeRef, Record, Local, Timeout, 1, SharedBegin);
	});

	return this;
}

void UControlFlowBP::StartWaitAttempt(UControlFlowBP* Root, const FControlFlowNodeRef& Node, const TSharedRef<FControlFlowBPStepRecord>& Record,
	const FInstancedStruct& Payload, float Timeout, int32 Attempt, const TSharedRef<TFunction<void(UControlFlowStepHandle&, FControlFlowBPStepRecord&)>>& Begin)
{
	UControlFlowStepHandle* Handle = NewObject<UControlFlowStepHandle>(Root);
	Handle->Init(Node, Root, Record->Path, Payload, Record, Attempt);
	Root->Track(Handle);

	const TWeakObjectPtr<UControlFlowBP> WeakRoot(Root);
	Handle->RestartStep = [WeakRoot, Record, Timeout, Begin](const FControlFlowNodeRef& RestartNode, int32 NextAttempt, const FInstancedStruct& RestartPayload) -> void
	{
		if (UControlFlowBP* LiveRoot = WeakRoot.Get())
		{
			const FControlFlowBPCallbackScope Callback(*LiveRoot);
			StartWaitAttempt(LiveRoot, RestartNode, Record, RestartPayload, Timeout, NextAttempt, Begin);
		}
	};

	if (Attempt > 1)
	{
		Record->Detail = FString::Printf(TEXT("attempt %d of %d"), Attempt, Root->GetStepRetries() + 1);
	}

	if (Root->IsOwnerStale())
	{
		UE_LOG(LogControlFlowBP, Warning, TEXT("Step '%s': the flow's owner was destroyed; aborting."), *Record->Path);
		Handle->AbortWithReason(EControlFlowBPCancelCause::OwnerDestroyed, FString::Printf(TEXT("its owner was destroyed (noticed at step '%s')"), *Record->Path));
		return;
	}

	if (Timeout > 0.f)
	{
		Handle->ArmWatchdog(Timeout);
	}

	(*Begin)(*Handle, *Record);
}

UControlFlowBP* UControlFlowBP::QueueDelay(float Seconds, FString StepName)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(StepName, NAME_None, TEXT("Delay"));
	const float ClampedSeconds = FMath::Max(0.f, Seconds);

	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Delay, Name, FlowPath + TEXT(".") + Name);
	Record->Info = FControlFlowBPDebug::FormatSeconds(ClampedSeconds);

	Flow->QueueWait(Name).BindWeakLambda(Root, [Root, Record, ClampedSeconds](FControlFlowNodeRef Node) -> void
	{
		Root->NotifyStepStarted(*Record);
		UE::ControlFlowBP::Private::ContinueAfter(*Root, Node, ClampedSeconds);
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueSubFlow(FControlFlowPopulate Populate, FString TaskName, const FInstancedStruct& Payload)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(TaskName, Populate.GetFunctionName(), TEXT("SubFlow"));
	const int32 PayloadId = Root->StorePayload(Payload);
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::SubFlow, Name, FlowPath + TEXT(".") + Name, Populate.GetUObject(), Populate.GetFunctionName());

	Flow->QueueControlFlow(Name, Name).BindWeakLambda(Root, [Root, Populate, PayloadId, Record](TSharedRef<FControlFlow> SubFlow) -> void
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);
		const FInstancedStruct Local = Root->GetStoredPayload(PayloadId);
		Root->ReleasePayload(PayloadId);

		UControlFlowBP* Child = UControlFlowBP::MakeChild(SubFlow.Get(), Root, Record->Path, Record);
		if (!Child)
		{
			return;
		}

		Root->ReleaseChildWhenFlowFinishes(Child, SubFlow);
		Child->SetStepPayload(Local);

		if (Populate.IsBound())
		{
			FControlFlowBPDebug::NotifyHandlerStarting(*Record);
			Populate.Execute(Child);
		}
		else
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Sub flow '%s': the step's function no longer exists; the group will be empty."), *Record->Path), *Record);
		}

		Child->CloseScope();
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueIf(FControlFlowCondition Condition, FControlFlowPopulate Then, FControlFlowPopulate Else, FString TaskName)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue If"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(TaskName, Condition.GetFunctionName(), TEXT("If"));
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::If, Name, FlowPath + TEXT(".") + Name, Condition.GetUObject(), Condition.GetFunctionName());
	Record->Info = FString::Printf(TEXT("if %s"), Condition.GetFunctionName().IsNone() ? TEXT("?") : *Condition.GetFunctionName().ToString());

	Flow->QueueControlFlowBranch(Name, Name).BindWeakLambda(Root,
		[Root, Condition, Then, Else, Record](TSharedRef<FControlFlowBranch> Branch) -> int32
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);

		bool bCondition = false;
		if (Condition.IsBound())
		{
			FControlFlowBPDebug::NotifyHandlerStarting(*Record);
			bCondition = Condition.Execute();
		}
		else
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("If '%s': the Condition function no longer exists; taking Else."), *Record->Path), *Record);
		}

		const int32 Key = bCondition ? 1 : 0;
		const FString CaseName = bCondition ? TEXT("Then") : TEXT("Else");
		FControlFlow& CaseFlow = Branch->AddOrGetBranch(Key, CaseName);

		if (UControlFlowBP* Child = UControlFlowBP::MakeChild(CaseFlow, Root, Record->Path + TEXT(".") + CaseName, Record))
		{
			Root->ReleaseChildWhenFlowFinishes(Child, CaseFlow.AsShared());

			const FControlFlowPopulate& Populate = bCondition ? Then : Else;
			if (Populate.IsBound())
			{
				Populate.Execute(Child);
			}

			Child->CloseScope();
		}

		Record->Detail = FString::Printf(TEXT("took %s"), *CaseName);
		if (const FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord())
		{
			Debug->TraceStepDetail(*Record, Record->Detail);
		}

		return Key;
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueIfByName(UObject* Owner, FName ConditionFunction, FName ThenFunction, FName ElseFunction, FString TaskName)
{
	if (!Owner)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue If on '%s': the owner is null."), *FlowPath));
		return this;
	}

	FString Error;
	if (!IsConditionFunction(Owner->FindFunction(ConditionFunction), Error))
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Queue If on '%s': cannot use '%s' on %s as the condition - %s."),
			*FlowPath, *ConditionFunction.ToString(), *GetNameSafe(Owner), *Error));
		return this;
	}

	const auto BindCase = [this, Owner](FName Function, const TCHAR* CaseName, FControlFlowPopulate& OutCase) -> bool
	{
		if (Function.IsNone())
		{
			return true;
		}

		FString Why;
		if (DeduceStepKind(Owner->FindFunction(Function), Why) != EControlFlowStepKind::SubFlow)
		{
			FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
				TEXT("Queue If on '%s': cannot use '%s' on %s as %s - %s."),
				*FlowPath, *Function.ToString(), *GetNameSafe(Owner), CaseName,
				Why.IsEmpty() ? TEXT("a case takes a single Control Flow to queue its steps onto") : *Why));
			return false;
		}

		OutCase.BindUFunction(Owner, Function);
		return true;
	};

	FControlFlowPopulate ThenCase;
	FControlFlowPopulate ElseCase;
	if (!BindCase(ThenFunction, TEXT("Then"), ThenCase) || !BindCase(ElseFunction, TEXT("Else"), ElseCase))
	{
		return this;
	}

	FControlFlowCondition Condition;
	Condition.BindUFunction(Owner, ConditionFunction);

	return QueueIf(Condition, ThenCase, ElseCase, TaskName);
}

UControlFlowBP* UControlFlowBP::QueueWaitUntilByName(UObject* Owner, FName ConditionFunction, FString StepName, float CheckInterval, float TimeoutSeconds)
{
	if (!Owner)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue Wait Until on '%s': the owner is null."), *FlowPath));
		return this;
	}

	FString Error;
	if (!IsConditionFunction(Owner->FindFunction(ConditionFunction), Error))
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Queue Wait Until on '%s': cannot use '%s' on %s as the condition - %s."),
			*FlowPath, *ConditionFunction.ToString(), *GetNameSafe(Owner), *Error));
		return this;
	}

	FControlFlowCondition Condition;
	Condition.BindUFunction(Owner, ConditionFunction);

	return QueueWaitUntil(Condition, StepName, CheckInterval, TimeoutSeconds);
}

UControlFlowBP* UControlFlowBP::QueueBranch(FControlFlowDefineBranch Define, FString TaskName, const FInstancedStruct& Payload)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(TaskName, Define.GetFunctionName(), TEXT("Switch"));
	const int32 PayloadId = Root->StorePayload(Payload);
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Branch, Name, FlowPath + TEXT(".") + Name, Define.GetUObject(), Define.GetFunctionName());

	Flow->QueueControlFlowBranch(Name, Name).BindWeakLambda(Root,
		[Root, Define, PayloadId, Record](TSharedRef<FControlFlowBranch> Branch) -> int32
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);

		UControlFlowBranchScope* Scope = NewObject<UControlFlowBranchScope>(Root);
		Scope->Init(Branch, Root, Record->Path, Root->GetStoredPayload(PayloadId), Record);
		Root->ReleasePayload(PayloadId);
		Root->Track(Scope);

		if (Define.IsBound())
		{
			FControlFlowBPDebug::NotifyHandlerStarting(*Record);
			Define.Execute(Scope);
		}
		else
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Switch '%s': the Define handler no longer exists; the step will do nothing."), *Record->Path), *Record);
		}

		const int32 SelectedKey = Scope->CloseAndResolve();
		Root->Release(Scope);
		return SelectedKey;
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueFork(FControlFlowDefineFork Define, EControlFlowConcurrency Mode, FString TaskName, const FInstancedStruct& Payload)
{
	return QueueForkInternal(Define, Mode, TaskName, Payload, false);
}

UControlFlowBP* UControlFlowBP::QueueRace(FControlFlowDefineFork Define, FString TaskName, const FInstancedStruct& Payload)
{
	return QueueForkInternal(Define, EControlFlowConcurrency::Ordered, TaskName, Payload, true);
}

UControlFlowBP* UControlFlowBP::QueueForkInternal(const FControlFlowDefineFork& Define, EControlFlowConcurrency Mode, const FString& TaskName, const FInstancedStruct& Payload, bool bRace)
{
	const TCHAR* const Label = bRace ? TEXT("Race") : TEXT("Parallel");
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Step"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(TaskName, Define.GetFunctionName(), Label);
	const int32 PayloadId = Root->StorePayload(Payload);
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(bRace ? EControlFlowBPStepType::Race : EControlFlowBPStepType::Fork,
		Name, FlowPath + TEXT(".") + Name, Define.GetUObject(), Define.GetFunctionName());
	Record->Info = bRace
		? TEXT("the first track to finish wins")
		: (Mode == EControlFlowConcurrency::Shuffled ? TEXT("tracks start in random order") : TEXT("tracks start in key order"));

	Flow->QueueConcurrentFlows(Name, Name).BindWeakLambda(Root,
		[Root, Define, Mode, PayloadId, Record, bRace, Label](TSharedRef<FConcurrentControlFlows> Fork) -> void
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		Root->NotifyStepStarted(*Record);

		Fork->SetExecution(Mode == EControlFlowConcurrency::Shuffled
			? EConcurrentExecution::Random
			: EConcurrentExecution::Default);

		UControlFlowForkScope* Scope = NewObject<UControlFlowForkScope>(Root);
		Scope->Init(Fork, Root, Record->Path, Root->GetStoredPayload(PayloadId), Record, bRace);
		Root->ReleasePayload(PayloadId);
		Root->Track(Scope);

		if (Define.IsBound())
		{
			FControlFlowBPDebug::NotifyHandlerStarting(*Record);
			Define.Execute(Scope);
		}
		else
		{
			Record->bHandlerMissing = true;
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("%s '%s': the Define handler no longer exists; the step will do nothing."), Label, *Record->Path), *Record);
		}

		const int32 TrackCount = Scope->CloseAndCountProngs();
		Root->Release(Scope);

		if (TrackCount == 0 && Define.IsBound())
		{
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("%s '%s': no tracks were added."), Label, *Record->Path), *Record);
		}
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueLoop(
	FControlFlowLoopCondition Condition,
	FControlFlowBuildLoop BuildIteration,
	EControlFlowLoopMode Mode,
	FString TaskName,
	const FInstancedStruct& Payload,
	int32 MaxIterations)
{
	return QueueLoopInternal(Condition, BuildIteration, false, Mode, TaskName, Payload, MaxIterations, FControlFlowBPLoopSetup());
}

UControlFlowBP* UControlFlowBP::QueueRepeat(int32 Count, FControlFlowBuildLoop BuildIteration, FString TaskName)
{
	const int32 Times = FMath::Max(0, Count);

	FControlFlowBPLoopSetup Setup;
	Setup.NodeLabel = TEXT("Queue Repeat");
	Setup.NativeCondition = [Times](int32 IterationIndex) -> bool { return IterationIndex < Times; };
	Setup.Info = FString::Printf(TEXT("%d time(s)"), Times);

	return QueueLoopInternal(FControlFlowLoopCondition(), BuildIteration, false, EControlFlowLoopMode::While, TaskName,
		FInstancedStruct(), 0, MoveTemp(Setup));
}

DEFINE_FUNCTION(UControlFlowBP::execQueueForEach)
{
	Stack.MostRecentProperty = nullptr;
	Stack.MostRecentPropertyAddress = nullptr;
	Stack.StepCompiledIn<FArrayProperty>(nullptr);
	const FArrayProperty* ItemsProperty = CastField<FArrayProperty>(Stack.MostRecentProperty);
	const void* Items = Stack.MostRecentPropertyAddress;

	P_GET_PROPERTY(FDelegateProperty, BuildIteration);
	P_GET_PROPERTY(FStrProperty, TaskName);
	P_FINISH;

	UControlFlowBP* Result = nullptr;
	P_NATIVE_BEGIN;
	FControlFlowBuildLoop TypedBuildIteration;
	TypedBuildIteration.BindUFunction(BuildIteration.GetUObject(), BuildIteration.GetFunctionName());
	Result = P_THIS->QueueForEachInternal(ItemsProperty, Items, TypedBuildIteration, TaskName);
	P_NATIVE_END;

	*static_cast<UControlFlowBP**>(RESULT_PARAM) = Result;
}

UControlFlowBP* UControlFlowBP::QueueForEachInternal(const FArrayProperty* ItemsProperty, const void* Items, const FControlFlowBuildLoop& BuildIteration, const FString& TaskName)
{
	using namespace UE::ControlFlowBP::PropertyUtils;

	if (!ItemsProperty || !Items)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Queue For Each on '%s': Items could not be read."), *FlowPath));
		return this;
	}

	static const FName ItemsName(TEXT("Items"));
	FControlFlowBPLoopSetup Setup;
	const FPropertyBagPropertyDesc ItemsDesc(ItemsName, ItemsProperty);
	if (ItemsDesc.ValueType != EPropertyBagPropertyType::None)
	{
		Setup.Items.AddProperties({ ItemsDesc });
	}

	const FPropertyBagPropertyDesc* Stored = Setup.Items.FindPropertyDescByName(ItemsName);
	if (!Stored || !Stored->CachedProperty)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Queue For Each on '%s': a %s cannot be looped over."), *FlowPath, *DescribeType(*ItemsProperty)));
		return this;
	}

	CopyValue(*Stored->CachedProperty, Stored->CachedProperty->ContainerPtrToValuePtr<void>(Setup.Items.GetMutableValue().GetMemory()), *ItemsProperty, Items);

	const int32 Count = FScriptArrayHelper(ItemsProperty, Items).Num();
	Setup.NodeLabel = TEXT("Queue For Each");
	Setup.NativeCondition = [Count](int32 IterationIndex) -> bool { return IterationIndex < Count; };
	Setup.Info = FString::Printf(TEXT("for each of %d item(s)"), Count);

	return QueueLoopInternal(FControlFlowLoopCondition(), BuildIteration, false, EControlFlowLoopMode::While, TaskName,
		FInstancedStruct(), 0, MoveTemp(Setup));
}

UControlFlowBP* UControlFlowBP::QueueLoopInternal(
	const FControlFlowLoopCondition& Condition,
	const FControlFlowBuildLoop& BuildIteration,
	bool bCombinedEvent,
	EControlFlowLoopMode Mode,
	const FString& TaskName,
	const FInstancedStruct& Payload,
	int32 MaxIterations,
	FControlFlowBPLoopSetup&& Setup)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(Setup.NodeLabel);
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(TaskName, BuildIteration.GetFunctionName(), TEXT("Loop"));
	const int32 PayloadId = Root->StorePayload(Payload);
	const int32 ItemsId = Setup.Items.IsValid() ? Root->StorePayload(FInstancedStruct::Make(Setup.Items)) : 0;
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Loop, Name, FlowPath + TEXT(".") + Name, BuildIteration.GetUObject(), BuildIteration.GetFunctionName());
	Record->Info = !Setup.Info.IsEmpty() ? Setup.Info : FString::Printf(TEXT("%s%s"),
		Mode == EControlFlowLoopMode::While ? TEXT("while") : TEXT("do-while"),
		MaxIterations > 0 ? *FString::Printf(TEXT(", at most %d iterations"), MaxIterations) : TEXT(""));

	TSharedRef<TWeakObjectPtr<UControlFlowLoopScope>> ScopeRef = MakeShared<TWeakObjectPtr<UControlFlowLoopScope>>();
	TFunction<bool(int32)> NativeCondition = MoveTemp(Setup.NativeCondition);

	Flow->QueueConditionalLoop(Name, Name).BindWeakLambda(Root,
		[Root, Condition, BuildIteration, bCombinedEvent, Mode, PayloadId, ItemsId, MaxIterations, NativeCondition, ScopeRef, Record]
		(TSharedRef<FConditionalLoop> Loop) -> EConditionalLoopResult
	{
		const FControlFlowBPCallbackScope Callback(*Root);

		FControlFlow& BodyFlow = Loop->SetCheckConditionFirst(Mode == EControlFlowLoopMode::While);

		UControlFlowLoopScope* Scope = ScopeRef->Get();
		const bool bFirstCall = (Scope == nullptr);
		if (bFirstCall)
		{
			Root->NotifyStepStarted(*Record);

			UControlFlowBP* BodyBuilder = UControlFlowBP::MakeChild(BodyFlow, Root, Record->Path + TEXT("#1"), Record);

			Scope = NewObject<UControlFlowLoopScope>(Root);
			Scope->Init(Root, BodyBuilder, Record->Path, Root->GetStoredPayload(PayloadId), Record);
			Root->ReleasePayload(PayloadId);

			if (ItemsId != 0)
			{
				const FInstancedStruct StoredItems = Root->GetStoredPayload(ItemsId);
				Root->ReleasePayload(ItemsId);

				if (const FInstancedPropertyBag* Items = StoredItems.GetPtr<FInstancedPropertyBag>())
				{
					Scope->SetItems(*Items);
				}
			}

			Root->Track(Scope);
			*ScopeRef = Scope;
		}

		UControlFlowBP* BodyBuilder = Scope->GetBodyBuilder();

		bool bOfferedBreak = false;
		const auto OfferBreak = [&bOfferedBreak, &Record]() -> void
		{
			if (!bOfferedBreak)
			{
				bOfferedBreak = true;
				FControlFlowBPDebug::NotifyHandlerStarting(*Record);
			}
		};

		const auto StopAtLimit = [Root, &Record, MaxIterations]() -> EConditionalLoopResult
		{
			const FString Reason = FString::Printf(TEXT("hit Max Iterations (%d)"), MaxIterations);
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Error,
				FString::Printf(TEXT("Loop '%s' %s and was stopped. Check that its condition can become false."), *Record->Path, *Reason), *Record);
			Root->ReportStepFailed(*Record, Reason);

			if (FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord(); Debug && FControlFlowBPDebug::ShouldDumpOnFailure())
			{
				Debug->Dump(ELogVerbosity::Warning);
			}

			return EConditionalLoopResult::LoopFinished;
		};

		const bool bFirstAnswerIgnored = bFirstCall && Mode == EControlFlowLoopMode::DoWhile;

		if (bCombinedEvent)
		{
			Scope->BeginCombinedPhase();

			BodyBuilder->QueueLoopYield();

			if (BuildIteration.IsBound())
			{
				OfferBreak();
				BuildIteration.Execute(Scope);
			}
			else
			{
				FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
					FString::Printf(TEXT("Loop '%s': the step's handler no longer exists; stopping."), *Record->Path), *Record);
			}

			const bool bRunAgain = Scope->EndCombinedPhase();
			const bool bOverLimit = bRunAgain && !bFirstAnswerIgnored && MaxIterations > 0 && Scope->GetIterationCount() > MaxIterations;

			if ((bRunAgain || bFirstAnswerIgnored) && !bOverLimit)
			{
				++Record->Iterations;
			}
			else if (FControlFlowBPFlowRecord* Debug = Root->GetDebugRecord(); Debug && BodyBuilder && BodyBuilder->GetDebugLane().IsValid())
			{
				Debug->SkipLane(*BodyBuilder->GetDebugLane(), TEXT("built on the loop's final call, which ended the loop"));
			}

			if (bOverLimit)
			{
				return StopAtLimit();
			}

			return bRunAgain ? EConditionalLoopResult::RunLoop : EConditionalLoopResult::LoopFinished;
		}

		if (!bFirstAnswerIgnored)
		{
			bool bRunAgain = false;
			if (NativeCondition)
			{
				bRunAgain = NativeCondition(Scope->GetIterationCount());
			}
			else
			{
				Scope->BeginConditionPhase();

				if (Condition.IsBound())
				{
					OfferBreak();
					Condition.Execute(Scope);
				}
				else
				{
					FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
						FString::Printf(TEXT("Loop '%s': the Condition handler no longer exists; stopping."), *Record->Path), *Record);
				}

				bRunAgain = Scope->EndConditionPhase();
			}

			if (!bRunAgain)
			{
				return EConditionalLoopResult::LoopFinished;
			}

			if (MaxIterations > 0 && Scope->GetIterationCount() >= MaxIterations)
			{
				return StopAtLimit();
			}
		}

		Scope->BeginBuildPhase();

		BodyBuilder->QueueLoopYield();

		if (BuildIteration.IsBound())
		{
			OfferBreak();
			BuildIteration.Execute(Scope);
		}
		else
		{
			FControlFlowBPDebug::ReportStepIssue(ELogVerbosity::Warning,
				FString::Printf(TEXT("Loop '%s': the Build Iteration handler no longer exists; the body will be empty."), *Record->Path), *Record);
		}

		Scope->EndBuildPhase();
		++Record->Iterations;
		return EConditionalLoopResult::RunLoop;
	});

	return this;
}

UControlFlowBP* UControlFlowBP::QueueSetCancelledStepAsComplete(bool bCancelledStepIsComplete, FString StepName)
{
	TSharedPtr<FControlFlow> Flow = BeginQueue(TEXT("Queue Set Cancelled Step As Complete"));
	if (!Flow.IsValid())
	{
		return this;
	}

	UControlFlowBP* Root = GetRoot();
	const FString Name = MakeStepName(StepName, NAME_None, TEXT("SetCancelledStepAsComplete"));
	const TSharedRef<FControlFlowBPStepRecord> Record = RecordStep(EControlFlowBPStepType::Setting, Name, FlowPath + TEXT(".") + Name);
	Record->Info = bCancelledStepIsComplete
		? TEXT("from here on a cancelled step counts as complete")
		: TEXT("from here on a cancelled step cancels the flow");

	const TWeakPtr<FControlFlow> WeakFlow = Flow;
	Flow->QueueFunction(Name).BindWeakLambda(Root, [Root, Record, WeakFlow, bCancelledStepIsComplete]() -> void
	{
		Root->NotifyStepStarted(*Record);

		if (const TSharedPtr<FControlFlow> PinnedFlow = WeakFlow.Pin())
		{
			PinnedFlow->SetCancelledNodeAsComplete(bCancelledStepIsComplete);
		}
	});

	return this;
}

UControlFlowBP* UControlFlowBP::EnableActivityTracking()
{
	TSharedPtr<FControlFlow> Flow = FlowWeak.Pin();
	if (Flow.IsValid())
	{
		Flow->TrackActivities();
	}

	return this;
}

UControlFlowBP* UControlFlowBP::EnableStepTrace()
{
	if (FControlFlowBPFlowRecord* Debug = GetDebugRecord())
	{
		Debug->bTraceSteps = true;
	}

	return this;
}

void UControlFlowBP::ExecuteFlow()
{
	if (!bIsRoot)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(
			TEXT("Execute Flow on '%s': only the flow from Create Control Flow can be executed. The engine starts sub-flows, cases, tracks and loop bodies for you."),
			*FlowPath));
		return;
	}

	if (bHasExecuted)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Execute Flow on '%s': this flow has already been started."), *FlowPath));
		return;
	}

	if (bInFlowCallback)
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Execute Flow on '%s': cannot be called from inside a step of the same flow."), *FlowPath));
		return;
	}

	if (!OwnedFlow.IsValid())
	{
		FControlFlowBPDebug::ReportCallerIssue(ELogVerbosity::Error, FString::Printf(TEXT("Execute Flow on '%s': the underlying flow no longer exists."), *FlowPath));
		return;
	}

	bHasExecuted = true;

	if (UControlFlowBPSubsystem* Subsystem = UControlFlowBPSubsystem::Get())
	{
		Subsystem->Retain(this);
	}

	UE_LOG(LogControlFlowBP, Verbose, TEXT("Executing flow '%s' (%d step(s) queued)."), *FlowPath, NumInQueue());

	if (DebugRecord.IsValid())
	{
		DebugRecord->NotifyFlowStarted(FControlFlowBPCallSite::Capture());
	}

	OwnedFlow->ExecuteFlow();
}

void UControlFlowBP::CancelEntireFlow()
{
	const FControlFlowBPCallSite Site = FControlFlowBPCallSite::Capture();
	CancelWithReason(EControlFlowBPCancelCause::Requested, Site.IsSet()
		? FString::Printf(TEXT("Cancel Flow was called from %s"), *Site.Describe())
		: FString(TEXT("Cancel Flow was called")));
}

void UControlFlowBP::CancelWithReason(EControlFlowBPCancelCause Cause, const FString& Reason)
{
	UControlFlowBP* Root = GetRoot();
	if (!Root || Root->bFinished)
	{
		return;
	}

	if (Root->bInFlowCallback)
	{
		const TWeakObjectPtr<UControlFlowBP> WeakRoot(Root);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakRoot, Cause, Reason](float) -> bool
		{
			if (UControlFlowBP* Live = WeakRoot.Get())
			{
				Live->CancelWithReason(Cause, Reason);
			}
			return false;
		}), 0.f);
		return;
	}

	FControlFlowBPFlowRecord* Debug = Root->DebugRecord.Get();
	if (Debug)
	{
		Debug->SetCancelReason(Cause, Reason);
	}

	if (!Root->bHasExecuted || !Root->OwnedFlow.IsValid())
	{
		Root->HandleFinished(true);
		return;
	}

	if (Debug)
	{
		Debug->MarkRunningStepsCancelRequested();
	}

	Root->OwnedFlow->CancelFlow();

	if (Debug && Debug->State != EControlFlowBPFlowState::Cancelled)
	{
		UE_LOG(LogControlFlowBP, Warning,
			TEXT("Cancel Flow on '%s' did not stop it: Set Cancelled Step As Complete is on, so the cancelled step counted as complete and the flow carried on."),
			*Root->FlowPath);

		if (!Debug->IsFinished())
		{
			Debug->CancelCause = EControlFlowBPCancelCause::None;
			Debug->CancelReason.Reset();
		}
	}
}

bool UControlFlowBP::IsRunning() const
{
	TSharedPtr<FControlFlow> Flow = FlowWeak.Pin();
	return Flow.IsValid() && Flow->IsRunning();
}

bool UControlFlowBP::IsFinished() const
{
	const UControlFlowBP* Root = GetRoot();
	return !Root || Root->bFinished;
}

int32 UControlFlowBP::NumInQueue() const
{
	TSharedPtr<FControlFlow> Flow = FlowWeak.Pin();
	return Flow.IsValid() ? static_cast<int32>(Flow->NumInQueue()) : 0;
}

FString UControlFlowBP::GetFlowDebugName() const
{
	return DebugName;
}

FString UControlFlowBP::GetFlowPath() const
{
	return FlowPath;
}

FString UControlFlowBP::GetCurrentStepPath() const
{
	const FControlFlowBPFlowRecord* Debug = GetDebugRecord();
	if (!Debug)
	{
		return FString();
	}

	TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
	Debug->GetActiveSteps(Active, true);

	const FControlFlowBPStepRecord* Latest = nullptr;
	for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
	{
		if (!Latest || Step->StartTime >= Latest->StartTime)
		{
			Latest = &Step.Get();
		}
	}

	return Latest ? Latest->Path : FString();
}

TArray<FString> UControlFlowBP::GetActiveStepPaths() const
{
	TArray<FString> Paths;

	if (const FControlFlowBPFlowRecord* Debug = GetDebugRecord())
	{
		TArray<TSharedRef<FControlFlowBPStepRecord>> Active;
		Debug->GetActiveSteps(Active, true);

		for (const TSharedRef<FControlFlowBPStepRecord>& Step : Active)
		{
			Paths.Add(Step->Path);
		}
	}

	return Paths;
}

FString UControlFlowBP::GetCancelReason() const
{
	const FControlFlowBPFlowRecord* Debug = GetDebugRecord();
	return (Debug && Debug->State == EControlFlowBPFlowState::Cancelled) ? Debug->CancelReason : FString();
}

void UControlFlowBP::DumpFlow() const
{
	if (const FControlFlowBPFlowRecord* Debug = GetDebugRecord())
	{
		Debug->Dump(ELogVerbosity::Log);
	}
	else
	{
		UE_LOG(LogControlFlowBP, Log, TEXT("--- Control Flow '%s': its root flow no longer exists ---"), *FlowPath);
	}
}
