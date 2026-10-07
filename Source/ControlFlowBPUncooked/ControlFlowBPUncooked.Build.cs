using UnrealBuildTool;

public class ControlFlowBPUncooked : ModuleRules
{
	public ControlFlowBPUncooked(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",

				"BlueprintGraph",
				"ControlFlowBP",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"KismetCompiler",
				"UnrealEd",
				"SlateCore",
			});
	}
}
