using UnrealBuildTool;

public class ControlFlowBPEditor : ModuleRules
{
	public ControlFlowBPEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Slate",
				"SlateCore",
				"InputCore",
				"UnrealEd",
				"GraphEditor",
				"BlueprintGraph",
				"Kismet",
				"KismetCompiler",
				"ToolMenus",
				"ToolWidgets",
				"WorkspaceMenuStructure",
				"ApplicationCore",

				"ControlFlowBP",
				"ControlFlowBPUncooked",
			});
	}
}
