using UnrealBuildTool;
public class Origin : ModuleRules
{
    public Origin(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        if (Target.bBuildEditor)
            PrivateDependencyModuleNames.Add("Projects");
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine" });
    }
}
