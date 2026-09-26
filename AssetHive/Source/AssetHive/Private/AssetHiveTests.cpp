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
    TestEqual(TEXT("Plant atlas parent default"), UAssetHiveSettings::GetDefaultPlantAtlasParentMaterialPath(), FString(TEXT("/Game/Common/MaterialInstance/MMI_Grass.MMI_Grass")));
    TestEqual(TEXT("Plant billboard parent default"), UAssetHiveSettings::GetDefaultPlantBillboardParentMaterialPath(), UAssetHiveSettings::GetDefaultPlantAtlasParentMaterialPath());
    TestTrue(TEXT("Plant atlas parent valid"), UAssetHiveSettings::IsValidPlantParentMaterialPath(UAssetHiveSettings::GetPlantParentMaterialPath(false)));
    TestTrue(TEXT("Plant billboard parent valid"), UAssetHiveSettings::IsValidPlantParentMaterialPath(UAssetHiveSettings::GetPlantParentMaterialPath(true)));
    TestEqual(TEXT("Plant atlas MI name"), UAssetHiveSettings::GetPlantMaterialName(TEXT("Fern_001"), false), FString(TEXT("MI_Fern_001")));
    TestEqual(TEXT("Plant billboard MI name"), UAssetHiveSettings::GetPlantMaterialName(TEXT("Fern_001"), true), FString(TEXT("MI_Billboard_Fern_001")));
    TestEqual(TEXT("Plant albedo parameter"), UAssetHiveSettings::GetPlantAlbedoParameter(), FString(TEXT("Albedo")));
    TestEqual(TEXT("Plant NRS parameter"), UAssetHiveSettings::GetPlantNRSParameter(), FString(TEXT("NRS")));

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
