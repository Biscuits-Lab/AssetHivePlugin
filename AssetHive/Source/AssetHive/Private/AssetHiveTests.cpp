#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AssetHiveSettings.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveSettingsTest, "AssetHive.Import.Settings",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveSettingsTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("Default"), UAssetHiveSettings::NormalizeImportRootPath(TEXT("")), FString(TEXT("/Game/AssetHive")));
    TestEqual(TEXT("Content syntax"), UAssetHiveSettings::NormalizeImportRootPath(TEXT(" Content/Nature/Trees/ ")), FString(TEXT("/Game/Nature/Trees")));
    TestEqual(TEXT("Package syntax"), UAssetHiveSettings::NormalizeImportRootPath(TEXT("/Game/Custom")), FString(TEXT("/Game/Custom")));
    TestEqual(TEXT("Windows separators"), UAssetHiveSettings::NormalizeImportRootPath(TEXT("Content\\Nature")), FString(TEXT("/Game/Nature")));
    TestFalse(TEXT("Traversal rejected"), UAssetHiveSettings::IsValidImportRootPath(TEXT("/Game/../Outside")));
    TestFalse(TEXT("Disk path rejected"), UAssetHiveSettings::IsValidImportRootPath(UAssetHiveSettings::NormalizeImportRootPath(TEXT("D:/Assets"))));
    TestFalse(TEXT("Other mount rejected"), UAssetHiveSettings::IsValidImportRootPath(TEXT("/Engine/Assets")));
    TestEqual(TEXT("Settings category"), GetDefault<UAssetHiveSettings>()->GetCategoryName(), FName(TEXT("Plugins")));
    TestEqual(TEXT("Surface parent default"), UAssetHiveSettings::GetSurfaceParentMaterialPath(), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Tiling/MI_Env_Tiling_VT_Simple.MI_Env_Tiling_VT_Simple")));
    TestTrue(TEXT("Surface parent valid"), UAssetHiveSettings::IsValidSurfaceParentMaterialPath(UAssetHiveSettings::GetSurfaceParentMaterialPath()));
    TestFalse(TEXT("Surface parent rejects invalid path"), UAssetHiveSettings::IsValidSurfaceParentMaterialPath(TEXT("Content/Material/MI_Test")));
    TestEqual(TEXT("Surface MI name"), UAssetHiveSettings::GetSurfaceMaterialName(TEXT("Moss_Rock"), 1), FString(TEXT("MI_Env_Tile_Moss_Rock")));
    TestEqual(TEXT("Surface MI group name"), UAssetHiveSettings::GetSurfaceMaterialName(TEXT("Moss_Rock"), 2), FString(TEXT("MI_Env_Tile_Moss_Rock_002")));
    TestEqual(TEXT("Decal parent default"), UAssetHiveSettings::GetDecalParentMaterialPath(), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_VT.MI_Env_Decal_VT")));
    TestEqual(TEXT("Decal POM parent default"), UAssetHiveSettings::GetDecalParentMaterialPath(TEXT("pomDecal")), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_VT.MI_Env_POMDecal_VT")));
    TestEqual(TEXT("Decal triplanar parent default"), UAssetHiveSettings::GetDecalParentMaterialPath(TEXT("decalTri")), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_Tri_VT.MI_Env_Decal_Tri_VT")));
    TestEqual(TEXT("Decal triplanar POM parent default"), UAssetHiveSettings::GetDecalParentMaterialPath(TEXT("pomDecalTri")), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_Tri_VT.MI_Env_POMDecal_Tri_VT")));
    TestEqual(TEXT("Decal parent mode alias"), UAssetHiveSettings::NormalizeDecalParentMode(TEXT("triplanar_pom")), FString(TEXT("pomDecalTri")));
    TestTrue(TEXT("Decal parent valid"), UAssetHiveSettings::IsValidDecalParentMaterialPath(UAssetHiveSettings::GetDecalParentMaterialPath()));
    TestEqual(TEXT("Decal MI name"), UAssetHiveSettings::GetDecalMaterialName(TEXT("LeakingRust_abc123")), FString(TEXT("MI_Env_Decal_LeakingRust_abc123")));
    TestEqual(TEXT("Decal diffuse parameter"), UAssetHiveSettings::GetDecalDiffuseParameter(), FString(TEXT("Diffuse_VT")));
    TestEqual(TEXT("Decal normal parameter"), UAssetHiveSettings::GetDecalNormalParameter(), FString(TEXT("Normal_VT")));
    TestEqual(TEXT("Decal opacity parameter"), UAssetHiveSettings::GetDecalOpacityParameter(), FString(TEXT("OpacityMasked_VT")));
    TestEqual(TEXT("Decal ORM parameter"), UAssetHiveSettings::GetDecalORMTextureParameter(), FString(TEXT("ORM_VT")));
    TestEqual(TEXT("Decal displacement parameter"), UAssetHiveSettings::GetDecalDisplacementParameter(), FString(TEXT("Displacement")));
    TestEqual(TEXT("Decal opacity switch"), UAssetHiveSettings::GetDecalUseOpacityTextureSwitch(), FString(TEXT("Opacity_UseOpacityTex")));
    TestEqual(TEXT("3D base parent default"), UAssetHiveSettings::GetAssetParentMaterialPath(TEXT("Objects"), false), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_VT_Simple.MI_Env_Base_VT_Simple")));
    TestEqual(TEXT("3D masked parent default"), UAssetHiveSettings::GetAssetParentMaterialPath(TEXT("Props"), true), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_Masked_VT_Simple.MI_Env_Base_Masked_VT_Simple")));
    TestEqual(TEXT("3D destructible parent default"), UAssetHiveSettings::GetAssetParentMaterialPath(TEXT("Destructible"), true), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Destructible/MI_Env_Destructible_VT_UV.MI_Env_Destructible_VT_UV")));
    TestEqual(TEXT("3D MEGA parent ignores masked"), UAssetHiveSettings::GetAssetParentMaterialPath(TEXT("MEGA"), true), FString(TEXT("/Game/MaterialLibrary/Environment/MI_Mega/MI_Env_Mega.MI_Env_Mega")));
    TestEqual(TEXT("3D diffuse parameter"), UAssetHiveSettings::GetAssetAlbedoParameter(), FString(TEXT("Diffuse_VT")));
    TestEqual(TEXT("3D normal parameter"), UAssetHiveSettings::GetAssetNormalParameter(), FString(TEXT("Normal_VT")));
    TestEqual(TEXT("3D ORM parameter"), UAssetHiveSettings::GetAssetORMTextureParameter(), FString(TEXT("ORM_VT")));
    TestEqual(TEXT("3D base masked parameter"), UAssetHiveSettings::GetAssetMaskParameter(), FString(TEXT("Mask_VT")));
    TestEqual(TEXT("3D MEGA mask parameter"), UAssetHiveSettings::GetAssetMegaMaskParameter(), FString(TEXT("Mega Mask_UV2")));
    TestEqual(TEXT("3D emissive parameter"), UAssetHiveSettings::GetAssetEmissiveParameter(TEXT("Objects")), FString(TEXT("Emissive_Tex_VT")));
    TestEqual(TEXT("3D destructible emissive parameter"), UAssetHiveSettings::GetAssetEmissiveParameter(TEXT("Destructible")), FString(TEXT("Emissive_Tex")));
    TestEqual(TEXT("3D emissive switch"), UAssetHiveSettings::GetAssetUseEmissiveSwitch(), FString(TEXT("UseEmissive")));
    TestEqual(TEXT("Plant billboard parent default"), UAssetHiveSettings::GetDefaultPlantBillboardParentMaterialPath(), UAssetHiveSettings::GetDefaultPlantAtlasParentMaterialPath());
    TestTrue(TEXT("Plant atlas parent valid"), UAssetHiveSettings::IsValidPlantParentMaterialPath(UAssetHiveSettings::GetPlantParentMaterialPath(false)));
    TestTrue(TEXT("Plant billboard parent valid"), UAssetHiveSettings::IsValidPlantParentMaterialPath(UAssetHiveSettings::GetPlantParentMaterialPath(true)));
    TestEqual(TEXT("Plant atlas MI name"), UAssetHiveSettings::GetPlantMaterialName(TEXT("Fern_001"), false), FString(TEXT("MI_Fern_001")));
    TestEqual(TEXT("Plant billboard MI name"), UAssetHiveSettings::GetPlantMaterialName(TEXT("Fern_001"), true), FString(TEXT("MI_Billboard_Fern_001")));
    TestEqual(TEXT("Plant diffuse parameter"), UAssetHiveSettings::GetPlantDiffuseParameter(), FString(TEXT("Diffuse_VT")));
    TestEqual(TEXT("Plant normal parameter"), UAssetHiveSettings::GetPlantNormalParameter(), FString(TEXT("Normal_VT")));
    TestEqual(TEXT("Plant ORM parameter"), UAssetHiveSettings::GetPlantORMParameter(), FString(TEXT("ORM_VT")));
    TestEqual(TEXT("Plant opacity masked parameter"), UAssetHiveSettings::GetPlantOpacityMaskedParameter(), FString(TEXT("OpacityMasked_VT")));
    TestEqual(TEXT("Plant opacity masked switch"), UAssetHiveSettings::GetPlantUseOpacityMaskedSwitch(), FString(TEXT("Use OpacityMasked")));

    UAssetHiveSettings* Defaults = GetMutableDefault<UAssetHiveSettings>();
    const FString Original = Defaults->ImportRootPath;
    const FString OriginalSurfaceBCR = Defaults->SurfaceBCRParameter;
    Defaults->ImportRootPath = TEXT("Content/First");
    TestEqual(TEXT("First import uses current setting"), UAssetHiveSettings::GetImportRootPath(), FString(TEXT("/Game/First")));
    Defaults->ImportRootPath = TEXT("Content/Second");
    TestEqual(TEXT("Next import picks up edits immediately"), UAssetHiveSettings::GetImportRootPath(), FString(TEXT("/Game/Second")));
    const FString TestConfig = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/AssetHiveSettingsTest.ini")));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(TestConfig), true);
    Defaults->SurfaceBCRParameter = TEXT("BCR_Custom");
    Defaults->SaveConfig(CPF_Config, *TestConfig);
    Defaults->ImportRootPath = Original;
    Defaults->SurfaceBCRParameter = OriginalSurfaceBCR;
    Defaults->LoadConfig(Defaults->GetClass(), *TestConfig);
    TestEqual(TEXT("Config persists"), Defaults->ImportRootPath, FString(TEXT("Content/Second")));
    TestEqual(TEXT("Surface settings persist"), Defaults->SurfaceBCRParameter, FString(TEXT("BCR_Custom")));
    Defaults->ImportRootPath = Original;
    Defaults->SurfaceBCRParameter = OriginalSurfaceBCR;
    GConfig->UnloadFile(TestConfig);
    IFileManager::Get().Delete(*TestConfig);
    return true;
}
#endif
