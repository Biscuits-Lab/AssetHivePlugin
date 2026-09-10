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

    UAssetHiveSettings* Defaults = GetMutableDefault<UAssetHiveSettings>();
    const FString Original = Defaults->ImportRootPath;
    Defaults->ImportRootPath = TEXT("Content/First");
    TestEqual(TEXT("First import uses current setting"), UAssetHiveSettings::GetImportRootPath(), FString(TEXT("/Game/First")));
    Defaults->ImportRootPath = TEXT("Content/Second");
    TestEqual(TEXT("Next import picks up edits immediately"), UAssetHiveSettings::GetImportRootPath(), FString(TEXT("/Game/Second")));
    const FString TestConfig = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation/AssetHiveSettingsTest.ini")));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(TestConfig), true);
    Defaults->SaveConfig(CPF_Config, *TestConfig);
    Defaults->ImportRootPath = Original;
    Defaults->LoadConfig(Defaults->GetClass(), *TestConfig);
    TestEqual(TEXT("Config persists"), Defaults->ImportRootPath, FString(TEXT("Content/Second")));
    Defaults->ImportRootPath = Original;
    GConfig->UnloadFile(TestConfig);
    IFileManager::Get().Delete(*TestConfig);
    return true;
}
#endif
