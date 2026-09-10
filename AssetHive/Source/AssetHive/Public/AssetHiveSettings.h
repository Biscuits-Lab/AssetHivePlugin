#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AssetHiveSettings.generated.h"

UCLASS(config=Game, defaultconfig, meta=(DisplayName="AssetHive"))
class ASSETHIVE_API UAssetHiveSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UAssetHiveSettings();
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    UPROPERTY(EditAnywhere, config, Category="Import", meta=(DisplayName="Import Root Path", ToolTip="Destination for new imports. Default: Content/AssetHive. Accepts Content/MyAssets or /Game/MyAssets. Existing assets are not moved."))
    FString ImportRootPath;

    static bool IsValidImportRootPath(const FString& Path);
    static FString GetDefaultImportRootPath();
    static FString GetImportRootPath();
    static FString NormalizeImportRootPath(const FString& Path);
    static FString ToProjectContentRelativePath(const FString& RootPath);
};
