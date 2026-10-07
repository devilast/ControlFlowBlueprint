using UnrealBuildTool;

public class ControlFlowBP : ModuleRules
{
	public ControlFlowBP(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",

				"ControlFlows",
			});

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UMG",
			});
	}
}
