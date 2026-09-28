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

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Base Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Parent MI used for Props, Kits and default Objects 3D assets. Default: /Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_VT_Simple.MI_Env_Base_VT_Simple"))
    TSoftObjectPtr<UMaterialInterface> AssetBaseParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Base Masked Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Parent MI used for Base-family 3D assets whose Mask texture carries opacity. Default: /Game/MaterialLibrary/Environment/MI_Base/MI_Env_Base_Masked_VT_Simple.MI_Env_Base_Masked_VT_Simple"))
    TSoftObjectPtr<UMaterialInterface> AssetBaseMaskedParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Destructible Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Fixed parent MI used for Destructible 3D assets. Default: /Game/MaterialLibrary/Environment/MI_Destructible/MI_Env_Destructible_VT_UV.MI_Env_Destructible_VT_UV"))
    TSoftObjectPtr<UMaterialInterface> AssetDestructibleParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="MEGA Parent Material", AllowedClasses="/Script/Engine.MaterialInterface", ToolTip="Fixed parent MI used for MEGA 3D assets. Default: /Game/MaterialLibrary/Environment/MI_Mega/MI_Env_Mega.MI_Env_Mega"))
    TSoftObjectPtr<UMaterialInterface> AssetMegaParentMaterial;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Diffuse Texture Parameter", ToolTip="Texture parameter receiving the 3D asset Diffuse texture. Default: Diffuse_VT"))
    FString AssetAlbedoParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Normal Texture Parameter", ToolTip="Texture parameter receiving the 3D asset Normal texture. Default: Normal_VT"))
    FString AssetNormalParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="ORM Texture Parameter", ToolTip="Texture parameter receiving AO/Roughness/Metallic packed in RGB. Default: ORM_VT"))
    FString AssetORMTextureParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Base Masked Texture Parameter", ToolTip="Texture parameter receiving OpacityMasked for the Base Masked parent. Default: Mask_VT"))
    FString AssetMaskParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="MEGA UV2 Mask Texture Parameter", ToolTip="Texture parameter receiving the MEGA UV2 Mask texture. Default: Mega Mask_UV2"))
    FString AssetMegaMaskParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Emissive Texture Parameter", ToolTip="Texture parameter receiving Emissive for Base/MEGA parents. Default: Emissive_Tex_VT"))
    FString AssetEmissiveParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Destructible Emissive Texture Parameter", ToolTip="Texture parameter receiving Emissive for the Destructible parent. Default: Emissive_Tex"))
    FString AssetDestructibleEmissiveParameter;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Material", meta=(DisplayName="Use Emissive Switch", ToolTip="Optional static switch enabled when an Emissive texture is present."))
    FString AssetUseEmissiveSwitch;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Mesh", meta=(DisplayName="Megascans / PBRMAX Max LOD0 Triangles", ToolTip="Dressing mesh spec triangle budget. Megascans and PBRMAX 3D assets are decimated on import so the stored LOD0 source geometry stays inside this limit. Default: 300000"))
    int32 Asset3DMaxLOD0Triangles = 300000;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Mesh", meta=(DisplayName="Megascans / PBRMAX Large Asset Max LOD0 Triangles", ToolTip="Triangle budget used when the asset is larger than the large asset size threshold. Default: 550000"))
    int32 Asset3DLargeMaxLOD0Triangles = 550000;

    UPROPERTY(EditAnywhere, config, Category="3D Asset Mesh", meta=(DisplayName="Large Asset Size Threshold (cm)", ToolTip="Assets whose largest bounding box axis exceeds this size use the large asset triangle budget. Default: 1000 (10 m)"))
    float Asset3DLargeSizeThresholdCm = 1000.0f;


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
    static FString GetAssetParentMaterialPath(const FString& AssetType, bool bMasked);
    static UMaterialInterface* GetAssetParentMaterial(const FString& AssetType, bool bMasked);
    static bool IsValidAssetParentMaterialPath(const FString& Path);
    static FString GetAssetAlbedoParameter();
    static FString GetAssetNormalParameter();
    static FString GetAssetORMTextureParameter();
    static FString GetAssetMaskParameter();
    static FString GetAssetMegaMaskParameter();
    static FString GetAssetEmissiveParameter(const FString& AssetType);
    static FString GetAssetDestructibleEmissiveParameter();
    static FString GetAssetUseEmissiveSwitch();
    static int32 GetAsset3DMaxLOD0Triangles();
    static int32 GetAsset3DLargeMaxLOD0Triangles();
    static float GetAsset3DLargeSizeThresholdCm();
    static FString GetDefaultPlantAtlasParentMaterialPath();
    static FString GetDefaultPlantBillboardParentMaterialPath();
    static FString GetPlantParentMaterialPath(bool bBillboard);
    static UMaterialInterface* GetPlantParentMaterial(bool bBillboard, bool bUseVT);
    static bool IsValidPlantParentMaterialPath(const FString& Path);
    static FString GetPlantMaterialName(const FString& AssetName, bool bBillboard);
    static FString GetPlantAlbedoParameter();
    static FString GetPlantNRSParameter();
};
