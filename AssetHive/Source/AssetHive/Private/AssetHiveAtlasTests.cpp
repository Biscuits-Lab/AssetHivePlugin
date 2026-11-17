#if WITH_DEV_AUTOMATION_TESTS
#include "AssetHiveImportCommandlet.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GCObjectScopeGuard.h"

// Supply a normal desktop export Job in an isolated project with Base parents.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveAtlasTextureOnlyTest,
    "AssetHive.Import.AtlasTextureOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveAtlasTextureOnlyTest::RunTest(const FString& Parameters) {
  FString File, Json;
  if (!FParse::Value(FCommandLine::Get(), TEXT("AssetHiveAtlasValidationJob="), File)) {
    AddInfo(TEXT("Supply -AssetHiveAtlasValidationJob=<job> in a scratch project with masked and opaque Atlas fixtures."));
    return true;
  }
  TSharedPtr<FJsonObject> Job;
  if (!TestTrue(TEXT("Read Atlas job"), FFileHelper::LoadFileToString(Json, *File) &&
      FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Job))) return false;
  auto* Importer = NewObject<UAssetHiveImportCommandlet>();
  FGCObjectScopeGuard Guard(Importer);
  auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
  TSet<FString> FirstPaths;
  for (int32 Pass = 0; Pass < 2; ++Pass) {
    TArray<FString> Folders;
    if (!TestEqual(TEXT("Import Atlas job"), Importer->ImportJob(Job, TEXT("/Game/AtlasValidation"), {}, &Folders), 0)) return false;
    if (!TestEqual(TEXT("Masked and opaque asset folders"), Folders.Num(), 2)) return false;
    TSet<FString> Paths;
    int32 Materials = 0, Textures = 0, Displacements = 0, MaskedMaterials = 0;
    for (const FString& Folder : Folders) {
      TestTrue(TEXT("Atlas uses the reference ThirdParty object directory"), Folder.StartsWith(TEXT("/Game/Environment/Asset/Objects/ThirdParty/")));
      TArray<FAssetData> Assets;
      Registry.GetAssetsByPath(FName(*Folder), Assets, true);
      for (const FAssetData& Asset : Assets) {
        UObject* Object = Asset.GetAsset();
        if (!TestNotNull(TEXT("Imported asset exists"), Object)) continue;
        Paths.Add(Object->GetPathName());
        if (auto* Material = Cast<UMaterialInstanceConstant>(Object)) {
          ++Materials;
          const bool bMasked = Folder.Contains(TEXT("AtlasMasked_"));
          if (bMasked) ++MaskedMaterials;
          const FString Parent = bMasked ? TEXT("MI_Env_Base_Masked_VT_Simple") : TEXT("MI_Env_Base_VT_Simple");
          TestTrue(TEXT("Uses the corresponding Base parent"), Material->Parent && Material->Parent->GetName() == Parent);
          const auto HasSlot = [&](const TCHAR* Name) {
            return Material->TextureParameterValues.ContainsByPredicate([&](const FTextureParameterValue& Value) {
              return Value.ParameterInfo.Name == FName(Name) && Value.ParameterValue;
            });
          };
          TestTrue(TEXT("Diffuse, Normal and ORM are bound"), HasSlot(TEXT("Diffuse_VT")) && HasSlot(TEXT("Normal_VT")) && HasSlot(TEXT("ORM_VT")));
          TestEqual(TEXT("Opacity uses the Aspen parameter only for masked groups"), HasSlot(TEXT("OpacityMask_VT")), bMasked);
          TestFalse(TEXT("Displacement stays a standalone texture"), HasSlot(TEXT("Displacement")));
        } else if (auto* Texture = Cast<UTexture2D>(Object)) {
          ++Textures;
          if (Texture->GetName().EndsWith(TEXT("_Dis"))) {
            ++Displacements;
            TestFalse(TEXT("Displacement is linear"), Texture->SRGB);
            TestEqual(TEXT("Displacement uses mask compression"), Texture->CompressionSettings, TC_Masks);
          }
        } else {
          AddError(TEXT("Atlas created a non-material/non-texture asset: ") + Object->GetPathName());
        }
      }
    }
    TestEqual(TEXT("One MI per material group"), Materials, 3);
    TestEqual(TEXT("Two groups use independent masked MIs"), MaskedMaterials, 2);
    TestEqual(TEXT("Only the requested texture assets exist"), Textures, 14);
    TestEqual(TEXT("Every group retains its independent displacement"), Displacements, 3);
    if (Pass == 0) FirstPaths = Paths;
    else {
      bool bSamePaths = Paths.Num() == FirstPaths.Num();
      for (const FString& Path : Paths) bSamePaths &= FirstPaths.Contains(Path);
      TestTrue(TEXT("Reimport reuses asset paths without duplicates or legacy meshes"), bSamePaths);
    }
  }
  return true;
}
#endif
