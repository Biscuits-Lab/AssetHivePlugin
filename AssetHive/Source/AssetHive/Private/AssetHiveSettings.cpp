#include "AssetHiveSettings.h"
#include "Misc/PackageName.h"

UAssetHiveSettings::UAssetHiveSettings() { ImportRootPath = TEXT("Content/AssetHive"); }
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
