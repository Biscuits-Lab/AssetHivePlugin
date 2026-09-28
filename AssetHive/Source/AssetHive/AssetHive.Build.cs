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
            "RenderCore",
            "RHI",
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
            "MeshDescription",
            "StaticMeshDescription",
            "StaticMeshEditor",
            "ToolMenus",
            "InputCore",
            "Sockets",
            "Networking"
        });
    }
}
