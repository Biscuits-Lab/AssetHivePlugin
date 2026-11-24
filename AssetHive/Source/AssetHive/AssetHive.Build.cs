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
            "AssetHiveRuntime",
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
            "PropertyEditor",
            "EditorFramework",
            "Foliage",
            "Json",
            "JsonUtilities",
            "PythonScriptPlugin",
            "LevelEditor",
            "MaterialEditor",
            "UnrealEd",
            "HairStrandsCore",
            "HairStrandsEditor",
            "MeshDescription",
            "StaticMeshDescription",
            "SkeletalMeshDescription",
            "MeshConversion",
            "DynamicMesh",
            "GeometryCore",
            "AnimationCore",
            "MeshReductionInterface",
            "MeshUtilitiesCommon",
            "StaticMeshEditor",
            "SkeletalMeshUtilitiesCommon",
            "TargetPlatform",
            "ToolMenus",
            "InputCore",
            "Sockets",
            "Networking"
        });
    }
}
