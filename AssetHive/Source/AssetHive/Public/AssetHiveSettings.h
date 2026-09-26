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

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="Atlas Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Parent MI used for 3D Plant Atlas texture groups. Default: /Game/Common/MaterialInstance/MMI_Grass.MMI_Grass"))
    TSoftObjectPtr<UMaterialInterface> PlantAtlasParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="Billboard Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Parent MI used for 3D Plant Billboard texture groups. Defaults to the Atlas parent so the two texture sets still produce separate material instances."))
    TSoftObjectPtr<UMaterialInterface> PlantBillboardParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="Atlas Material Name Prefix", ToolTip="Prefix for 3D Plant Atlas material instances. Default: MI_"))
    FString PlantAtlasMaterialNamePrefix;

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="Billboard Material Name Prefix", ToolTip="Prefix for 3D Plant Billboard material instances. Default: MI_Billboard_"))
    FString PlantBillboardMaterialNamePrefix;

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="Albedo Texture Parameter", ToolTip="Texture parameter receiving the packed Plant albedo/opacity texture."))
    FString PlantAlbedoParameter;

    UPROPERTY(EditAnywhere, config, Category="Plant Material", meta=(DisplayName="NRS Texture Parameter", ToolTip="Texture parameter receiving the packed Plant NRS texture."))
    FString PlantNRSParameter;

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
    static FString GetDefaultPlantAtlasParentMaterialPath();
    static FString GetDefaultPlantBillboardParentMaterialPath();
    static FString GetPlantParentMaterialPath(bool bBillboard);
    static UMaterialInterface* GetPlantParentMaterial(bool bBillboard, bool bUseVT);
    static bool IsValidPlantParentMaterialPath(const FString& Path);
    static FString GetPlantMaterialName(const FString& AssetName, bool bBillboard);
    static FString GetPlantAlbedoParameter();
    static FString GetPlantNRSParameter();
};
