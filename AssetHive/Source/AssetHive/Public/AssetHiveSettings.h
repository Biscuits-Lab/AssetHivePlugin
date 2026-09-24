#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/SoftObjectPtr.h"
#include "AssetHiveSettings.generated.h"

class UMaterialInterface;

UCLASS(config=Game, defaultconfig, meta=(DisplayName="AssetHive"))
class ASSETHIVE_API UAssetHiveSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UAssetHiveSettings();
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    UPROPERTY(EditAnywhere, config, Category="Import", meta=(DisplayName="Import Root Path", ToolTip="Destination for new imports. Default: Content/AssetHive. Accepts Content/MyAssets or /Game/MyAssets. Existing assets are not moved."))
    FString ImportRootPath;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Parent Material Instance", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Project-side parent MI used for Surface exports. Default: /Game/MaterialLibrary/Environment/MI_Tiling/MI_Env_Tiling_VT_Simple.MI_Env_Tiling_VT_Simple"))
    TSoftObjectPtr<UMaterialInterface> SurfaceParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Material Name Prefix", ToolTip="Prefix for generated Surface material instances. Default: MI_Env_Tile_"))
    FString SurfaceMaterialNamePrefix;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="BCR Texture Parameter", ToolTip="Texture parameter receiving packed BaseColor RGB + Roughness A."))
    FString SurfaceBCRParameter;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Normal Texture Parameter", ToolTip="Texture parameter receiving the Normal texture."))
    FString SurfaceNormalParameter;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Metallic Texture Parameter", ToolTip="Texture parameter receiving the Metallic texture."))
    FString SurfaceMetallicParameter;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Emissive Texture Parameter", ToolTip="Texture parameter receiving the Emissive texture."))
    FString SurfaceEmissiveParameter;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Use Metallic Switch", ToolTip="Static switch enabled when a Metallic texture is present."))
    FString SurfaceUseMetallicSwitch;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Use Emissive Switch", ToolTip="Static switch enabled when an Emissive texture is present."))
    FString SurfaceUseEmissiveSwitch;

    UPROPERTY(EditAnywhere, config, Category="Surface Material", meta=(DisplayName="Tiling Parameter", ToolTip="Vector parameter receiving Base Tiling in XY and zero offset in ZW."))
    FString SurfaceTilingParameter;

    static bool IsValidImportRootPath(const FString& Path);
    static FString GetDefaultImportRootPath();
    static FString GetImportRootPath();
    static FString NormalizeImportRootPath(const FString& Path);
    static FString ToProjectContentRelativePath(const FString& RootPath);
    static FString GetDefaultSurfaceParentMaterialPath();
    static FString GetSurfaceParentMaterialPath();
    static UMaterialInterface* GetSurfaceParentMaterial();
    static bool IsValidSurfaceParentMaterialPath(const FString& Path);
    static FString GetSurfaceMaterialName(const FString& AssetName, int32 GroupId = 1);
};
