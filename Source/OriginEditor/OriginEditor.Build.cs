using UnrealBuildTool;
public class OriginEditor : ModuleRules
{
    public OriginEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Origin", "UnrealEd", "Slate", "SlateCore",
            "InputCore", "LevelEditor", "SceneOutliner", "PropertyEditor", "ToolMenus",
            "Projects", "AssetRegistry", "AssetTools"
        });
    }
}
