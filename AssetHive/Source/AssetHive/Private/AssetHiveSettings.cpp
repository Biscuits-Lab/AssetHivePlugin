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
