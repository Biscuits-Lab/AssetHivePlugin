using UnrealBuildTool;

public class AssetHive : ModuleRules
{
    public AssetHive(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "DeveloperSettings",
            "Projects",
            "Slate",
            "SlateCore"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "AssetTools",
            "AssetRegistry",
            "ContentBrowser",
            "EditorFramework",
            "Foliage",
            "Json",
            "JsonUtilities",
            "LevelEditor",
            "MaterialEditor",
            "UnrealEd",
            "ToolMenus",
            "InputCore",
            "Sockets",
            "Networking"
        });
    }
}
