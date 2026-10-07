#pragma once

#include "CoreMinimal.h"
#include "BlueprintCompilerExtension.h"
#include "ControlFlowBPCompilerChecks.generated.h"

/**
 * Compile warnings for the mistakes the runtime can only report once a flow is already stuck: a
 * Wait event that never resolves its handle, and a Switch event that never selects a case.
 *
 * Only a handle that is never passed anywhere is flagged. One handed to a function, stored in a
 * variable or wired into a macro may well be resolved there, and is left alone.
 */
UCLASS()
class UControlFlowBPCompilerChecks : public UBlueprintCompilerExtension
{
	GENERATED_BODY()

protected:
	virtual void ProcessBlueprintCompiled(const FKismetCompilerContext& CompilationContext, const FBlueprintCompiledData& Data) override;
};
