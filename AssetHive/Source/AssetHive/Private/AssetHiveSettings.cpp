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
    DecalParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_VT.MI_Env_Decal_VT")));
    DecalPOMParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_VT.MI_Env_POMDecal_VT")));
    DecalTriPlanarParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_Tri_VT.MI_Env_Decal_Tri_VT")));
    DecalTriPlanarPOMParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_Tri_VT.MI_Env_POMDecal_Tri_VT")));
    DecalMaterialNamePrefix = TEXT("MI_Env_Decal_");
    DecalDiffuseParameter = TEXT("Diffuse_VT");
    DecalNormalParameter = TEXT("Normal_VT");
    DecalOpacityParameter = TEXT("OpacityMasked_VT");
    DecalORMTextureParameter = TEXT("ORM_VT");
    DecalDisplacementParameter = TEXT("Displacement");
    DecalUseOpacityTextureSwitch = TEXT("Opacity_UseOpacityTex");
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
    Asset3DMissingSmoothingAngleDegrees = 60.0f;
    // 2026-09-30：项目侧统一使用 GrassBend 植被材质，未裁切（Masked）植被用 Masked 版本。
    PlantAtlasParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_GrassBend_Masked_ST_VT.MI_Env_GrassBend_Masked_ST_VT")));
    PlantBillboardParentMaterial = PlantAtlasParentMaterial;
    PlantAtlasMaterialNamePrefix = TEXT("MI_");
    PlantBillboardMaterialNamePrefix = TEXT("MI_Billboard_");
    PlantAlbedoParameter = TEXT("Albedo");
    PlantNRSParameter = TEXT("NRS");
    // 2026-09-30：_OPAQUE 裁切变体（grass & bush）使用 Opaque 版本。
    PlantOpaqueParentMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_GrassBend_ST_VT.MI_Env_GrassBend_ST_VT")));
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

FString UAssetHiveSettings::NormalizeDecalParentMode(const FString& Mode)
{
    FString Value = Mode.TrimStartAndEnd().ToLower();
    Value.ReplaceInline(TEXT("-"), TEXT(""));
    Value.ReplaceInline(TEXT("_"), TEXT(""));
    Value.ReplaceInline(TEXT(" "), TEXT(""));
    if (Value.IsEmpty() || Value == TEXT("decal") || Value == TEXT("normal") || Value == TEXT("normaldecal") || Value == TEXT("普通贴花"))
    {
        return TEXT("decal");
    }
    if (Value == TEXT("pom") || Value == TEXT("parallax") || Value == TEXT("pomdecal") || Value == TEXT("视差贴花"))
    {
        return TEXT("pomDecal");
    }
    if (Value == TEXT("tri") || Value == TEXT("triplanar") || Value == TEXT("triplanardecal") || Value == TEXT("decaltri") || Value == TEXT("三平面投射贴花"))
    {
        return TEXT("decalTri");
    }
    if (Value == TEXT("pomtri") || Value == TEXT("triplanarpom") || Value == TEXT("pomdecaltri") || Value == TEXT("三平面视差贴花"))
    {
        return TEXT("pomDecalTri");
    }
    return TEXT("decal");
}

FString UAssetHiveSettings::GetDefaultDecalParentMaterialPath(const FString& Mode)
{
    const FString Normalized = NormalizeDecalParentMode(Mode);
    if (Normalized == TEXT("pomDecal"))
    {
        return TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_VT.MI_Env_POMDecal_VT");
    }
    if (Normalized == TEXT("decalTri"))
    {
        return TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_Tri_VT.MI_Env_Decal_Tri_VT");
    }
    if (Normalized == TEXT("pomDecalTri"))
    {
        return TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_POMDecal_Tri_VT.MI_Env_POMDecal_Tri_VT");
    }
    return TEXT("/Game/MaterialLibrary/Environment/MI_Decal/MI_Env_Decal_VT.MI_Env_Decal_VT");
}

FString UAssetHiveSettings::GetDecalParentMaterialPath(const FString& Mode)
{
    const UAssetHiveSettings* Settings = GetDefault<UAssetHiveSettings>();
    const FString Normalized = NormalizeDecalParentMode(Mode);
    FString Configured;
    if (Normalized == TEXT("pomDecal"))
    {
        Configured = Settings->DecalPOMParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    }
    else if (Normalized == TEXT("decalTri"))
    {
        Configured = Settings->DecalTriPlanarParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    }
    else if (Normalized == TEXT("pomDecalTri"))
    {
        Configured = Settings->DecalTriPlanarPOMParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    }
    else
    {
        Configured = Settings->DecalParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    }
    return Configured.IsEmpty() ? GetDefaultDecalParentMaterialPath(Normalized) : Configured;
}

UMaterialInterface* UAssetHiveSettings::GetDecalParentMaterial(const FString& Mode)
{
    const FString Path = GetDecalParentMaterialPath(Mode);
    return IsValidDecalParentMaterialPath(Path) ? LoadObject<UMaterialInterface>(nullptr, *Path) : nullptr;
}

bool UAssetHiveSettings::IsValidDecalParentMaterialPath(const FString& Path)
{
    return IsValidSurfaceParentMaterialPath(Path);
}

FString UAssetHiveSettings::GetDecalMaterialName(const FString& AssetStem)
{
    FString SafeAssetStem = AssetStem.TrimStartAndEnd();
    SafeAssetStem.ReplaceInline(TEXT(" "), TEXT("_"));
    SafeAssetStem.ReplaceInline(TEXT("-"), TEXT("_"));
    SafeAssetStem.ReplaceInline(TEXT("."), TEXT("_"));
    if (SafeAssetStem.IsEmpty()) SafeAssetStem = TEXT("Decal");
    const FString Prefix = GetDefault<UAssetHiveSettings>()->DecalMaterialNamePrefix.TrimStartAndEnd().IsEmpty()
        ? TEXT("MI_Env_Decal_") : GetDefault<UAssetHiveSettings>()->DecalMaterialNamePrefix.TrimStartAndEnd();
    return Prefix + SafeAssetStem;
}

FString UAssetHiveSettings::GetDecalDiffuseParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalDiffuseParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Diffuse_VT") : Value;
}

FString UAssetHiveSettings::GetDecalNormalParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalNormalParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Normal_VT") : Value;
}

FString UAssetHiveSettings::GetDecalOpacityParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalOpacityParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("OpacityMasked_VT") : Value;
}

FString UAssetHiveSettings::GetDecalORMTextureParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalORMTextureParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("ORM_VT") : Value;
}

FString UAssetHiveSettings::GetDecalDisplacementParameter()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalDisplacementParameter.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Displacement") : Value;
}

FString UAssetHiveSettings::GetDecalUseOpacityTextureSwitch()
{
    const FString Value = GetDefault<UAssetHiveSettings>()->DecalUseOpacityTextureSwitch.TrimStartAndEnd();
    return Value.IsEmpty() ? TEXT("Opacity_UseOpacityTex") : Value;
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

float UAssetHiveSettings::GetAsset3DMissingSmoothingAngle()
{
    const float Value = GetDefault<UAssetHiveSettings>()->Asset3DMissingSmoothingAngleDegrees;
    return FMath::Clamp(Value, 0.0f, 180.0f);
}

FString UAssetHiveSettings::GetDefaultPlantAtlasParentMaterialPath()
{
    // 2026-09-30：未裁切（Masked）植被的默认父材质。
    return TEXT("/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_GrassBend_Masked_ST_VT.MI_Env_GrassBend_Masked_ST_VT");
}

FString UAssetHiveSettings::GetDefaultPlantOpaqueParentMaterialPath()
{
    // 2026-09-30：_OPAQUE 裁切变体（grass & bush）的默认父材质。
    return TEXT("/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_GrassBend_ST_VT.MI_Env_GrassBend_ST_VT");
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

FString UAssetHiveSettings::GetPlantOpaqueParentMaterialPath(bool bUseVT)
{
    const UAssetHiveSettings* Settings = GetDefault<UAssetHiveSettings>();
    FString Path = Settings->PlantOpaqueParentMaterial.ToSoftObjectPath().ToString().TrimStartAndEnd();
    if (Path.IsEmpty())
    {
        // 未配置时回退到 GrassBend Opaque 父材质；裁切几何不再依赖遮罩。
        Path = GetDefaultPlantOpaqueParentMaterialPath();
    }
    if (bUseVT)
    {
        FString PackageName;
        FString ObjectName;
        if (Path.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
            && !ObjectName.EndsWith(TEXT("_VT"), ESearchCase::CaseSensitive))
        {
            Path = FString::Printf(TEXT("%s_VT.%s_VT"), *PackageName, *ObjectName);
        }
    }
    return Path;
}

UMaterialInterface* UAssetHiveSettings::GetPlantOpaqueParentMaterial(bool bUseVT)
{
    const FString Path = GetPlantOpaqueParentMaterialPath(bUseVT);
    return IsValidPlantParentMaterialPath(Path) ? LoadObject<UMaterialInterface>(nullptr, *Path) : nullptr;
}

FString UAssetHiveSettings::GetPlantMaterialName(const FString& AssetName, bool bBillboard, bool bOpaque)
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
    const FString DefaultPrefix = bBillboard ? TEXT("MI_Billboard_") : TEXT("MI_");
    const FString Prefix = ConfiguredPrefix.TrimStartAndEnd().IsEmpty()
        ? DefaultPrefix
        : ConfiguredPrefix.TrimStartAndEnd();
    // 2026-09-30 定稿：Opaque 变体使用固定 _OPAQUE 后缀（不再用 MI_Opaque_ 前缀）。
    return bOpaque ? Prefix + SafeAssetName + TEXT("_OPAQUE")
                   : Prefix + SafeAssetName;
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
