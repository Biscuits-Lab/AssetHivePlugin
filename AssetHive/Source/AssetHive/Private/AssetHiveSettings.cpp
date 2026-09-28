#include "AssetHiveSettings.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

UAssetHiveSettings::UAssetHiveSettings()
{
    ImportRootPath = TEXT("Content/AssetHive");
    SurfaceParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Tiling/MI_Env_Tiling_VT_Simple.MI_Env_Tiling_VT_Simple")));
    SurfaceMaterialNamePrefix = TEXT("MI_Env_Tile_");
    SurfaceBCRParameter = TEXT("BCR_VT");
    SurfaceNormalParameter = TEXT("NRM_VT");
    SurfaceMetallicParameter = TEXT("Metallic_VT");
    SurfaceEmissiveParameter = TEXT("Emissive_Tex_VT");
    SurfaceUseMetallicSwitch = TEXT("UseMetallicTex");
    SurfaceUseEmissiveSwitch = TEXT("UseEmissive");
    SurfaceTilingParameter = TEXT("Tiling_Offset");
    AssetBaseParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_VT_Simple.MI_Env_Base_VT_Simple")));
    AssetBaseMaskedParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_Masked_VT_Simple.MI_Env_Base_Masked_VT_Simple")));
    AssetDestructibleParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Destructible/MI_Env_Destructible_VT_UV.MI_Env_Destructible_VT_UV")));
    AssetMegaParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Mega/MI_Env_Mega.MI_Env_Mega")));
    AssetAlbedoParameter = TEXT("Diffuse_VT");
    AssetNormalParameter = TEXT("Normal_VT");
    AssetORMTextureParameter = TEXT("ORM_VT");
    AssetMaskParameter = TEXT("Mask_VT");
    AssetMegaMaskParameter = TEXT("Mega Mask_UV2");
    AssetEmissiveParameter = TEXT("Emissive_Tex_VT");
    AssetDestructibleEmissiveParameter = TEXT("Emissive_Tex");
    AssetUseEmissiveSwitch = TEXT("UseEmissive");
    Asset3DMaxLOD0Triangles = 300000;
    Asset3DLargeMaxLOD0Triangles = 550000;
    Asset3DLargeSizeThresholdCm = 1000.0f;
    PlantAtlasParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/Common/MaterialInstance/MMI_Grass.MMI_Grass")));
    PlantBillboardParentMaterial = PlantAtlasParentMaterial;
    PlantAtlasMaterialNamePrefix = TEXT("MI_");
    PlantBillboardMaterialNamePrefix = TEXT("MI_Billboard_");
    PlantAlbedoParameter = TEXT("Albedo");
    PlantNRSParameter = TEXT("NRS");
}
FString UAssetHiveSettings::GetDefaultImportRootPath() { return TEXT("/Game/AssetHive"); }
FString UAssetHiveSettings::GetImportRootPath()
{
    return NormalizeImportRootPath(GetDefault<UAssetHiveSettings>()->ImportRootPath);
}
FString UAssetHiveSettings::NormalizeImportRootPath(const FString& Path)
{
    FString Value = Path.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
    if (Value.IsEmpty()) return GetDefaultImportRootPath();
    while (Value.StartsWith(TEXT("/"))) Value.RightChopInline(1);
    while (Value.EndsWith(TEXT("/"))) Value.LeftChopInline(1);
    if (Value.Equals(TEXT("Content"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("Game"), ESearchCase::IgnoreCase)) return TEXT("/Game");
    if (Value.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase)) Value.RightChopInline(8);
    else if (Value.StartsWith(TEXT("Game/"), ESearchCase::IgnoreCase)) Value.RightChopInline(5);
    return TEXT("/Game/") + Value;
}
bool UAssetHiveSettings::IsValidImportRootPath(const FString& Path)
{
    return Path == TEXT("/Game") || (Path.StartsWith(TEXT("/Game/")) &&
        FPackageName::IsValidLongPackageName(Path) && !Path.Contains(TEXT("..")));
}
FString UAssetHiveSettings::ToProjectContentRelativePath(const FString& RootPath)
{
    const FString Normalized = NormalizeImportRootPath(RootPath);
    return Normalized == TEXT("/Game") ? FString() : Normalized.Mid(6);
}

FString UAssetHiveSettings::GetDefaultSurfaceParentMaterialPath()
{
    return TEXT("/Game/MaterialLibrary/Environment/MI_Tiling/MI_Env_Tiling_VT_Simple.MI_Env_Tiling_VT_Simple");
}

FString UAssetHiveSettings::GetSurfaceParentMaterialPath()
{
    const FString Configured = GetDefault<UAssetHiveSettings>()->SurfaceParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    return Configured.IsEmpty() ? GetDefaultSurfaceParentMaterialPath() : Configured;
}

UMaterialInterface* UAssetHiveSettings::GetSurfaceParentMaterial()
{
    const FString Path = GetSurfaceParentMaterialPath();
    return IsValidSurfaceParentMaterialPath(Path) ? LoadObject<UMaterialInterface>(nullptr, *Path) : nullptr;
}

bool UAssetHiveSettings::IsValidSurfaceParentMaterialPath(const FString& Path)
{
    const FString Value = Path.TrimStartAndEnd();
    if (Value.IsEmpty() || !Value.StartsWith(TEXT("/Game/")) || Value.Contains(TEXT(".."))) return false;
    FString PackageName;
    FString ObjectName;
    if (!Value.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || ObjectName.IsEmpty()) return false;
    return FPackageName::IsValidLongPackageName(PackageName);
}

FString UAssetHiveSettings::GetSurfaceMaterialName(const FString& AssetName, int32 GroupId)
{
    FString SafeAssetName = AssetName.TrimStartAndEnd();
    SafeAssetName.ReplaceInline(TEXT(" "), TEXT("_"));
    SafeAssetName.ReplaceInline(TEXT("-"), TEXT("_"));
    SafeAssetName.ReplaceInline(TEXT("."), TEXT("_"));
    if (SafeAssetName.IsEmpty()) SafeAssetName = TEXT("Surface");
    const FString Prefix = GetDefault<UAssetHiveSettings>()->SurfaceMaterialNamePrefix.TrimStartAndEnd().IsEmpty()
        ? TEXT("MI_Env_Tile_") : GetDefault<UAssetHiveSettings>()->SurfaceMaterialNamePrefix.TrimStartAndEnd();
    return GroupId > 1 ? FString::Printf(TEXT("%s%s_%03d"), *Prefix, *SafeAssetName, GroupId) : Prefix + SafeAssetName;
}

FString UAssetHiveSettings::GetAssetParentMaterialPath(const FString& AssetType, bool bMasked)
{
    const UAssetHiveSettings* Settings = GetDefault<UAssetHiveSettings>();
    const TSoftObjectPtr<UMaterialInterface>* Configured = &Settings->AssetBaseParentMaterial;
    if (AssetType.Equals(TEXT("MEGA"), ESearchCase::IgnoreCase))
    {
        Configured = &Settings->AssetMegaParentMaterial;
    }
    else if (AssetType.Equals(TEXT("Destructible"), ESearchCase::IgnoreCase))
    {
        Configured = &Settings->AssetDestructibleParentMaterial;
    }
    else if (bMasked)
    {
        Configured = &Settings->AssetBaseMaskedParentMaterial;
    }
    return Configured->ToSoftObjectPath().ToString().TrimStartAndEnd();
}

UMaterialInterface* UAssetHiveSettings::GetAssetParentMaterial(const FString& AssetType, bool bMasked)
{
    const FString Path = GetAssetParentMaterialPath(AssetType, bMasked);
    return IsValidAssetParentMaterialPath(Path) ? LoadObject<UMaterialInterface>(nullptr, *Path) : nullptr;
}

bool UAssetHiveSettings::IsValidAssetParentMaterialPath(const FString& Path)
{
    return IsValidSurfaceParentMaterialPath(Path);
}

FString UAssetHiveSettings::GetAssetAlbedoParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetAlbedoParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Diffuse_VT") : Value;
}

FString UAssetHiveSettings::GetAssetNormalParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetNormalParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Normal_VT") : Value;
}

FString UAssetHiveSettings::GetAssetORMTextureParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetORMTextureParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("ORM_VT") : Value;
}

FString UAssetHiveSettings::GetAssetMaskParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetMaskParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Mask_VT") : Value;
}

FString UAssetHiveSettings::GetAssetMegaMaskParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetMegaMaskParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Mega Mask_UV2") : Value;
}

FString UAssetHiveSettings::GetAssetEmissiveParameter(const FString& AssetType)
{
    if (AssetType.Equals(TEXT("Destructible"), ESearchCase::IgnoreCase))
    {
        return GetAssetDestructibleEmissiveParameter();
    }
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetEmissiveParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Emissive_Tex_VT") : Value;
}

FString UAssetHiveSettings::GetAssetDestructibleEmissiveParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetDestructibleEmissiveParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Emissive_Tex") : Value;
}

FString UAssetHiveSettings::GetAssetUseEmissiveSwitch()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->AssetUseEmissiveSwitch.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("UseEmissive") : Value;
}

int32 UAssetHiveSettings::GetAsset3DMaxLOD0Triangles()
{
    const int32 Value = GetDefault<UAssetHiveSettings>()->Asset3DMaxLOD0Triangles;
    return Value > 0 ? Value : 300000;
}

int32 UAssetHiveSettings::GetAsset3DLargeMaxLOD0Triangles()
{
    const int32 Value = GetDefault<UAssetHiveSettings>()->Asset3DLargeMaxLOD0Triangles;
    return Value > 0 ? Value : 550000;
}

float UAssetHiveSettings::GetAsset3DLargeSizeThresholdCm()
{
    const float Value = GetDefault<UAssetHiveSettings>()->Asset3DLargeSizeThresholdCm;
    return Value > 0.0f ? Value : 1000.0f;
}

FString UAssetHiveSettings::GetDefaultPlantAtlasParentMaterialPath()
{
    return TEXT("/Game/Common/MaterialInstance/MMI_Grass.MMI_Grass");
}

FString UAssetHiveSettings::GetDefaultPlantBillboardParentMaterialPath()
{
    return GetDefaultPlantAtlasParentMaterialPath();
}

FString UAssetHiveSettings::GetPlantParentMaterialPath(bool bBillboard)
{
    const UAssetHiveSettings* Settings = GetDefault<UAssetHiveSettings>();
    const TSoftObjectPtr<UMaterialInterface>& Configured = bBillboard
        ? Settings->PlantBillboardParentMaterial
        : Settings->PlantAtlasParentMaterial;
    const FString Path = Configured.ToSoftObjectPath().ToString().TrimStartAndEnd();
    if (!Path.IsEmpty()) return Path;
    return bBillboard ? GetDefaultPlantBillboardParentMaterialPath() : GetDefaultPlantAtlasParentMaterialPath();
}

bool UAssetHiveSettings::IsValidPlantParentMaterialPath(const FString& Path)
{
    return IsValidSurfaceParentMaterialPath(Path);
}

UMaterialInterface* UAssetHiveSettings::GetPlantParentMaterial(bool bBillboard, bool bUseVT)
{
    FString Path = GetPlantParentMaterialPath(bBillboard);
    if (bUseVT) {
        FString PackageName;
        FString ObjectName;
        if (Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
            && !ObjectName.EndsWith(TEXT("_VT"), ESearchCase::CaseSensitive)) {
            Path = FString::Printf(TEXT("%s_VT.%s_VT"), *PackageName, *ObjectName);
        }
    }
    return IsValidPlantParentMaterialPath(Path) ? LoadObject<UMaterialInterface>(nullptr, *Path) : nullptr;
}

FString UAssetHiveSettings::GetPlantMaterialName(const FString& AssetName, bool bBillboard)
{
    FString SafeAssetName = AssetName.TrimStartAndEnd();
    SafeAssetName.ReplaceInline(TEXT(" "), TEXT("_"));
    SafeAssetName.ReplaceInline(TEXT("-"), TEXT("_"));
    SafeAssetName.ReplaceInline(TEXT("."), TEXT("_"));
    if (SafeAssetName.IsEmpty()) SafeAssetName = TEXT("Plant");
    const UAssetHiveSettings* Settings = GetDefault<UAssetHiveSettings>();
    const FString ConfiguredPrefix = bBillboard
        ? Settings->PlantBillboardMaterialNamePrefix
        : Settings->PlantAtlasMaterialNamePrefix;
    const FString Prefix = ConfiguredPrefix.TrimStartAndEnd().IsEmpty()
        ? (bBillboard ? TEXT("MI_Billboard_") : TEXT("MI_"))
        : ConfiguredPrefix.TrimStartAndEnd();
    return Prefix + SafeAssetName;
}

FString UAssetHiveSettings::GetPlantAlbedoParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->PlantAlbedoParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Albedo") : Value;
}

FString UAssetHiveSettings::GetPlantNRSParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->PlantNRSParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("NRS") : Value;
}
