#include "AssetHiveImportCommandlet.h"
#include "AssetHiveSettings.h"
#include "AssetHiveThumbnailRefresh.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/CollisionProfile.h"
#include "Editor.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshEditorSubsystem.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Internationalization/Regex.h"
#include "MaterialEditingLibrary.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInstanceBasePropertyOverrides.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PixelFormat.h"
#include "TextureCompiler.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"
#include <type_traits>

template <typename T, typename = void>
struct TAssetHiveHasNaniteSettingsAccessor : std::false_type {};

template <typename T>
struct TAssetHiveHasNaniteSettingsAccessor<
    T, std::void_t<decltype(std::declval<T &>().GetNaniteSettings())>>
    : std::true_type {};

// UE 5.7 adds accessors that are absent from the stock 5.6 header.
template <typename T>
static std::enable_if_t<TAssetHiveHasNaniteSettingsAccessor<T>::value,
                        FMeshNaniteSettings *>
GetMutableNaniteSettings(T *StaticMesh) {
  return StaticMesh ? &StaticMesh->GetNaniteSettings() : nullptr;
}

template <typename T>
static std::enable_if_t<!TAssetHiveHasNaniteSettingsAccessor<T>::value,
                        FMeshNaniteSettings *>
GetMutableNaniteSettings(T *StaticMesh) {
  return StaticMesh ? &StaticMesh->NaniteSettings : nullptr;
}


// Wait only for the referenced texture, including a possible VT rebuild.
// Resource readiness must not depend on saving its package.
static void ForceTextureDataReady(UTexture *Texture) {
  if (!Texture) {
    return;
  }
  UTexture *Textures[] = {Texture};
  FTextureCompilingManager::Get().FinishCompilation(Textures);
  Texture->UpdateResource();
  FTextureCompilingManager::Get().FinishCompilation(Textures);
}

// Imports run on the game thread; keep per-call failure state separate from nested operations.
static thread_local bool GAssetHiveImportFailed = false;
static void FinalizeImportedAsset(UObject* Object) {
  if (!Object) return;
  // Explicit imports remain unsaved even when ordinary dirty marking is suppressed.
  Object->GetOutermost()->SetDirtyFlag(true);
}

UAssetHiveImportCommandlet::UAssetHiveImportCommandlet() {
  IsClient = false;
  IsServer = false;
  IsEditor = true;
  LogToConsole = true;
}

static FString MakeSafeObjectName(const FString &Name) {
  FString SafeName = Name;
  SafeName.ReplaceInline(TEXT(" "), TEXT("_"));
  SafeName.ReplaceInline(TEXT("-"), TEXT("_"));
  SafeName.ReplaceInline(TEXT("."), TEXT("_"));
  return SafeName;
}
static FString NormalizeAssetTagToken(const FString &Value) {
  FString Token = Value;
  Token.TrimStartAndEndInline();
  Token.ReplaceInline(TEXT(" "), TEXT(""));
  Token.ReplaceInline(TEXT("_"), TEXT(""));
  Token.ReplaceInline(TEXT("-"), TEXT(""));
  return Token.ToLower();
}

static bool HasAssetTag(const TArray<FString> &Tags, const TCHAR *Expected) {
  const FString ExpectedToken = NormalizeAssetTagToken(Expected);
  return Tags.ContainsByPredicate([&ExpectedToken](const FString &Tag) {
    return NormalizeAssetTagToken(Tag) == ExpectedToken;
  });
}

static void CollectAssetTags(const TSharedPtr<FJsonObject> &AssetObject,
                             TArray<FString> &OutTags) {
  const TCHAR *FieldNames[] = {TEXT("standardAssetTags"), TEXT("tags")};
  for (const TCHAR *FieldName : FieldNames) {
    const TArray<TSharedPtr<FJsonValue>> *Values = nullptr;
    if (!AssetObject->TryGetArrayField(FieldName, Values) || !Values) {
      continue;
    }
    for (const TSharedPtr<FJsonValue> &Value : *Values) {
      if (Value.IsValid() && Value->Type == EJson::String) {
        const FString Tag = Value->AsString().TrimStartAndEnd();
        if (!Tag.IsEmpty()) {
          OutTags.AddUnique(Tag);
        }
      }
    }
  }
}

// 3D Plants 命名（用户口头 P0，2026-09-30 定稿）：
// SM_Env_<标准AssetTag>_<资产ID>_<变体编号>[_OPAQUE] /
// T_Env_<标准AssetTag>_<资产ID>_<纹理类型>（不带分辨率）/
// MI_Env_<标准AssetTag>_<资产ID>_<变体编号>[_OPAQUE]。
static FString ResolvePlantTagSegment(const TArray<FString> &Tags) {
  const TCHAR *PreferredTags[] = {TEXT("Tree"), TEXT("Bush"), TEXT("Grass"),
                                  TEXT("MicroFoliage"), TEXT("HeroFoliage")};
  for (const TCHAR *Preferred : PreferredTags) {
    if (HasAssetTag(Tags, Preferred)) {
      return FString(Preferred);
    }
  }
  for (const FString &Tag : Tags) {
    const FString SafeTag = MakeSafeObjectName(Tag.TrimStartAndEnd());
    if (!SafeTag.IsEmpty()) {
      return SafeTag;
    }
  }
  return TEXT("Plant");
}

static FString BuildPlantObjectStem(const TArray<FString> &Tags,
                                    const FString &AssetId) {
  const FString Id = MakeSafeObjectName(AssetId).TrimStartAndEnd();
  const FString Tag = ResolvePlantTagSegment(Tags);
  return Id.IsEmpty() ? FString::Printf(TEXT("Env_%s"), *Tag)
                      : FString::Printf(TEXT("Env_%s_%s"), *Tag, *Id);
}

// 3D Plants 导出档案（md §5.2，2026-09-30 插件侧定稿）：路径 / 面数 / Nanite /
// 碰撞 / 纹理预设按标准 Asset Tag 分子类；命名模板仍只对 FBX 原始资产生效。
struct FPlantAssetProfile {
  FString SubtypeFolder;  // Vegetation 下的子类目录，未识别时留空
  int32 MaxLOD0Triangles = 0;
  bool bNanite = true;
  bool bHandleCollision = false;
  bool bTrunkCollision = false;
  int32 TextureMaxSize = 2048;
  int32 TextureVTSize = 2048;
  bool bAllowVirtualTexture = true;
};

static FPlantAssetProfile ResolvePlantAssetProfile(const TArray<FString> &Tags) {
  FPlantAssetProfile Profile;
  if (HasAssetTag(Tags, TEXT("Tree"))) {
    Profile.SubtypeFolder = TEXT("Tree");
    Profile.MaxLOD0Triangles = 100000;
    Profile.bHandleCollision = true;
    Profile.bTrunkCollision = true;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Bush"))) {
    Profile.SubtypeFolder = TEXT("Bush");
    Profile.MaxLOD0Triangles = 35000;
    Profile.bHandleCollision = true;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Grass"))) {
    Profile.SubtypeFolder = TEXT("Grass");
    Profile.MaxLOD0Triangles = 10000;
    Profile.bHandleCollision = true;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("MicroFoliage"))) {
    Profile.SubtypeFolder = TEXT("Micro");
    Profile.MaxLOD0Triangles = 500;
    Profile.bHandleCollision = true;
    Profile.bNanite = false;
    Profile.TextureMaxSize = 1024;
    Profile.bAllowVirtualTexture = false;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("HeroFoliage"))) {
    Profile.SubtypeFolder = TEXT("HeroFoliage");
    Profile.MaxLOD0Triangles = 200000;
    Profile.bHandleCollision = true;
    Profile.bTrunkCollision = true;
    Profile.TextureMaxSize = 4096;
    return Profile;
  }
  // 未识别的植被不做子类分档，也不改写 Nanite / 碰撞设置。
  Profile.MaxLOD0Triangles = 0;
  return Profile;
}

// 命名模板只对原始资产为 FBX 的 3D Plants 生效（st9 保持原有命名）。
// 口径与软件侧 buildExportModelPlan 一致：模型文件存在且全部为 .fbx。
static bool AssetHasFbxPlantModels(const TSharedPtr<FJsonObject> &AssetObject) {
  if (!AssetObject.IsValid()) {
    return false;
  }
  TArray<FString> ModelFiles;
  const TArray<TSharedPtr<FJsonValue>> *VariantValues = nullptr;
  if (AssetObject->TryGetArrayField(TEXT("modelVariants"), VariantValues) &&
      VariantValues) {
    for (const TSharedPtr<FJsonValue> &Value : *VariantValues) {
      if (!Value.IsValid() || Value->Type != EJson::Object) {
        continue;
      }
      const TSharedPtr<FJsonObject> VariantObject = Value->AsObject();
      if (!VariantObject.IsValid()) {
        continue;
      }
      FString SourceFile;
      if ((VariantObject->TryGetStringField(TEXT("file"), SourceFile) ||
           VariantObject->TryGetStringField(TEXT("path"), SourceFile) ||
           VariantObject->TryGetStringField(TEXT("uri"), SourceFile)) &&
          !SourceFile.IsEmpty()) {
        ModelFiles.AddUnique(SourceFile);
      }
    }
  }
  if (ModelFiles.Num() == 0) {
    const TArray<TSharedPtr<FJsonValue>> *FileValues = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("modelFiles"), FileValues) &&
        FileValues) {
      for (const TSharedPtr<FJsonValue> &Value : *FileValues) {
        if (Value.IsValid() && Value->Type == EJson::String) {
          const FString SourceFile = Value->AsString();
          if (!SourceFile.IsEmpty()) {
            ModelFiles.AddUnique(SourceFile);
          }
        }
      }
    }
  }
  if (ModelFiles.Num() == 0) {
    return false;
  }
  for (const FString &SourceFile : ModelFiles) {
    if (FPaths::GetExtension(SourceFile).ToLower() != TEXT("fbx")) {
      return false;
    }
  }
  return true;
}

struct FEnvironmentAssetProfile {
  FString TypeKey = TEXT("Objects");
  FString TypePrefix;
  bool bUseAssetIdOnly = false;
  FString FolderName = TEXT("Objects");
  bool bBaseFamily = true;
  bool bHighResPreset = true;
  bool bMega = false;
};

static FEnvironmentAssetProfile ResolveEnvironmentAssetProfile(
    const TArray<FString> &Tags) {
  FEnvironmentAssetProfile Profile;
  if (HasAssetTag(Tags, TEXT("MEGA"))) {
    Profile.TypeKey = TEXT("MEGA");
    Profile.TypePrefix = TEXT("MEGA");
    Profile.FolderName = TEXT("MEGA");
    Profile.bBaseFamily = false;
    Profile.bHighResPreset = true;
    Profile.bMega = true;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Kits")) || HasAssetTag(Tags, TEXT("Kit"))) {
    Profile.TypeKey = TEXT("Kits");
    Profile.TypePrefix = TEXT("Kit");
    Profile.FolderName = TEXT("Kits");
    Profile.bBaseFamily = true;
    Profile.bHighResPreset = false;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Destructible"))) {
    Profile.TypeKey = TEXT("Destructible");
    Profile.TypePrefix = TEXT("Dest");
    Profile.FolderName = TEXT("Destructible");
    Profile.bBaseFamily = false;
    Profile.bHighResPreset = true;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Props"))) {
    Profile.TypeKey = TEXT("Props");
    Profile.TypePrefix = TEXT("Prop");
    Profile.FolderName = TEXT("Props");
    Profile.bBaseFamily = true;
    Profile.bHighResPreset = false;
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Dressing"))) {
    Profile.TypeKey = TEXT("Dressing");
  } else if (HasAssetTag(Tags, TEXT("Megascans"))) {
    Profile.TypeKey = TEXT("Megascans");
    Profile.bUseAssetIdOnly = true;
  } else if (HasAssetTag(Tags, TEXT("PBRMAX"))) {
    Profile.TypeKey = TEXT("PBRMAX");
  }
  Profile.FolderName = TEXT("Objects");
  Profile.bBaseFamily = true;
  Profile.bHighResPreset = true;
  return Profile;
}

static FString BuildEnvironmentObjectStem(const FEnvironmentAssetProfile &Profile,
                                          const FString &AssetName,
                                          const FString &AssetId) {
  const FString Name = MakeSafeObjectName(AssetName).TrimStartAndEnd();
  const FString Id = MakeSafeObjectName(AssetId).TrimStartAndEnd();
  FString Stem;
  if (Profile.bUseAssetIdOnly && !Id.IsEmpty()) {
    Stem = Id;
  } else if (!Id.IsEmpty()) {
    Stem = Name.IsEmpty() ? Id
                          : FString::Printf(TEXT("%s_%s"), *Name, *Id);
  } else {
    Stem = Name.IsEmpty() ? FString(TEXT("AssetHiveAsset")) : Name;
  }
  return Profile.TypePrefix.IsEmpty()
             ? FString::Printf(TEXT("Env_%s"), *Stem)
             : FString::Printf(TEXT("Env_%s_%s"), *Profile.TypePrefix, *Stem);
}

static FString BuildEnvironmentAssetMaterialName(
    const FString &EnvironmentStem, int32 GroupId,
    bool bMultipleTextureGroups, const FString &VariantKey) {
  const FString GroupSegment =
      bMultipleTextureGroups
          ? FString::Printf(TEXT("_%03d"), GroupId)
          : FString();
  return FString::Printf(TEXT("MI_%s%s_%s"), *EnvironmentStem,
                         *GroupSegment, *VariantKey);
}


static FString ToSlotSuffix(const FString &SlotName);

static FString To3DTextureSlotSuffix(const FString &SlotName) {
  if (SlotName == TEXT("albedo")) return TEXT("AL");
  if (SlotName == TEXT("normal")) return TEXT("N");
  if (SlotName == TEXT("orm")) return TEXT("ORM");
  if (SlotName == TEXT("mask")) return TEXT("Mask");
  if (SlotName == TEXT("opacity")) return TEXT("O");
  if (SlotName == TEXT("emissive")) return TEXT("E");
  if (SlotName == TEXT("metalness")) return TEXT("M");
  if (SlotName == TEXT("roughness")) return TEXT("R");
  if (SlotName == TEXT("displacement")) return TEXT("Dis");
  if (SlotName == TEXT("subsurfacecolor")) return TEXT("SSC");
  if (SlotName == TEXT("translucency")) return TEXT("T");
  if (SlotName == TEXT("fuzz")) return TEXT("Fuzz");
  return ToSlotSuffix(SlotName).ToUpper();
}

static void Apply3DAssetTexturePreset(UTexture *Texture,
                                      const FString &SlotName,
                                      const FEnvironmentAssetProfile &Profile,
                                      bool bUseVT) {
  if (!Texture) {
    return;
  }
  const UTexture2D *Texture2D = Cast<UTexture2D>(Texture);
  const int32 ActualMax = Texture2D
                              ? FMath::Max(Texture2D->GetSizeX(), Texture2D->GetSizeY())
                              : 0;
  const int32 PrimaryLimit = Profile.bHighResPreset ? 4096 : 2048;
  const int32 SecondaryLimit = Profile.bHighResPreset ? 2048 : 1024;
  const bool bPrimary = SlotName == TEXT("albedo") || SlotName == TEXT("normal");
  int32 DesiredLimit = bPrimary ? PrimaryLimit : SecondaryLimit;
  if (ActualMax > 0) {
    DesiredLimit = FMath::Min(DesiredLimit, ActualMax);
  }
  Texture->MaxTextureSize = FMath::Clamp(DesiredLimit, 256, 8192);
  Texture->MipGenSettings = TMGS_FromTextureGroup;
  Texture->VirtualTextureStreaming = bUseVT && Texture->MaxTextureSize >= 2048;

  if (SlotName == TEXT("albedo")) {
    Texture->CompressionSettings = TC_Default;
    Texture->SRGB = true;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Low;
  } else if (SlotName == TEXT("normal")) {
    Texture->CompressionSettings = TC_Normalmap;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
    Texture->LossyCompressionAmount = TLCA_Low;
  } else if (SlotName == TEXT("emissive") ||
             SlotName == TEXT("subsurfacecolor") ||
             SlotName == TEXT("translucency")) {
    Texture->CompressionSettings = TC_Default;
    Texture->SRGB = true;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
  } else if (SlotName == TEXT("orm")) {
    Texture->CompressionSettings = TC_Masks;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
  } else if (SlotName == TEXT("mask") || SlotName == TEXT("opacity")) {
    Texture->CompressionSettings = TC_Masks;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = false;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
  } else if (SlotName == TEXT("fuzz") || SlotName == TEXT("metalness") ||
             SlotName == TEXT("roughness") || SlotName == TEXT("ao") ||
             SlotName == TEXT("displacement")) {
    Texture->CompressionSettings = TC_Masks;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
  }
}

// Megascans and PBRMAX 3D assets arrive as dense scan meshes. Instead of
// decimating the source mesh the Nanite build keeps only a fraction of the
// source triangles so the asset stays inside the scene Dressing mesh spec:
// 300k triangles, or 550k once the asset is bigger than 10 m on any axis.
static float GetSourceMeshMaxDimensionCm(const FMeshDescription &Mesh) {
  const FBox Bounds = Mesh.ComputeBoundingBox();
  const FVector Size = Bounds.GetSize();
  return static_cast<float>(FMath::Max3(Size.X, Size.Y, Size.Z));
}

static int32 ResolveScanSourceTriangleBudget(float MaxDimensionCm) {
  return MaxDimensionCm > UAssetHiveSettings::GetAsset3DLargeSizeThresholdCm()
             ? UAssetHiveSettings::GetAsset3DLargeMaxLOD0Triangles()
             : UAssetHiveSettings::GetAsset3DMaxLOD0Triangles();
}

// Returns true when the Nanite keep percentage was written for this mesh.
static bool ApplyNaniteTriangleBudget(UStaticMesh *StaticMesh,
                                      FString &OutSummary) {
  OutSummary.Reset();
  if (!StaticMesh) {
    return false;
  }
  const FMeshDescription *SourceMesh = StaticMesh->GetMeshDescription(0);
  if (!SourceMesh) {
    return false;
  }
  const int32 SourceTriangles = SourceMesh->Triangles().Num();
  if (SourceTriangles <= 0) {
    return false;
  }
  const float MaxDimensionCm = GetSourceMeshMaxDimensionCm(*SourceMesh);
  const int32 TriangleBudget = ResolveScanSourceTriangleBudget(MaxDimensionCm);
  if (TriangleBudget <= 0) {
    return false;
  }
  // Keeping 100% of the source triangles leaves Nanite untouched.
  const float KeepPercent =
      SourceTriangles <= TriangleBudget
          ? 1.0f
          : FMath::Clamp(static_cast<float>(TriangleBudget) /
                             static_cast<float>(SourceTriangles),
                         0.0f, 1.0f);
  if (FMath::IsNearlyEqual(GetMutableNaniteSettings(StaticMesh)->KeepPercentTriangles,
                           KeepPercent, 0.0001f)) {
    return false;
  }
  GetMutableNaniteSettings(StaticMesh)->KeepPercentTriangles = KeepPercent;
  OutSummary = FString::Printf(
      TEXT("%s %.2f m source, %d triangles, budget %d, Nanite keep %.4f"),
      *StaticMesh->GetName(), MaxDimensionCm / 100.0f, SourceTriangles,
      TriangleBudget, KeepPercent);
  UE_LOG(LogTemp, Display, TEXT("AssetHive import: %s"), *OutSummary);
  return true;
}

// 3D Plants 面数规范（md §5.2）：Tree 100k / Bush 35k / Grass 10k /
// HeroFoliage 200k / Micro 0.5k。超出时用 Nanite 保留百分比压到规范内。
static bool ApplyPlantNaniteTriangleBudget(UStaticMesh *StaticMesh,
                                           int32 TriangleBudget,
                                           FString &OutSummary) {
  OutSummary.Reset();
  if (!StaticMesh || TriangleBudget <= 0) {
    return false;
  }
  const FMeshDescription *SourceMesh = StaticMesh->GetMeshDescription(0);
  if (!SourceMesh) {
    return false;
  }
  const int32 SourceTriangles = SourceMesh->Triangles().Num();
  if (SourceTriangles <= 0) {
    return false;
  }
  const float MaxDimensionCm = GetSourceMeshMaxDimensionCm(*SourceMesh);
  const float KeepPercent =
      SourceTriangles <= TriangleBudget
          ? 1.0f
          : FMath::Clamp(static_cast<float>(TriangleBudget) /
                             static_cast<float>(SourceTriangles),
                         0.0f, 1.0f);
  if (FMath::IsNearlyEqual(GetMutableNaniteSettings(StaticMesh)->KeepPercentTriangles,
                           KeepPercent, 0.0001f)) {
    return false;
  }
  GetMutableNaniteSettings(StaticMesh)->KeepPercentTriangles = KeepPercent;
  OutSummary = FString::Printf(
      TEXT("%s %.2f m plant source, %d triangles, plant budget %d, Nanite keep %.4f"),
      *StaticMesh->GetName(), MaxDimensionCm / 100.0f, SourceTriangles,
      TriangleBudget, KeepPercent);
  UE_LOG(LogTemp, Display, TEXT("AssetHive import: %s"), *OutSummary);
  return true;
}

static bool FbxHasSmoothingGroupLayer(const FString &SourceFile) {
  TArray<uint8> FileData;
  if (!FFileHelper::LoadFileToArray(FileData, *SourceFile)) {
    UE_LOG(LogTemp, Warning,
           TEXT("AssetHive import: failed to read FBX smoothing data: %s"),
           *SourceFile);
    // Keep the imported data untouched when the source cannot be inspected.
    return true;
  }

  static constexpr ANSICHAR SmoothingToken[] = "LayerElementSmoothing";
  constexpr int32 SmoothingTokenLength = UE_ARRAY_COUNT(SmoothingToken) - 1;
  if (FileData.Num() < SmoothingTokenLength) {
    return false;
  }

  const uint8 *Data = FileData.GetData();
  const uint8 FirstTokenByte = static_cast<uint8>(SmoothingToken[0]);
  for (int32 Offset = 0; Offset <= FileData.Num() - SmoothingTokenLength;
       ++Offset) {
    if (Data[Offset] == FirstTokenByte &&
        FMemory::Memcmp(Data + Offset, SmoothingToken,
                        SmoothingTokenLength) == 0) {
      return true;
    }
  }
  return false;
}

// UE keeps Recompute Normals disabled for this import path. When an FBX has
// no smoothing-group layer, derive explicit hard edges from the polygon angle
// while leaving the imported vertex normals untouched.
static bool ApplyGeneratedSmoothingGroups(UStaticMesh *StaticMesh,
                                          float AngleDegrees,
                                          FString &OutSummary) {
  OutSummary.Reset();
  if (!StaticMesh) {
    return false;
  }

  const float ClampedAngle = FMath::Clamp(AngleDegrees, 0.0f, 180.0f);
  const float CosThreshold =
      FMath::Cos(FMath::DegreesToRadians(ClampedAngle));
  int32 GeneratedLodCount = 0;
  int32 GeneratedHardEdgeCount = 0;

  for (int32 LodIndex = 0; LodIndex < StaticMesh->GetNumSourceModels();
       ++LodIndex) {
    FMeshDescription *Mesh = StaticMesh->GetMeshDescription(LodIndex);
    if (!Mesh || Mesh->Polygons().Num() == 0 || Mesh->Triangles().Num() == 0) {
      continue;
    }

    FStaticMeshAttributes Attributes(*Mesh);
    Attributes.Register(true);
    FStaticMeshOperations::ComputeTriangleTangentsAndNormals(*Mesh);
    Mesh->BuildEdgeIndexers();
    Mesh->BuildPolygonIndexers();

    TTriangleAttributesRef<FVector3f> TriangleNormals =
        Attributes.GetTriangleNormals();
    TMap<FPolygonID, FVector3f> PolygonNormals;
    PolygonNormals.Reserve(Mesh->Polygons().Num());
    for (const FPolygonID PolygonID : Mesh->Polygons().GetElementIDs()) {
      FVector3f NormalSum = FVector3f::ZeroVector;
      int32 TriangleCount = 0;
      for (const FTriangleID TriangleID :
           Mesh->GetPolygonTriangles(PolygonID)) {
        NormalSum += TriangleNormals[TriangleID];
        ++TriangleCount;
      }
      PolygonNormals.Add(PolygonID, TriangleCount > 0
                                        ? NormalSum.GetSafeNormal()
                                        : FVector3f::ZeroVector);
    }

    TEdgeAttributesRef<bool> EdgeHardnesses = Attributes.GetEdgeHardnesses();
    int32 HardEdgeCount = 0;
    for (const FEdgeID EdgeID : Mesh->Edges().GetElementIDs()) {
      const TArray<FPolygonID, TInlineAllocator<2>> ConnectedPolygons =
          Mesh->GetEdgeConnectedPolygons<TInlineAllocator<2>>(EdgeID);
      bool bHardEdge = ConnectedPolygons.Num() != 2;
      if (!bHardEdge) {
        const FVector3f *FirstNormal =
            PolygonNormals.Find(ConnectedPolygons[0]);
        const FVector3f *SecondNormal =
            PolygonNormals.Find(ConnectedPolygons[1]);
        bHardEdge =
            !FirstNormal || !SecondNormal ||
            FVector3f::DotProduct(*FirstNormal, *SecondNormal) < CosThreshold;
      }
      EdgeHardnesses[EdgeID] = bHardEdge;
      HardEdgeCount += bHardEdge ? 1 : 0;
    }

    StaticMesh->GetSourceModel(LodIndex)
        .BuildSettings.bRecomputeNormals = false;
    StaticMesh->CommitMeshDescription(LodIndex);
    ++GeneratedLodCount;
    GeneratedHardEdgeCount += HardEdgeCount;
  }

  if (GeneratedLodCount == 0) {
    return false;
  }

  OutSummary = FString::Printf(
      TEXT("%s: generated smoothing groups for %d LOD(s), %d hard edge(s), angle %.1f deg; Recompute Normals remains disabled"),
      *StaticMesh->GetName(), GeneratedLodCount, GeneratedHardEdgeCount,
      ClampedAngle);
  UE_LOG(LogTemp, Display, TEXT("AssetHive import: %s"), *OutSummary);
  return true;
}

static void Configure3DAssetCollision(UStaticMesh *StaticMesh) {
  if (!StaticMesh) {
    return;
  }
  UBodySetup *BodySetup = StaticMesh->GetBodySetup();
  const bool bHasCollision =
      BodySetup && BodySetup->AggGeom.GetElementCount() > 0;
  if (!bHasCollision && GEditor) {
    if (UStaticMeshEditorSubsystem *Subsystem =
            GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>()) {
      // Use UE's maximum Auto Convex Collision precision for the provisional hull; 16 voxels was too coarse and rounded away silhouettes.
      constexpr int32 TemporaryHullPrecision = 1000000;
      Subsystem->SetConvexDecompositionCollisions(StaticMesh, 1, 16, TemporaryHullPrecision);
      BodySetup = StaticMesh->GetBodySetup();
    }
  }
  if (BodySetup) {
    BodySetup->DefaultInstance.SetCollisionProfileName(
        UCollisionProfile::BlockAll_ProfileName);
    BodySetup->MarkPackageDirty();
  }
  FinalizeImportedAsset(StaticMesh);
}

// 3D Plants 碰撞（md §5.2）：Tree / HeroFoliage 只做树干碰撞——已有碰撞原样保留，
// 缺失时按树干材质槽位（识别不到槽位时退化为高度下段 30%）生成临时凸包；
// Bush / Grass / Micro 不参与碰撞。
static bool IsPlantTrunkSlotToken(const FString &SlotName) {
  const FString Lower = SlotName.ToLower();
  static const TCHAR *const Tokens[] = {TEXT("trunk"), TEXT("bark"),
                                        TEXT("stem"),  TEXT("log"),
                                        TEXT("wood"),  TEXT("branch")};
  for (const TCHAR *Token : Tokens) {
    if (Lower.Contains(Token)) {
      return true;
    }
  }
  return false;
}

static void CollectPlantCollisionPoints(const UStaticMesh &StaticMesh,
                                        const FMeshDescription &MeshDescription,
                                        TSet<FVector> &OutPoints,
                                        bool &bOutUsedTrunkSlots) {
  const FStaticMeshConstAttributes Attributes(MeshDescription);
  const TVertexAttributesConstRef<FVector3f> VertexPositions =
      Attributes.GetVertexPositions();
  const TPolygonGroupAttributesConstRef<FName> PolygonGroupSlotNames =
      Attributes.GetPolygonGroupMaterialSlotNames();
  const TArray<FStaticMaterial> &Slots = StaticMesh.GetStaticMaterials();

  bool bHasTrunkSlot = false;
  for (const FStaticMaterial &Slot : Slots) {
    if (IsPlantTrunkSlotToken(Slot.MaterialSlotName.ToString()) ||
        IsPlantTrunkSlotToken(Slot.ImportedMaterialSlotName.ToString())) {
      bHasTrunkSlot = true;
      break;
    }
  }

  TSet<FIntVector> SeenPoints;
  TArray<FVector> AllPoints;
  TArray<FVertexInstanceID, TInlineAllocator<8>> PolygonVertexInstances;
  bool bGatheredTrunkPoints = false;
  auto AddPoint = [&OutPoints, &SeenPoints](const FVector &Position) {
    const FIntVector Key(FMath::RoundToInt(Position.X * 10.0),
                         FMath::RoundToInt(Position.Y * 10.0),
                         FMath::RoundToInt(Position.Z * 10.0));
    bool bAlreadySeen = false;
    SeenPoints.Add(Key, &bAlreadySeen);
    if (!bAlreadySeen) {
      OutPoints.Add(Position);
    }
  };
  for (const FPolygonID PolygonID : MeshDescription.Polygons().GetElementIDs()) {
    const FPolygonGroupID PolygonGroupID =
        MeshDescription.GetPolygonPolygonGroup(PolygonID);
    FString SlotName = PolygonGroupSlotNames[PolygonGroupID].ToString();
    if (SlotName.IsEmpty() && Slots.IsValidIndex(PolygonGroupID.GetValue())) {
      const FStaticMaterial &Slot = Slots[PolygonGroupID.GetValue()];
      SlotName = Slot.MaterialSlotName.IsNone()
                     ? Slot.ImportedMaterialSlotName.ToString()
                     : Slot.MaterialSlotName.ToString();
    }
    const bool bTrunkPolygon = bHasTrunkSlot && IsPlantTrunkSlotToken(SlotName);
    PolygonVertexInstances.Reset();
    MeshDescription.GetPolygonVertexInstances(PolygonID,
                                              PolygonVertexInstances);
    for (const FVertexInstanceID VertexInstanceID : PolygonVertexInstances) {
      const FVertexID VertexID =
          MeshDescription.GetVertexInstanceVertex(VertexInstanceID);
      const FVector Position(VertexPositions[VertexID]);
      if (bTrunkPolygon && !bGatheredTrunkPoints) {
        bGatheredTrunkPoints = true;
      }
      if (bTrunkPolygon) {
        AddPoint(Position);
      } else if (!bHasTrunkSlot) {
        // 槽位识别失败时才需要收集全网格顶点用于高度下段退化。
        AllPoints.Add(Position);
      }
    }
  }
  if (bGatheredTrunkPoints) {
    bOutUsedTrunkSlots = true;
    return;
  }
  // 没有可识别的树干槽位：退化为“高度下段 30%”，保证仍有临时碰撞。
  OutPoints.Reset();
  SeenPoints.Reset();
  if (AllPoints.Num() == 0) {
    return;
  }
  double MinZ = AllPoints[0].Z;
  double MaxZ = AllPoints[0].Z;
  for (const FVector &Point : AllPoints) {
    MinZ = FMath::Min(MinZ, Point.Z);
    MaxZ = FMath::Max(MaxZ, Point.Z);
  }
  const double SliceHeight = MinZ + (MaxZ - MinZ) * 0.3;
  for (const FVector &Point : AllPoints) {
    if (Point.Z <= SliceHeight) {
      AddPoint(Point);
    }
  }
}

static bool BuildPlantTrunkConvexElement(const UStaticMesh &StaticMesh,
                                         const FMeshDescription &MeshDescription,
                                         FKConvexElem &OutElem,
                                         FString &OutSummary) {
  TSet<FVector> Points;
  bool bUsedTrunkSlots = false;
  CollectPlantCollisionPoints(StaticMesh, MeshDescription, Points,
                              bUsedTrunkSlots);
  if (Points.Num() < 4) {
    return false;
  }
  OutElem.VertexData = Points.Array();
  OutElem.UpdateElemBox();
  OutSummary = FString::Printf(
      TEXT("%s trunk collision from %d vertices (%s)"),
      *StaticMesh.GetName(), OutElem.VertexData.Num(),
      bUsedTrunkSlots ? TEXT("trunk material slots") : TEXT("lower 30% slice"));
  UE_LOG(LogTemp, Display, TEXT("AssetHive import: %s"), *OutSummary);
  return true;
}

static void ConfigurePlantAssetCollision(UStaticMesh *StaticMesh,
                                         const FPlantAssetProfile &Profile) {
  if (!StaticMesh || !Profile.bHandleCollision) {
    return;
  }
  UBodySetup *BodySetup = StaticMesh->GetBodySetup();
  const bool bHasCollision =
      BodySetup && BodySetup->AggGeom.GetElementCount() > 0;
  if (!Profile.bTrunkCollision) {
    // Bush / Grass / Micro：植被不参与碰撞。
    if (bHasCollision) {
      if (UStaticMeshEditorSubsystem *Subsystem =
              GEditor ? GEditor->GetEditorSubsystem<UStaticMeshEditorSubsystem>()
                      : nullptr) {
        Subsystem->RemoveCollisions(StaticMesh);
      } else if (BodySetup) {
        BodySetup->RemoveSimpleCollision();
      }
      BodySetup = StaticMesh->GetBodySetup();
    }
    if (BodySetup) {
      BodySetup->DefaultInstance.SetCollisionProfileName(
          UCollisionProfile::NoCollision_ProfileName);
      BodySetup->MarkPackageDirty();
    }
    FinalizeImportedAsset(StaticMesh);
    return;
  }
  // Tree / HeroFoliage：已有碰撞原样保留，缺失时补一个临时树干凸包。
  if (!bHasCollision) {
    const FMeshDescription *SourceMesh = StaticMesh->GetMeshDescription(0);
    if (BodySetup && SourceMesh) {
      FKConvexElem TrunkElem;
      FString TrunkSummary;
      if (BuildPlantTrunkConvexElement(*StaticMesh, *SourceMesh, TrunkElem,
                                       TrunkSummary)) {
        BodySetup->AggGeom.ConvexElems.Add(MoveTemp(TrunkElem));
        BodySetup->InvalidatePhysicsData();
        BodySetup->CreatePhysicsMeshes();
        BodySetup = StaticMesh->GetBodySetup();
      } else {
        UE_LOG(LogTemp, Warning,
               TEXT("AssetHive import: %s 未找到可用的树干几何，跳过临时碰撞生成"),
               *StaticMesh->GetName());
      }
    }
  }
  if (BodySetup) {
    BodySetup->DefaultInstance.SetCollisionProfileName(
        UCollisionProfile::BlockAll_ProfileName);
    BodySetup->MarkPackageDirty();
  }
  FinalizeImportedAsset(StaticMesh);
}

static FString NormalizePathLower(const FString &Value) {
  FString Result = Value.Replace(TEXT("\\"), TEXT("/"));
  return Result.ToLower();
}

static FString DetectTextureSlot(const FString &SourceFile) {
  const FString Name = FPaths::GetBaseFilename(SourceFile).ToLower();
  // SubsurfaceColor (SSC) slot: the library exports the Translucency (T)
  // texture here, so it must win over the generic "color" rule below.
  if (Name.Contains(TEXT("subsurface")) ||
      Name.Contains(TEXT("translucency")) ||
      Name.Contains(TEXT("translucent")) ||
      Name.Contains(TEXT("transmission")) || Name.Contains(TEXT("sss")))
    return TEXT("subsurfacecolor");
  if (Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("basecolor")) ||
      Name.Contains(TEXT("base_color")) || Name.Contains(TEXT("diffuse")) ||
      Name.Contains(TEXT("color")))
    return TEXT("albedo");
  if (Name.Contains(TEXT("hdr")) || Name.Contains(TEXT("hdri")) ||
      Name.EndsWith(TEXT(".hdr")) || Name.EndsWith(TEXT(".exr")))
    return TEXT("hdr");
  if (Name == TEXT("orm") || Name == TEXT("ormh") ||
      Name.EndsWith(TEXT("_orm")) || Name.EndsWith(TEXT("-orm")) ||
      Name.EndsWith(TEXT("_ormh")) || Name.EndsWith(TEXT("-ormh")) ||
      Name.Contains(TEXT("_orm_")) || Name.Contains(TEXT("-orm-")) ||
      Name.Contains(TEXT("_ormh_")) || Name.Contains(TEXT("-ormh-")))
    return TEXT("orm");
  if (Name.Contains(TEXT("ao")) || Name.Contains(TEXT("ambientocclusion")) ||
      Name.Contains(TEXT("ambient_occlusion")))
    return TEXT("ao");
  if (Name.Contains(TEXT("normal")) || Name.Contains(TEXT("nrm")) ||
      Name.Contains(TEXT("nor")))
    return TEXT("normal");
  if (Name.Contains(TEXT("roughness")) || Name.Contains(TEXT("rough")))
    return TEXT("roughness");
  if (Name.Contains(TEXT("metalness")) || Name.Contains(TEXT("metallic")) ||
      Name.Contains(TEXT("metal")))
    return TEXT("metalness");
  if (Name.Contains(TEXT("emissive")) || Name.Contains(TEXT("emission")) ||
      Name.Contains(TEXT("emit")))
    return TEXT("emissive");
  if (Name.Contains(TEXT("displacement")) || Name.Contains(TEXT("height")))
    return TEXT("displacement");
  if (Name.Contains(TEXT("fuzz")))
    return TEXT("fuzz");
  if (Name.Contains(TEXT("opacitymasked")) ||
      Name.Contains(TEXT("opacity_mask")) ||
      Name.Contains(TEXT("opacitymask")))
    return TEXT("opacity");
  if (Name.Contains(TEXT("mask")) || Name.Contains(TEXT("ordp")))
    return TEXT("mask");
  if (Name.Contains(TEXT("specular")) || Name.Contains(TEXT("spec")))
    return TEXT("specular");
  if (Name.Contains(TEXT("opacity")) || Name.Contains(TEXT("alpha")) ||
      Name.Contains(TEXT("transparency")))
    return TEXT("opacity");
  return TEXT("");
}

static FString DetectModelSuffix(const FString &SourceFile) {
  const FString Name = FPaths::GetBaseFilename(SourceFile).ToLower();
  if (Name.Contains(TEXT("highpoly")) || Name.Contains(TEXT("_high")) ||
      Name.Contains(TEXT("-high")) || Name.EndsWith(TEXT("high")))
    return TEXT("High");
  if (Name.Contains(TEXT("lod0")))
    return TEXT("Lod0");
  if (Name.Contains(TEXT("lod1")))
    return TEXT("Lod1");
  if (Name.Contains(TEXT("lod2")))
    return TEXT("Lod2");
  if (Name.Contains(TEXT("lod3")))
    return TEXT("Lod3");
  if (Name.Contains(TEXT("ztool")) || Name.EndsWith(TEXT(".ztl")))
    return TEXT("Ztool");
  return TEXT("Mesh");
}

static FString ToSlotSuffix(const FString &SlotName) {
  if (SlotName.IsEmpty()) {
    return TEXT("Texture");
  }
  if (SlotName.Equals(TEXT("mask"), ESearchCase::IgnoreCase)) {
    return TEXT("M");
  }
  if (SlotName.Equals(TEXT("hdr"), ESearchCase::IgnoreCase)) {
    return TEXT("HDR");
  }
  if (SlotName.Equals(TEXT("subsurfacecolor"), ESearchCase::IgnoreCase)) {
    return TEXT("SSC");
  }
  FString Result = SlotName.ToLower();
  Result[0] = FChar::ToUpper(Result[0]);
  return Result;
}

static void AppendImportedObjects(UAssetImportTask *Task,
                                  TArray<UObject *> &OutObjects) {
  if (!Task) {
    return;
  }
  OutObjects.Append(Task->GetObjects());
  for (const FString &ImportedPath : Task->ImportedObjectPaths) {
    if (UObject *ImportedObject =
            StaticLoadObject(UObject::StaticClass(), nullptr, *ImportedPath)) {
      OutObjects.AddUnique(ImportedObject);
    }
  }
}

static UFbxImportUI *MakeStaticMeshImportOptions(bool bBuildNanite = true) {
  UFbxImportUI *ImportOptions = NewObject<UFbxImportUI>();
  ImportOptions->bImportMesh = true;
  ImportOptions->bImportMaterials = false;
  ImportOptions->bImportTextures = false;
  ImportOptions->bImportAnimations = false;
  ImportOptions->bImportAsSkeletal = false;
  ImportOptions->bAutomatedImportShouldDetectType = false;
  ImportOptions->MeshTypeToImport = FBXIT_StaticMesh;
  if (ImportOptions->StaticMeshImportData) {
    ImportOptions->StaticMeshImportData->bGenerateLightmapUVs = false;
    ImportOptions->StaticMeshImportData->bAutoGenerateCollision = false;
    // Honor the FBX file unit (cm/m). Legacy foliage OPAQUE FBXs were
    // exported as meters (UnitScaleFactor=100) with a 0.01 root scale; the
    // cm conversion cancels that scale. cm-native FBX files are unchanged.
    ImportOptions->StaticMeshImportData->bConvertSceneUnit = true;
    // These meshes are always finalized as Nanite. Set it before the initial
    // FBX build instead of first constructing a full conventional render mesh.
    // Temporary LOD sources skip the build because only their mesh description
    // is copied into the destination mesh.
    ImportOptions->StaticMeshImportData->bBuildNanite = bBuildNanite;
    // Preserve authored normals; UE still generates missing normals/tangents.
    ImportOptions->StaticMeshImportData->NormalImportMethod = FBXNIM_ImportNormals;
  }
  return ImportOptions;
}

static UStaticMesh *ImportStaticMeshAsset(FAssetToolsModule &AssetToolsModule,
                                          const FString &SourceFile,
                                          const FString &DestinationPath,
                                          const FString &DestinationName,
                                          bool bBuildNanite = true) {
  UAssetImportTask *Task = NewObject<UAssetImportTask>();
  Task->Filename = SourceFile;
  Task->DestinationPath = DestinationPath;
  Task->DestinationName = DestinationName;
  Task->bReplaceExisting = true;
  Task->bAutomated = true;
  Task->bAsync = false;
  Task->bSave = false;
  Task->Options = MakeStaticMeshImportOptions(bBuildNanite);
  AssetToolsModule.Get().ImportAssetTasks({Task});

  TArray<UObject *> ImportedObjects;
  AppendImportedObjects(Task, ImportedObjects);
  for (UObject *ImportedObject : ImportedObjects) {
    if (UStaticMesh *StaticMesh = Cast<UStaticMesh>(ImportedObject)) {
      return StaticMesh;
    }
  }
  return nullptr;
}

static FString NormalizeModelVariantKey(const FString &RawValue) {
  FString Value = RawValue;
  Value.TrimStartAndEndInline();
  if (Value.IsEmpty()) {
    return TEXT("01");
  }

  const FRegexPattern PrefixPattern(
      TEXT("^(?:var(?:iant)?|variation)[\\s._-]*(.+)$"));
  FRegexMatcher PrefixMatcher(PrefixPattern, Value);
  if (PrefixMatcher.FindNext()) {
    Value = PrefixMatcher.GetCaptureGroup(1);
  }

  FString CleanValue;
  CleanValue.Reserve(Value.Len());
  for (const TCHAR Character : Value) {
    if (FChar::IsAlnum(Character) || Character == TEXT('_') ||
        Character == TEXT('-')) {
      CleanValue.AppendChar(Character);
    }
  }
  CleanValue.TrimStartAndEndInline();
  CleanValue.ReplaceInline(TEXT("_"), TEXT(""));
  CleanValue.ReplaceInline(TEXT("-"), TEXT(""));
  if (CleanValue.IsEmpty()) {
    return TEXT("01");
  }

  if (CleanValue.IsNumeric()) {
    const int32 NumericValue = FCString::Atoi(*CleanValue);
    if (NumericValue > 0) {
      return FString::Printf(TEXT("%02d"), NumericValue);
    }
    return TEXT("01");
  }

  CleanValue.ToUpperInline();
  return CleanValue.Left(32);
}

// The library marks transparency-cut foliage variants with an OPAQUE suffix.
// The canonical file form is 01_OPAQUE; the legacy compact form 01OPAQUE and
// metadata keys such as 01OPAQUE remain supported. Cut meshes keep the asset
// texture set but must render as true Opaque surfaces instead of Masked.
static bool IsOpaqueModelVariantKey(const FString &VariantKey) {
  FString Value = VariantKey.TrimStartAndEnd().ToUpper();
  Value.ReplaceInline(TEXT("_"), TEXT(""));
  Value.ReplaceInline(TEXT("-"), TEXT(""));
  return Value.EndsWith(TEXT("OPAQUE"));
}

static bool IsAcceptedModelVariantToken(const FString &Token) {
  const FString LowerToken = Token.ToLower();
  if (LowerToken.IsEmpty() || LowerToken == TEXT("base") ||
      LowerToken == TEXT("default") || LowerToken == TEXT("high") ||
      LowerToken == TEXT("highpoly") || LowerToken == TEXT("low") ||
      LowerToken == TEXT("mid") || LowerToken == TEXT("preview") ||
      LowerToken == TEXT("render") || LowerToken == TEXT("thumb") ||
      LowerToken == TEXT("thumbnail")) {
    return false;
  }
  if (LowerToken.StartsWith(TEXT("lod"))) {
    return false;
  }
  if (LowerToken == TEXT("1k") || LowerToken == TEXT("2k") ||
      LowerToken == TEXT("4k") || LowerToken == TEXT("8k") ||
      LowerToken == TEXT("16k") || LowerToken == TEXT("32k")) {
    return false;
  }
  if (Token.Len() > 6) {
    return false;
  }
  for (const TCHAR Character : Token) {
    if (!FChar::IsAlnum(Character)) {
      return false;
    }
  }
  return true;
}

static bool IsBarePlantVariantToken(const FString &Token) {
  if (!IsAcceptedModelVariantToken(Token)) {
    return false;
  }
  if (Token.IsNumeric()) {
    return true;
  }
  if (Token.Len() == 1 && FChar::IsAlpha(Token[0])) {
    return true;
  }
  const FRegexPattern BarePattern(
      TEXT("^(?:[A-Za-z]\\d{1,2}|\\d{1,3}[A-Za-z])$"));
  FRegexMatcher BareMatcher(BarePattern, Token);
  return BareMatcher.FindNext();
}

static bool ExtractPlantVariantAndLod(const FString &SourceFile,
                                      FString &OutVariantKey,
                                      int32 &OutLodIndex) {
  const FString Base = FPaths::GetBaseFilename(SourceFile).ToLower();
  const FString FullPath = SourceFile.Replace(TEXT("\\"), TEXT("/")).ToLower();
  OutVariantKey = TEXT("01");
  OutLodIndex = 0;

  {
    const FRegexPattern LodPattern(TEXT("lod(\\d+)"));
    FRegexMatcher LodMatcher(LodPattern, Base);
    if (LodMatcher.FindNext()) {
      const FString Token = LodMatcher.GetCaptureGroup(1);
      OutLodIndex = FMath::Max(0, FCString::Atoi(*Token));
    }
  }

  {
    const FRegexPattern VariantNamedPattern(
        TEXT("(?:^|[/_\\-.])(?:var(?:iant)?|variation)_?([A-Za-z0-9]{1,6})(?=$|[/_\\-.])"));
    FRegexMatcher VariantNamedMatcher(VariantNamedPattern, FullPath);
    FString Token;
    while (VariantNamedMatcher.FindNext()) {
      Token = VariantNamedMatcher.GetCaptureGroup(1);
    }
    if (!Token.IsEmpty() && IsAcceptedModelVariantToken(Token)) {
      OutVariantKey = NormalizeModelVariantKey(Token);
      return true;
    }
  }

  {
    const FRegexPattern VariantPattern(
        TEXT("(^|[_\\-.])([A-Za-z0-9]{1,6})(?:$|[_\\-.])"));
    FRegexMatcher VariantMatcher(VariantPattern, Base);
    FString Token;
    while (VariantMatcher.FindNext()) {
      const FString Candidate = VariantMatcher.GetCaptureGroup(2);
      if (IsBarePlantVariantToken(Candidate)) {
        Token = Candidate;
      }
    }
    if (!Token.IsEmpty()) {
      OutVariantKey = NormalizeModelVariantKey(Token);
    }
  }

  return true;
}

static FString NormalizeModelSourceKey(const FString &SourceFile) {
  return FPaths::ConvertRelativePathToFull(SourceFile)
      .Replace(TEXT("\\"), TEXT("/"))
      .ToLower();
}

static bool ReadModelVariantKey(const TSharedPtr<FJsonObject> &Object,
                                FString &OutVariantKey) {
  if (!Object.IsValid()) {
    return false;
  }

  const TCHAR *StringFields[] = {TEXT("variantKey"), TEXT("variation"),
                                 TEXT("variant"), TEXT("variantName"),
                                 TEXT("variationName"), TEXT("variantNumber")};
  for (const TCHAR *FieldName : StringFields) {
    FString TextValue;
    if (Object->TryGetStringField(FieldName, TextValue) &&
        !TextValue.TrimStartAndEnd().IsEmpty()) {
      OutVariantKey = NormalizeModelVariantKey(TextValue);
      return true;
    }
  }

  double NumericValue = 0.0;
  if (Object->TryGetNumberField(TEXT("variantNumber"), NumericValue) &&
      NumericValue > 0.0) {
    OutVariantKey = NormalizeModelVariantKey(
        FString::FromInt(FMath::Max(1, FMath::RoundToInt(NumericValue))));
    return true;
  }

  return false;
}

static FString ResolveModelVariantKey(
    const FString &SourceFile,
    const TMap<FString, FString> &ExplicitVariantByFile,
    int32 FallbackVariantNumber) {
  const FString SourceKey = NormalizeModelSourceKey(SourceFile);
  if (const FString *ExplicitVariant = ExplicitVariantByFile.Find(SourceKey)) {
    if (!ExplicitVariant->IsEmpty()) {
      return NormalizeModelVariantKey(*ExplicitVariant);
    }
  }

  FString ParsedVariant = TEXT("01");
  int32 LodIndex = 0;
  ExtractPlantVariantAndLod(SourceFile, ParsedVariant, LodIndex);
  if (!ParsedVariant.IsEmpty()) {
    return ParsedVariant;
  }
  return NormalizeModelVariantKey(FString::FromInt(FMath::Max(1, FallbackVariantNumber)));
}

// Grass/Bush 有两种导出方式（软件侧导出面板选择）：OPAQUE 版本 = 裁切模型 +
// Opaque 材质、不带 LOD；LOD 版本 = 原始 Masked 模型 + 该变体自己的 LOD 链。
// 这里读取 LOD 版本的目标 LOD 列表（lod 已是目标序号 1..N）。
struct FPlantModelLodEntry {
  FString SourceFile;
  FString VariantKey = TEXT("01");
  int32 LodIndex = 0;
};

static void CollectPlantModelLodPlan(
    const TSharedPtr<FJsonObject> &AssetObject,
    TMap<FString, TArray<FPlantModelLodEntry>> &OutLodsByVariant) {
  OutLodsByVariant.Reset();
  if (!AssetObject.IsValid()) {
    return;
  }
  const TArray<TSharedPtr<FJsonValue>> *ModelLods = nullptr;
  if (!AssetObject->TryGetArrayField(TEXT("modelLods"), ModelLods) ||
      ModelLods == nullptr) {
    return;
  }
  for (const TSharedPtr<FJsonValue> &LodValue : *ModelLods) {
    if (!LodValue.IsValid() || LodValue->Type != EJson::Object) {
      continue;
    }
    const TSharedPtr<FJsonObject> LodObject = LodValue->AsObject();
    if (!LodObject.IsValid()) {
      continue;
    }
    FString SourceFile;
    if (!LodObject->TryGetStringField(TEXT("file"), SourceFile) &&
        !LodObject->TryGetStringField(TEXT("path"), SourceFile) &&
        !LodObject->TryGetStringField(TEXT("uri"), SourceFile)) {
      continue;
    }
    if (SourceFile.IsEmpty() || !FPaths::FileExists(SourceFile)) {
      UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"), *SourceFile);
      continue;
    }
    int32 LodIndex = 0;
    double RawLodIndex = 0.0;
    if (LodObject->TryGetNumberField(TEXT("lod"), RawLodIndex)) {
      LodIndex = FMath::Max(0, FMath::RoundToInt(RawLodIndex));
    }
    FString VariantKey;
    if (!ReadModelVariantKey(LodObject, VariantKey)) {
      const TMap<FString, FString> EmptyVariantMap;
      VariantKey = ResolveModelVariantKey(SourceFile, EmptyVariantMap, 1);
    }
    FPlantModelLodEntry Entry;
    Entry.SourceFile = SourceFile;
    Entry.VariantKey = NormalizeModelVariantKey(VariantKey);
    Entry.LodIndex = LodIndex;
    OutLodsByVariant.FindOrAdd(Entry.VariantKey).Add(Entry);
  }
  for (TPair<FString, TArray<FPlantModelLodEntry>> &Pair : OutLodsByVariant) {
    Pair.Value.Sort([](const FPlantModelLodEntry &A,
                       const FPlantModelLodEntry &B) {
      if (A.LodIndex != B.LodIndex) {
        return A.LodIndex < B.LodIndex;
      }
      return A.SourceFile < B.SourceFile;
    });
  }
}

// Merge the exported LOD chain into the imported base mesh (Masked 植被的 LOD
// 版本). The custom LOD mesh descriptions live inside the destination package
// afterwards, so the meshes imported only as LOD sources are deleted again.
static int32 ImportPlantCustomLods(FAssetToolsModule &AssetToolsModule,
                                   UStaticMesh *BaseMesh,
                                   const TArray<FPlantModelLodEntry> &Lods,
                                   const FString &AssetFolder,
                                   const FString &BaseMeshName) {
  if (!BaseMesh) {
    return 0;
  }
  int32 MergedLodCount = 0;
  for (const FPlantModelLodEntry &Lod : Lods) {
    // LOD0 is the base mesh that was imported already.
    if (Lod.LodIndex <= 0 || Lod.SourceFile.IsEmpty()) {
      continue;
    }
    const FString TempMeshName =
        FString::Printf(TEXT("TEMP_%s_LOD%d"), *BaseMeshName, Lod.LodIndex);
    UStaticMesh *LodMesh =
        ImportStaticMeshAsset(AssetToolsModule, Lod.SourceFile, AssetFolder,
                              TempMeshName, /*bBuildNanite=*/false);
    if (!LodMesh) {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive import: failed to import LOD source %s"),
             *Lod.SourceFile);
      continue;
    }
    if (BaseMesh->SetCustomLOD(LodMesh, Lod.LodIndex, Lod.SourceFile)) {
      MergedLodCount += 1;
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive import: failed to set LOD %d from %s"),
             Lod.LodIndex, *Lod.SourceFile);
    }
    ObjectTools::DeleteSingleObject(LodMesh,
                                    /*bPerformReferenceCheck=*/false);
  }
  return MergedLodCount;
}

static UFoliageType_InstancedStaticMesh *
CreateFoliageTypeAsset(const FString &AssetFolder, const FString &AssetName,
                       UStaticMesh *StaticMesh) {
  if (!StaticMesh) {
    return nullptr;
  }
  const FString FoliageAssetName = FString::Printf(TEXT("FT_%s"), *AssetName);
  const FString PackagePath = AssetFolder / FoliageAssetName;
  UPackage *Package = CreatePackage(*PackagePath);
  if (!Package) {
    return nullptr;
  }
  UFoliageType_InstancedStaticMesh *FoliageType =
      FindObject<UFoliageType_InstancedStaticMesh>(Package, *FoliageAssetName);
  const bool bIsNew = FoliageType == nullptr;
  if (!FoliageType) {
    FoliageType = NewObject<UFoliageType_InstancedStaticMesh>(
        Package, *FoliageAssetName, RF_Public | RF_Standalone);
  }
  if (!FoliageType) {
    return nullptr;
  }
  FoliageType->SetStaticMesh(StaticMesh);
  FoliageType->PostEditChange();
  FoliageType->MarkPackageDirty();
  FinalizeImportedAsset(FoliageType);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(FoliageType);
  }
  return FoliageType;
}

struct FTexturePixels {
  int32 Width = 0;
  int32 Height = 0;
  TArray<FColor> Pixels;
};

static bool ReadTexturePixels(UTexture2D *Texture, FTexturePixels &OutPixels) {
  if (!Texture) {
    return false;
  }
  constexpr int64 MaxTextureDimension = 16384;
  constexpr int64 MaxPixelCount = MaxTextureDimension * MaxTextureDimension;

  if (Texture->Source.IsValid()) {
    const int32 Width = Texture->Source.GetSizeX();
    const int32 Height = Texture->Source.GetSizeY();
    const int64 PixelCount64 =
        static_cast<int64>(Width) * static_cast<int64>(Height);
    if (Width <= 0 || Height <= 0 || Width > MaxTextureDimension ||
        Height > MaxTextureDimension || PixelCount64 <= 0 ||
        PixelCount64 > MaxPixelCount || PixelCount64 > MAX_int32) {
      return false;
    }
    const int32 PixelCount = static_cast<int32>(PixelCount64);

    TArray64<uint8> RawData;
    if (!Texture->Source.GetMipData(RawData, 0)) {
      return false;
    }

    const ETextureSourceFormat Format = Texture->Source.GetFormat();
    OutPixels.Width = Width;
    OutPixels.Height = Height;
    OutPixels.Pixels.SetNum(PixelCount);

    if (Format == TSF_BGRA8) {
      const int64 RequiredBytes =
          PixelCount64 * static_cast<int64>(sizeof(FColor));
      if (RequiredBytes <= 0 || RawData.Num() < RequiredBytes) {
        return false;
      }
      FMemory::Memcpy(OutPixels.Pixels.GetData(), RawData.GetData(),
                      PixelCount * sizeof(FColor));
      return true;
    }
    if (Format == TSF_G8) {
      if (RawData.Num() < PixelCount) {
        return false;
      }
      for (int32 Index = 0; Index < PixelCount; Index++) {
        const uint8 Value = RawData[Index];
        OutPixels.Pixels[Index] = FColor(Value, Value, Value, 255);
      }
      return true;
    }
  }
  const FTexturePlatformData *PlatformData = Texture->GetPlatformData();
  if (!PlatformData || PlatformData->Mips.Num() <= 0) {
    return false;
  }
  const FTexture2DMipMap &Mip = PlatformData->Mips[0];
  const int32 Width = Mip.SizeX;
  const int32 Height = Mip.SizeY;
  const int64 PixelCount64 =
      static_cast<int64>(Width) * static_cast<int64>(Height);
  if (Width <= 0 || Height <= 0 || Width > MaxTextureDimension ||
      Height > MaxTextureDimension || PixelCount64 <= 0 ||
      PixelCount64 > MaxPixelCount || PixelCount64 > MAX_int32) {
    return false;
  }
  const int32 PixelCount = static_cast<int32>(PixelCount64);
  const int64 RequiredRGBA = PixelCount64 * 4;
  OutPixels.Width = Width;
  OutPixels.Height = Height;
  OutPixels.Pixels.SetNum(PixelCount);

  const EPixelFormat PixelFormat = PlatformData->PixelFormat;
  const void *RawPtr = Mip.BulkData.LockReadOnly();
  if (!RawPtr) {
    Mip.BulkData.Unlock();
    return false;
  }
  const int64 RawSize = Mip.BulkData.GetBulkDataSize();
  bool bOk = false;
  if ((PixelFormat == PF_B8G8R8A8 || PixelFormat == PF_R8G8B8A8) &&
      RawSize >= RequiredRGBA) {
    const uint8 *Bytes = static_cast<const uint8 *>(RawPtr);
    for (int32 Index = 0; Index < PixelCount; Index++) {
      const int32 Offset = Index * 4;
      if (PixelFormat == PF_B8G8R8A8) {
        OutPixels.Pixels[Index] = FColor(Bytes[Offset + 2], Bytes[Offset + 1],
                                         Bytes[Offset], Bytes[Offset + 3]);
      } else {
        OutPixels.Pixels[Index] = FColor(Bytes[Offset], Bytes[Offset + 1],
                                         Bytes[Offset + 2], Bytes[Offset + 3]);
      }
    }
    bOk = true;
  } else if ((PixelFormat == PF_G8 || PixelFormat == PF_R8) &&
             RawSize >= PixelCount) {
    const uint8 *Bytes = static_cast<const uint8 *>(RawPtr);
    for (int32 Index = 0; Index < PixelCount; Index++) {
      const uint8 Value = Bytes[Index];
      OutPixels.Pixels[Index] = FColor(Value, Value, Value, 255);
    }
    bOk = true;
  }
  Mip.BulkData.Unlock();
  return bOk;
}

static uint8 SampleChannel(const FTexturePixels *Pixels, float U, float V,
                           int32 ChannelIndex, uint8 DefaultValue) {
  if (!Pixels || Pixels->Width <= 0 || Pixels->Height <= 0 ||
      Pixels->Pixels.IsEmpty()) {
    return DefaultValue;
  }
  const int32 X = FMath::Clamp(FMath::FloorToInt(U * (Pixels->Width - 1)), 0,
                               Pixels->Width - 1);
  const int32 Y = FMath::Clamp(FMath::FloorToInt(V * (Pixels->Height - 1)), 0,
                               Pixels->Height - 1);
  const FColor &Pixel = Pixels->Pixels[Y * Pixels->Width + X];
  if (ChannelIndex == 0)
    return Pixel.R;
  if (ChannelIndex == 1)
    return Pixel.G;
  if (ChannelIndex == 2)
    return Pixel.B;
  return Pixel.A;
}

static uint8 SampleLuminance(const FTexturePixels *Pixels, float U, float V,
                             uint8 DefaultValue) {
  if (!Pixels || Pixels->Width <= 0 || Pixels->Height <= 0 ||
      Pixels->Pixels.IsEmpty()) {
    return DefaultValue;
  }
  const int32 X = FMath::Clamp(FMath::FloorToInt(U * (Pixels->Width - 1)), 0,
                               Pixels->Width - 1);
  const int32 Y = FMath::Clamp(FMath::FloorToInt(V * (Pixels->Height - 1)), 0,
                               Pixels->Height - 1);
  const FColor &Pixel = Pixels->Pixels[Y * Pixels->Width + X];
  const float Luma =
      (0.2126f * Pixel.R) + (0.7152f * Pixel.G) + (0.0722f * Pixel.B);
  return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Luma), 0, 255));
}

static UTexture2D *CreatePackedMaskTexture(
    const FString &AssetFolder, const FString &TextureAssetName,
    UTexture2D *AOTexture, int32 AOChannel, UTexture2D *RoughnessTexture,
    int32 RoughnessChannel, UTexture2D *DisplacementTexture,
    int32 DisplacementChannel, UTexture2D *OpacityTexture, int32 OpacityChannel,
    UTexture2D *SizeRefA, UTexture2D *SizeRefB) {
  const bool HasAOInput = AOTexture != nullptr;
  const bool HasRoughnessInput = RoughnessTexture != nullptr;
  const bool HasDisplacementInput = DisplacementTexture != nullptr;
  FTexturePixels AOPixels;
  FTexturePixels RoughnessPixels;
  FTexturePixels DisplacementPixels;
  FTexturePixels OpacityPixels;
  const bool HasAO = ReadTexturePixels(AOTexture, AOPixels);
  const bool HasRoughness =
      ReadTexturePixels(RoughnessTexture, RoughnessPixels);
  const bool HasDisplacement =
      ReadTexturePixels(DisplacementTexture, DisplacementPixels);
  const bool HasOpacity = ReadTexturePixels(OpacityTexture, OpacityPixels);
  int32 Width = 0;
  int32 Height = 0;
  if (HasAO) {
    Width = AOPixels.Width;
    Height = AOPixels.Height;
  } else if (HasRoughness) {
    Width = RoughnessPixels.Width;
    Height = RoughnessPixels.Height;
  } else if (HasDisplacement) {
    Width = DisplacementPixels.Width;
    Height = DisplacementPixels.Height;
  } else if (HasOpacity) {
    Width = OpacityPixels.Width;
    Height = OpacityPixels.Height;
  } else {
    FTexturePixels RefPixels;
    if (ReadTexturePixels(SizeRefA, RefPixels) ||
        ReadTexturePixels(SizeRefB, RefPixels)) {
      Width = RefPixels.Width;
      Height = RefPixels.Height;
    } else {
      Width = 1024;
      Height = 1024;
    }
  }

  const FString PackagePath = AssetFolder / TextureAssetName;
  UPackage *Package = CreatePackage(*PackagePath);
  if (!Package) {
    return nullptr;
  }

  UTexture2D *PackedTexture = NewObject<UTexture2D>(Package, *TextureAssetName,
                                                    RF_Public | RF_Standalone);
  if (!PackedTexture) {
    return nullptr;
  }

  PackedTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);
  uint8 *DestData = PackedTexture->Source.LockMip(0);
  for (int32 Y = 0; Y < Height; Y++) {
    for (int32 X = 0; X < Width; X++) {
      const float U =
          Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1)
                    : 0.0f;
      const float V =
          Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1)
                     : 0.0f;
      const uint8 AOFallback =
          HasAOInput ? static_cast<uint8>(0) : static_cast<uint8>(255);
      const uint8 RoughnessFallback =
          HasRoughnessInput ? static_cast<uint8>(0) : static_cast<uint8>(204);
      const uint8 DisplacementFallback = HasDisplacementInput
                                             ? static_cast<uint8>(0)
                                             : static_cast<uint8>(128);
      const uint8 AOValue = SampleChannel(HasAO ? &AOPixels : nullptr, U, V,
                                          AOChannel, AOFallback);
      const uint8 RoughnessValue =
          SampleChannel(HasRoughness ? &RoughnessPixels : nullptr, U, V,
                        RoughnessChannel, RoughnessFallback);
      const uint8 DisplacementValue =
          SampleChannel(HasDisplacement ? &DisplacementPixels : nullptr, U, V,
                        DisplacementChannel, DisplacementFallback);
      const uint8 OpacityValue =
          SampleChannel(HasOpacity ? &OpacityPixels : nullptr, U, V,
                        OpacityChannel, 255);
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = DisplacementValue;
      DestData[DestIndex + 1] = RoughnessValue;
      DestData[DestIndex + 2] = AOValue;
      DestData[DestIndex + 3] = OpacityValue;
    }
  }
  PackedTexture->Source.UnlockMip(0);
  PackedTexture->CompressionSettings = TC_Masks;
  PackedTexture->CompressionNoAlpha = false;
  PackedTexture->SRGB = false;
  PackedTexture->PostEditChange();
  PackedTexture->MarkPackageDirty();
  ForceTextureDataReady(PackedTexture);
  FinalizeImportedAsset(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
}

static UTexture2D *CreatePackedORMTexture(
    const FString &AssetFolder, const FString &TextureAssetName,
    UTexture2D *AOTexture, UTexture2D *RoughnessTexture,
    UTexture2D *MetallicTexture, UTexture2D *SizeRefA, UTexture2D *SizeRefB) {
  FTexturePixels AOPixels;
  FTexturePixels RoughnessPixels;
  FTexturePixels MetallicPixels;
  const bool HasAO = ReadTexturePixels(AOTexture, AOPixels);
  const bool HasRoughness = ReadTexturePixels(RoughnessTexture, RoughnessPixels);
  const bool HasMetallic = ReadTexturePixels(MetallicTexture, MetallicPixels);
  int32 Width = 0;
  int32 Height = 0;
  if (HasAO) {
    Width = AOPixels.Width;
    Height = AOPixels.Height;
  } else if (HasRoughness) {
    Width = RoughnessPixels.Width;
    Height = RoughnessPixels.Height;
  } else if (HasMetallic) {
    Width = MetallicPixels.Width;
    Height = MetallicPixels.Height;
  } else {
    FTexturePixels RefPixels;
    if (ReadTexturePixels(SizeRefA, RefPixels) ||
        ReadTexturePixels(SizeRefB, RefPixels)) {
      Width = RefPixels.Width;
      Height = RefPixels.Height;
    } else {
      Width = 1024;
      Height = 1024;
    }
  }

  const FString PackagePath = AssetFolder / TextureAssetName;
  UPackage *Package = CreatePackage(*PackagePath);
  if (!Package) {
    return nullptr;
  }

  UTexture2D *PackedTexture = NewObject<UTexture2D>(Package, *TextureAssetName,
                                                    RF_Public | RF_Standalone);
  if (!PackedTexture) {
    return nullptr;
  }

  PackedTexture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);
  uint8 *DestData = PackedTexture->Source.LockMip(0);
  for (int32 Y = 0; Y < Height; Y++) {
    for (int32 X = 0; X < Width; X++) {
      const float U =
          Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1)
                    : 0.0f;
      const float V =
          Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1)
                     : 0.0f;
      const uint8 AOValue =
          SampleChannel(HasAO ? &AOPixels : nullptr, U, V, 0, 255);
      const uint8 RoughnessValue =
          SampleChannel(HasRoughness ? &RoughnessPixels : nullptr, U, V, 0, 204);
      const uint8 MetallicValue =
          SampleChannel(HasMetallic ? &MetallicPixels : nullptr, U, V, 0, 0);
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = MetallicValue;
      DestData[DestIndex + 1] = RoughnessValue;
      DestData[DestIndex + 2] = AOValue;
      DestData[DestIndex + 3] = 255;
    }
  }
  PackedTexture->Source.UnlockMip(0);
  PackedTexture->CompressionSettings = TC_Masks;
  PackedTexture->CompressionNoAlpha = true;
  PackedTexture->SRGB = false;
  PackedTexture->PostEditChange();
  PackedTexture->MarkPackageDirty();
  ForceTextureDataReady(PackedTexture);
  FinalizeImportedAsset(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
}

// 3D Plants 纹理种类（md §5.2）：Diffuse（D，albedo 代用）/ Normal（N）/
// ORM / OpacityMasked（O，仅 masked 资产）。
enum class EPlantTextureKind { Diffuse, Normal, ORM, OpacityMasked,
                                SubsurfaceColor };

// 植被纹理预设：Tree / Bush / Grass 1024-2048、HeroFoliage 1024-4096、
// Micro 1024 且不使用 VT；VT 分档（>= 2048 开）。
static void ApplyPlantTexturePreset(UTexture2D *Texture,
                                    EPlantTextureKind Kind,
                                    const FPlantAssetProfile &Profile,
                                    bool bUseVT) {
  if (!Texture) {
    return;
  }
  const int32 ActualMax = FMath::Max(Texture->GetSizeX(), Texture->GetSizeY());
  int32 DesiredSize =
      Profile.TextureMaxSize > 0 ? Profile.TextureMaxSize : ActualMax;
  if (ActualMax > 0) {
    DesiredSize = FMath::Min(DesiredSize, ActualMax);
  }
  Texture->MaxTextureSize = FMath::Clamp(DesiredSize, 256, 8192);
  Texture->MipGenSettings = TMGS_FromTextureGroup;
  Texture->VirtualTextureStreaming =
      bUseVT && Profile.bAllowVirtualTexture &&
      Texture->MaxTextureSize >= FMath::Max(1024, Profile.TextureVTSize);

  switch (Kind) {
  case EPlantTextureKind::Diffuse:
    Texture->CompressionSettings = TC_Default;
    Texture->SRGB = true;
    Texture->CompressionNoAlpha = false;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Low;
    break;
  case EPlantTextureKind::Normal:
    Texture->CompressionSettings = TC_Normalmap;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
    Texture->LossyCompressionAmount = TLCA_Low;
    break;
  case EPlantTextureKind::SubsurfaceColor:
    // 植被纹理预设（export-3dplants.html）：Emissive / SSC = sRGB + Default(BC1)。
    Texture->CompressionSettings = TC_Default;
    Texture->SRGB = true;
    Texture->CompressionNoAlpha = true;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
    break;
  case EPlantTextureKind::ORM:
  case EPlantTextureKind::OpacityMasked:
  default:
    Texture->CompressionSettings = TC_Masks;
    Texture->SRGB = false;
    Texture->CompressionNoAlpha = false;
    Texture->LODGroup = TEXTUREGROUP_World;
    Texture->LossyCompressionAmount = TLCA_Medium;
    break;
  }
}

// 把源贴图逐张落成独立的植被贴图资产（不做 Albedo/NRS 打包）。
static UTexture2D *CreatePlantTextureAsset(const FString &AssetFolder,
                                           const FString &AssetName,
                                           UTexture2D *SourceTexture,
                                           EPlantTextureKind Kind,
                                           const FPlantAssetProfile &Profile,
                                           bool bUseVT) {
  FTexturePixels SourcePixels;
  if (!ReadTexturePixels(SourceTexture, SourcePixels)) {
    return nullptr;
  }
  const int32 Width = SourcePixels.Width;
  const int32 Height = SourcePixels.Height;
  const FString PackagePath = AssetFolder / AssetName;
  UPackage *Package = CreatePackage(*PackagePath);
  if (!Package) {
    return nullptr;
  }
  UTexture2D *Texture = NewObject<UTexture2D>(Package, *AssetName,
                                              RF_Public | RF_Standalone);
  if (!Texture) {
    return nullptr;
  }
  Texture->Source.Init(Width, Height, 1, 1, TSF_BGRA8);
  uint8 *DestData = Texture->Source.LockMip(0);
  FMemory::Memcpy(DestData, SourcePixels.Pixels.GetData(),
                  static_cast<SIZE_T>(Width) * static_cast<SIZE_T>(Height) * 4);
  Texture->Source.UnlockMip(0);
  ApplyPlantTexturePreset(Texture, Kind, Profile, bUseVT);
  Texture->PostEditChange();
  Texture->MarkPackageDirty();
  ForceTextureDataReady(Texture);
  FinalizeImportedAsset(Texture);
  FAssetRegistryModule::AssetCreated(Texture);
  return Texture;
}


// Match Megascans: opt into VT conversion only when both project switches are on.
static bool IsAssetVirtualTextureImportEnabled() {
  const auto *VirtualTextures = IConsoleManager::Get().FindTConsoleVariableDataInt(
      TEXT("r.VirtualTextures"));
  const auto *AutoImport = IConsoleManager::Get().FindTConsoleVariableDataInt(
      TEXT("r.VT.EnableAutoImport"));
  return VirtualTextures && AutoImport &&
         VirtualTextures->GetValueOnAnyThread() != 0 &&
         AutoImport->GetValueOnAnyThread() != 0;
}

// Keep all overridden samplers compatible, including textures composed after import.
static UMaterialInterface *LoadAssetMaterialParent(
    const TCHAR *BaseName, bool bUseVT, TArray<UTexture *> Textures) {
  const FString ParentName = FString(BaseName) + (bUseVT ? TEXT("_VT") : TEXT(""));
  const FString ParentPath = FString::Printf(
      TEXT("/Game/Common/MaterialInstance/%s.%s"), *ParentName, *ParentName);
  UMaterialInterface *ParentMaterial = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing parent material: %s"), *ParentPath);
    return nullptr;
  }
  if (bUseVT) {
    for (UTexture *Texture : Textures) {
      if (Texture && !Texture->VirtualTextureStreaming) {
        // Settle any pending import build before toggling VT.
        ForceTextureDataReady(Texture);
        Texture->Modify();
        Texture->PreEditChange(nullptr);
        Texture->VirtualTextureStreaming = true;
        Texture->PostEditChange();
        Texture->MarkPackageDirty();
        FinalizeImportedAsset(Texture);
      }
    }
  }
  for (UTexture *Texture : Textures) {
    ForceTextureDataReady(Texture);
  }
  return ParentMaterial;
}

static UMaterialInstanceConstant *
CreateAssetMaterialInstance(const FString &AssetFolder,
                            const FString &AssetName, UTexture *AlbedoTexture,
                            UTexture *NormalTexture, UTexture *MaskTexture,
                            UTexture *FuzzTexture, bool bUseVT) {
  const bool HasFuzz = FuzzTexture != nullptr;
  UMaterialInterface *ParentMaterial = LoadAssetMaterialParent(
      HasFuzz ? TEXT("MMI_GeneralMat_Fuzz") : TEXT("MMI_GeneralMat"), bUseVT,
      {AlbedoTexture, NormalTexture, MaskTexture, FuzzTexture});
  if (!ParentMaterial) {
    return nullptr;
  }

  const FString MaterialAssetName = FString::Printf(TEXT("MI_%s"), *AssetName);
  const FString MaterialPackagePath = AssetFolder / MaterialAssetName;
  UPackage *MaterialPackage = CreatePackage(*MaterialPackagePath);
  UMaterialInstanceConstant *MaterialInstance =
      FindObject<UMaterialInstanceConstant>(MaterialPackage,
                                            *MaterialAssetName);
  const bool bIsNew = MaterialInstance == nullptr;
  if (!MaterialInstance) {
    MaterialInstance = NewObject<UMaterialInstanceConstant>(
        MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
  }
  if (!MaterialInstance) {
    return nullptr;
  }
  MaterialInstance->SetParentEditorOnly(ParentMaterial);

  if (AlbedoTexture) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(TEXT("Albedo"))), AlbedoTexture);
  }
  if (MaskTexture) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(TEXT("Mask"))), MaskTexture);
  }
  if (NormalTexture) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(TEXT("Normal"))), NormalTexture);
  }
  if (FuzzTexture) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(TEXT("fuzzmap"))), FuzzTexture);
  }

  // Publish the complete parameter set once, after texture compilation.
  UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  MaterialInstance->MarkPackageDirty();
  FinalizeImportedAsset(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  AssetHiveThumbnailRefresh::Queue(MaterialInstance);
  return MaterialInstance;
}

static FString NormalizeMaterialParameterToken(const FString &Value) {
  FString Token = Value;
  Token.ReplaceInline(TEXT(" "), TEXT(""));
  Token.ReplaceInline(TEXT("_"), TEXT(""));
  return Token.ToLower();
}

// Static switches usually live in the base material graph, while an instance
// only reports the switches it already overrides. Walk the parent chain (with
// a normalized name fallback) so an imported instance can still drive it.
static bool ResolveStaticSwitchParameterName(UMaterialInterface *Material,
                                             const FString &ConfiguredName,
                                             FName &OutName) {
  if (!Material || ConfiguredName.IsEmpty()) {
    return false;
  }
  const FName Configured(ConfiguredName);
  bool bDefaultValue = false;
  FGuid ExpressionGuid;
  if (Material->GetStaticSwitchParameterDefaultValue(
          FHashedMaterialParameterInfo(Configured), bDefaultValue,
          ExpressionGuid)) {
    OutName = Configured;
    return true;
  }
  const FString Wanted = NormalizeMaterialParameterToken(ConfiguredName);
  for (UMaterialInterface *Cursor = Material; Cursor;) {
    TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
    Cursor->GetAllParametersOfType(EMaterialParameterType::StaticSwitch,
                                   Parameters);
    for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>
             &Parameter : Parameters) {
      const FString Candidate = Parameter.Key.Name.ToString();
      if (Candidate.Equals(ConfiguredName, ESearchCase::IgnoreCase) ||
          NormalizeMaterialParameterToken(Candidate) == Wanted) {
        OutName = Parameter.Key.Name;
        return true;
      }
    }
    UMaterialInstance *Instance = Cast<UMaterialInstance>(Cursor);
    Cursor = Instance ? Instance->Parent : nullptr;
  }
  return false;
}

static UMaterialInstanceConstant *
CreateEnvironmentAssetMaterialInstance(
    const FString &MaterialFolder, const FString &MaterialAssetName,
    const FEnvironmentAssetProfile &Profile, UTexture *AlbedoTexture,
    UTexture *NormalTexture, UTexture *ORMTexture, UTexture *MegaMaskTexture,
    UTexture *OpacityTexture, UTexture *EmissiveTexture, bool bMasked) {
  const bool bUseMaskedParent = bMasked && Profile.bBaseFamily;
  UMaterialInterface *ParentMaterial = UAssetHiveSettings::GetAssetParentMaterial(
      Profile.TypeKey, bUseMaskedParent);
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error,
           TEXT("AssetHive: missing 3D asset parent material: %s"),
           *UAssetHiveSettings::GetAssetParentMaterialPath(Profile.TypeKey,
                                                           bUseMaskedParent));
    return nullptr;
  }
  for (UTexture *Texture :
       {AlbedoTexture, NormalTexture, ORMTexture, MegaMaskTexture,
        OpacityTexture, EmissiveTexture}) {
    ForceTextureDataReady(Texture);
  }

  const FString MaterialPackagePath = MaterialFolder / MaterialAssetName;
  UPackage *MaterialPackage = CreatePackage(*MaterialPackagePath);
  UMaterialInstanceConstant *MaterialInstance =
      FindObject<UMaterialInstanceConstant>(MaterialPackage,
                                            *MaterialAssetName);
  const bool bIsNew = MaterialInstance == nullptr;
  if (!MaterialInstance) {
    MaterialInstance = NewObject<UMaterialInstanceConstant>(
        MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
  }
  if (!MaterialInstance) {
    return nullptr;
  }
  MaterialInstance->SetParentEditorOnly(ParentMaterial);

  const FString AlbedoParameter = UAssetHiveSettings::GetAssetAlbedoParameter();
  const FString NormalParameter = UAssetHiveSettings::GetAssetNormalParameter();
  const FString ORMParameter =
      UAssetHiveSettings::GetAssetORMTextureParameter();
  const FString MaskParameter = UAssetHiveSettings::GetAssetMaskParameter();
  const FString MegaMaskParameter =
      UAssetHiveSettings::GetAssetMegaMaskParameter();
  const FString EmissiveParameter =
      UAssetHiveSettings::GetAssetEmissiveParameter(Profile.TypeKey);
  const FString EmissiveSwitch =
      UAssetHiveSettings::GetAssetUseEmissiveSwitch();

  if (AlbedoTexture && !AlbedoParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*AlbedoParameter)), AlbedoTexture);
  }
  if (NormalTexture && !NormalParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*NormalParameter)), NormalTexture);
  }
  if (ORMTexture && !ORMParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*ORMParameter)), ORMTexture);
  }
  if (EmissiveTexture && !EmissiveParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*EmissiveParameter)), EmissiveTexture);
  }
  if (Profile.bMega && MegaMaskTexture && !MegaMaskParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*MegaMaskParameter)), MegaMaskTexture);
  } else if (Profile.bBaseFamily && bMasked && OpacityTexture &&
             !MaskParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*MaskParameter)), OpacityTexture);
  }
  if (!EmissiveSwitch.IsEmpty()) {
    FName EmissiveSwitchName = NAME_None;
    if (ResolveStaticSwitchParameterName(ParentMaterial, EmissiveSwitch,
                                         EmissiveSwitchName)) {
      MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
          FMaterialParameterInfo(EmissiveSwitchName),
          EmissiveTexture != nullptr);
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive: static switch '%s' not found on %s; emissive "
                  "state keeps the parent default"),
             *EmissiveSwitch, *ParentMaterial->GetPathName());
    }
  }

  UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  MaterialInstance->MarkPackageDirty();
  FinalizeImportedAsset(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  AssetHiveThumbnailRefresh::Queue(MaterialInstance);
  return MaterialInstance;
}
static UMaterialInterface *LoadSurfaceMaterialParent(
    const TArray<UTexture *> &Textures) {
  UMaterialInterface *ParentMaterial =
      UAssetHiveSettings::GetSurfaceParentMaterial();
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing Surface parent material: %s"),
           *UAssetHiveSettings::GetSurfaceParentMaterialPath());
    return nullptr;
  }
  for (UTexture *Texture : Textures) {
    ForceTextureDataReady(Texture);
  }
  return ParentMaterial;
}

static UMaterialInterface *LoadDecalMaterialParent(
    const TArray<UTexture *> &Textures, const FString &DecalParentMode) {
  UMaterialInterface *ParentMaterial =
      UAssetHiveSettings::GetDecalParentMaterial(DecalParentMode);
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing Decal parent material: %s"),
           *UAssetHiveSettings::GetDecalParentMaterialPath(DecalParentMode));
    return nullptr;
  }
  for (UTexture *Texture : Textures) {
    ForceTextureDataReady(Texture);
  }
  return ParentMaterial;
}

static UMaterialInstanceConstant *CreateSurfaceMaterialInstance(
    const FString &MaterialFolder, const FString &AssetName, int32 GroupId,
    UTexture *BCRTexture, UTexture *NormalTexture, UTexture *MetallicTexture,
    UTexture *EmissiveTexture, double BaseTiling) {
  UMaterialInterface *ParentMaterial = LoadSurfaceMaterialParent(
      {BCRTexture, NormalTexture, MetallicTexture, EmissiveTexture});
  if (!ParentMaterial) {
    return nullptr;
  }
  const UAssetHiveSettings *Settings = GetDefault<UAssetHiveSettings>();
  const FString MaterialAssetName =
      UAssetHiveSettings::GetSurfaceMaterialName(AssetName, GroupId);
  const FString MaterialPackagePath = MaterialFolder / MaterialAssetName;
  UPackage *MaterialPackage = CreatePackage(*MaterialPackagePath);
  UMaterialInstanceConstant *MaterialInstance =
      FindObject<UMaterialInstanceConstant>(MaterialPackage, *MaterialAssetName);
  const bool bIsNew = MaterialInstance == nullptr;
  if (!MaterialInstance) {
    MaterialInstance = NewObject<UMaterialInstanceConstant>(
        MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
  }
  if (!MaterialInstance) {
    return nullptr;
  }
  MaterialInstance->SetParentEditorOnly(ParentMaterial);

  const FString BCRParameter = Settings->SurfaceBCRParameter.TrimStartAndEnd();
  const FString NormalParameter = Settings->SurfaceNormalParameter.TrimStartAndEnd();
  const FString MetallicParameter = Settings->SurfaceMetallicParameter.TrimStartAndEnd();
  const FString EmissiveParameter = Settings->SurfaceEmissiveParameter.TrimStartAndEnd();
  const FString MetallicSwitch = Settings->SurfaceUseMetallicSwitch.TrimStartAndEnd();
  const FString EmissiveSwitch = Settings->SurfaceUseEmissiveSwitch.TrimStartAndEnd();
  const FString TilingParameter = Settings->SurfaceTilingParameter.TrimStartAndEnd();

  if (BCRTexture && !BCRParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*BCRParameter)), BCRTexture);
  }
  if (NormalTexture && !NormalParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*NormalParameter)), NormalTexture);
  }
  if (MetallicTexture && !MetallicParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*MetallicParameter)), MetallicTexture);
  }
  if (!MetallicSwitch.IsEmpty()) {
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*MetallicSwitch)), MetallicTexture != nullptr);
  }
  if (EmissiveTexture && !EmissiveParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*EmissiveParameter)), EmissiveTexture);
  }
  if (!EmissiveSwitch.IsEmpty()) {
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*EmissiveSwitch)), EmissiveTexture != nullptr);
  }
  if (!TilingParameter.IsEmpty()) {
    const float Tiling = FMath::Max(0.0001f, static_cast<float>(BaseTiling));
    MaterialInstance->SetVectorParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*TilingParameter)),
        FLinearColor(Tiling, Tiling, 0.0f, 0.0f));
  }

  UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  MaterialInstance->MarkPackageDirty();
  FinalizeImportedAsset(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  AssetHiveThumbnailRefresh::Queue(MaterialInstance);
  return MaterialInstance;
}

// SSC（SubsurfaceColor）纹理参数名在不同植被母材质上命名不同：
// GrassBend Opaque / Masked (MI_Env_GrassBend_ST_VT / MI_Env_GrassBend_Masked_ST_VT) use SubsurfaceColor_VT.
// Tree / Bush / Grass（M_Env_Tree_ST 等）用 SubsurfaceColor_VT。
// 这里先在父材质链上解析真实存在的纹理参数，避免写入不存在的参数。
static bool ResolvePlantSubsurfaceParameterName(UMaterialInterface *Material,
                                                const FString &ConfiguredName,
                                                FName &OutName) {
  if (!Material || ConfiguredName.IsEmpty()) {
    return false;
  }
  const TCHAR *FallbackNames[] = {TEXT("SubsurfaceColor_VT"),
                                  TEXT("SubsurfaceColor"),
                                  TEXT("Subsurface_VT"), TEXT("Subsurface")};
  TArray<FString> Candidates;
  Candidates.Add(ConfiguredName);
  for (const TCHAR *FallbackName : FallbackNames) {
    Candidates.Add(FString(FallbackName));
  }
  for (UMaterialInterface *Cursor = Material; Cursor;) {
    TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
    Cursor->GetAllParametersOfType(EMaterialParameterType::Texture, Parameters);
    for (const FString &Candidate : Candidates) {
      for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>
               &Parameter : Parameters) {
        if (Parameter.Key.Name.ToString().Equals(Candidate,
                                                 ESearchCase::IgnoreCase)) {
          OutName = Parameter.Key.Name;
          return true;
        }
      }
    }
    UMaterialInstance *Instance = Cast<UMaterialInstance>(Cursor);
    Cursor = Instance ? Instance->Parent : nullptr;
  }
  return false;
}

static UMaterialInstanceConstant *
CreatePlantMaterialInstance(const FString &AssetFolder,
                            const FString &AssetName, UTexture *DiffuseTexture,
                            UTexture *NormalTexture, UTexture *ORMTexture,
                            UTexture *OpacityMaskTexture,
                            UTexture *SubsurfaceTexture,
                            const FString &MaterialRole, bool bUseVT,
                            bool bOpaque = false) {
  const bool bBillboard = MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase);
  const FString ParentPath =
      bOpaque ? UAssetHiveSettings::GetPlantOpaqueParentMaterialPath(bUseVT)
              : UAssetHiveSettings::GetPlantParentMaterialPath(bBillboard);
  UMaterialInterface *ParentMaterial =
      bOpaque ? UAssetHiveSettings::GetPlantOpaqueParentMaterial(bUseVT)
              : UAssetHiveSettings::GetPlantParentMaterial(bBillboard, bUseVT);
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing Plant %s parent material: %s"),
           bOpaque ? TEXT("Opaque")
                   : (bBillboard ? TEXT("Billboard") : TEXT("Atlas")),
           *ParentPath);
    return nullptr;
  }
  for (UTexture *Texture :
       {DiffuseTexture, NormalTexture, ORMTexture, OpacityMaskTexture,
        SubsurfaceTexture}) {
    ForceTextureDataReady(Texture);
  }

  const FString MaterialAssetName =
      UAssetHiveSettings::GetPlantMaterialName(AssetName, bBillboard, bOpaque);
  const FString MaterialPackagePath = AssetFolder / MaterialAssetName;
  UPackage *MaterialPackage = CreatePackage(*MaterialPackagePath);
  UMaterialInstanceConstant *MaterialInstance =
      FindObject<UMaterialInstanceConstant>(MaterialPackage,
                                            *MaterialAssetName);
  const bool bIsNew = MaterialInstance == nullptr;
  if (!MaterialInstance) {
    MaterialInstance = NewObject<UMaterialInstanceConstant>(
        MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
  }
  if (!MaterialInstance) {
    return nullptr;
  }
  MaterialInstance->SetParentEditorOnly(ParentMaterial);

  if (bOpaque) {
    // The transparency mask is already cut into the geometry, so the variant
    // renders as a true Opaque surface instead of Masked.
    // UMaterialInstance exposes BasePropertyOverrides as a public property;
    // UpdateMaterialInstance() below pushes it through UpdateStaticPermutation().
    FMaterialInstanceBasePropertyOverrides &Overrides =
        MaterialInstance->BasePropertyOverrides;
    Overrides.bOverride_BlendMode = true;
    Overrides.BlendMode = BLEND_Opaque;
  }

  // 植被材质参数（md §5.2）：Diffuse（albedo 代用）/ Normal / ORM /
  // OpacityMasked（masked 资产）/ SubsurfaceColor（SSC，Megascans Translucency），
  // 默认名对齐项目 GrassBend 母材质（M_Env_GrassBend_ST）。
  const FString DiffuseParameter = UAssetHiveSettings::GetPlantDiffuseParameter();
  const FString NormalParameter = UAssetHiveSettings::GetPlantNormalParameter();
  const FString ORMParameter = UAssetHiveSettings::GetPlantORMParameter();
  const FString OpacityMaskedParameter =
      UAssetHiveSettings::GetPlantOpacityMaskedParameter();
  FName SubsurfaceParameterName;
  const bool bHasSubsurfaceParameter = ResolvePlantSubsurfaceParameterName(
      ParentMaterial, UAssetHiveSettings::GetPlantSubsurfaceParameter(),
      SubsurfaceParameterName);
  if (DiffuseTexture && !DiffuseParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*DiffuseParameter)), DiffuseTexture);
  }
  if (NormalTexture && !NormalParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*NormalParameter)), NormalTexture);
  }
  if (ORMTexture && !ORMParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*ORMParameter)), ORMTexture);
  }
  const bool bUseOpacityMasked = !bOpaque && OpacityMaskTexture != nullptr;
  if (bUseOpacityMasked && !OpacityMaskedParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*OpacityMaskedParameter)),
        OpacityMaskTexture);
  }
  // SSC 槽位：Megascans 植被的 Translucency（T）贴图；Opaque 裁切变体同样需要。
  if (SubsurfaceTexture) {
    if (bHasSubsurfaceParameter) {
      MaterialInstance->SetTextureParameterValueEditorOnly(
          FMaterialParameterInfo(SubsurfaceParameterName), SubsurfaceTexture);
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive: %s has no SubsurfaceColor texture parameter ")
             TEXT("(configured '%s'); SSC texture not assigned."),
             *ParentPath,
             *UAssetHiveSettings::GetPlantSubsurfaceParameter());
    }
  }
  const FString OpacityMaskedSwitch =
      UAssetHiveSettings::GetPlantUseOpacityMaskedSwitch();
  if (!OpacityMaskedSwitch.IsEmpty()) {
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*OpacityMaskedSwitch)), bUseOpacityMasked);
  }

  // Publish the complete parameter set once, after texture compilation.
  UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  MaterialInstance->MarkPackageDirty();
  FinalizeImportedAsset(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  AssetHiveThumbnailRefresh::Queue(MaterialInstance);
  return MaterialInstance;
}

static UMaterialInstanceConstant *
CreateDecalMaterialInstance(const FString &MaterialFolder,
                            const FString &AssetStem,
                            UTexture *DiffuseTexture, UTexture *NormalTexture,
                            UTexture *OpacityTexture, UTexture *ORMTexture,
                            UTexture *DisplacementTexture,
                            const FString &DecalParentMode) {
  UMaterialInterface *ParentMaterial = LoadDecalMaterialParent(
      {DiffuseTexture, NormalTexture, OpacityTexture, ORMTexture,
       DisplacementTexture},
      DecalParentMode);
  if (!ParentMaterial) {
    return nullptr;
  }

  const FString MaterialAssetName =
      UAssetHiveSettings::GetDecalMaterialName(AssetStem);
  const FString MaterialPackagePath = MaterialFolder / MaterialAssetName;
  UPackage *MaterialPackage = CreatePackage(*MaterialPackagePath);
  UMaterialInstanceConstant *MaterialInstance =
      FindObject<UMaterialInstanceConstant>(MaterialPackage,
                                            *MaterialAssetName);
  const bool bIsNew = MaterialInstance == nullptr;
  if (!MaterialInstance) {
    MaterialInstance = NewObject<UMaterialInstanceConstant>(
        MaterialPackage, *MaterialAssetName, RF_Public | RF_Standalone);
  }
  if (!MaterialInstance) {
    return nullptr;
  }
  MaterialInstance->SetParentEditorOnly(ParentMaterial);

  const FString OpacitySwitch =
      UAssetHiveSettings::GetDecalUseOpacityTextureSwitch();
  if (!OpacitySwitch.IsEmpty()) {
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*OpacitySwitch)), true);
    UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  }

  const FString DiffuseParameter =
      UAssetHiveSettings::GetDecalDiffuseParameter();
  const FString NormalParameter =
      UAssetHiveSettings::GetDecalNormalParameter();
  const FString OpacityParameter =
      UAssetHiveSettings::GetDecalOpacityParameter();
  const FString ORMParameter =
      UAssetHiveSettings::GetDecalORMTextureParameter();
  const FString DisplacementParameter =
      UAssetHiveSettings::GetDecalDisplacementParameter();
  if (DiffuseTexture && !DiffuseParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*DiffuseParameter)), DiffuseTexture);
  }
  if (NormalTexture && !NormalParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*NormalParameter)), NormalTexture);
  }
  if (OpacityTexture && !OpacityParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*OpacityParameter)), OpacityTexture);
  }
  if (ORMTexture && !ORMParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*ORMParameter)), ORMTexture);
  }
  if (DisplacementTexture && !DisplacementParameter.IsEmpty()) {
    MaterialInstance->SetTextureParameterValueEditorOnly(
        FMaterialParameterInfo(FName(*DisplacementParameter)),
        DisplacementTexture);
  }
  UMaterialEditingLibrary::UpdateMaterialInstance(MaterialInstance);
  MaterialInstance->MarkPackageDirty();
  FinalizeImportedAsset(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  AssetHiveThumbnailRefresh::Queue(MaterialInstance);
  return MaterialInstance;
}

int32 UAssetHiveImportCommandlet::Main(const FString& Params) {
  // Explicit command-line jobs remain readable; editor imports never create/read job files.
  FString JobFilePath, JobContent;
  TSharedPtr<FJsonObject> Root;
  if (!FParse::Value(*Params, TEXT("Job="), JobFilePath) ||
      !FFileHelper::LoadFileToString(JobContent, *JobFilePath) ||
      !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JobContent), Root)) {
    UE_LOG(LogTemp, Error, TEXT("Missing or invalid -Job argument."));
    return 1;
  }
  return ImportJob(Root, UAssetHiveSettings::GetImportRootPath(), {});
}

int32 UAssetHiveImportCommandlet::ImportJob(const TSharedPtr<FJsonObject>& Root,
    const FString& DestinationPath, TFunction<void(int32, const FString&, bool)> OnProgress,
    TArray<FString>* OutImportedFolders) {
  TGuardValue<bool> ImportFailureGuard(GAssetHiveImportFailed, false);
  if (OutImportedFolders) OutImportedFolders->Reset();
  if (!Root.IsValid() || !UAssetHiveSettings::IsValidImportRootPath(DestinationPath)) {
    UE_LOG(LogTemp, Error, TEXT("Invalid import job or import root path: %s"), *DestinationPath);
    return 1;
  }
  const auto SetStageProgress = [&OnProgress](float Target, const FString& Stage, bool bShowInEditor = true) {
    const int32 Percent = FMath::Clamp(FMath::RoundToInt(Target), 0, 100);
    UE_LOG(LogTemp, Display, TEXT("[AssetHiveProgress]%d|%s"), Percent, *Stage);
    if (OnProgress) OnProgress(Percent, Stage, bShowInEditor);
  };
  SetStageProgress(2.0f, TEXT("读取导入任务"), false);
  bool bCreateFoliageDefault = false;
  Root->TryGetBoolField(TEXT("createFoliage"), bCreateFoliageDefault);

  const TArray<TSharedPtr<FJsonValue>> *AssetsJson = nullptr;
  if (!Root->TryGetArrayField(TEXT("assets"), AssetsJson) ||
      AssetsJson == nullptr || AssetsJson->IsEmpty()) {
    UE_LOG(LogTemp, Error, TEXT("No assets in job file."));
    return 1;
  }

  FAssetToolsModule &AssetToolsModule = FAssetToolsModule::GetModule();
  bool bNeedRestoreInterchange = false;
  bool bInterchangeOriginalValue = true;
#if (ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 4)
  if (IConsoleVariable *InterchangeEnable =
          IConsoleManager::Get().FindConsoleVariable(
              TEXT("Interchange.FeatureFlags.Import.Enable"))) {
    bInterchangeOriginalValue = InterchangeEnable->GetBool();
    InterchangeEnable->Set(false);
    bNeedRestoreInterchange = true;
    UE_LOG(LogTemp, Display,
           TEXT("AssetHive import: disable Interchange for FBX import"));
  }
#endif

  const int32 AssetCount = AssetsJson->Num();
  int32 AssetIndex = 0;
  for (const TSharedPtr<FJsonValue> &AssetValue : *AssetsJson) {
    if (!AssetValue.IsValid() || AssetValue->Type != EJson::Object) {
      continue;
    }
    const int32 AssetBaseProgress =
        10 + (AssetIndex * 80) / FMath::Max(1, AssetCount);
    const int32 AssetEndProgress =
        10 + ((AssetIndex + 1) * 80) / FMath::Max(1, AssetCount);
    SetStageProgress(
        static_cast<float>(AssetBaseProgress),
        FString::Printf(TEXT("处理资产 %d/%d"), AssetIndex + 1, AssetCount), false);

    TSharedPtr<FJsonObject> AssetObject = AssetValue->AsObject();
    FString AssetName = TEXT("AssetHiveAsset");
    FString AssetId = TEXT("");
    AssetObject->TryGetStringField(TEXT("name"), AssetName);
    AssetObject->TryGetStringField(TEXT("id"), AssetId);
    FString AssetType = TEXT("");
    AssetObject->TryGetStringField(TEXT("assetType"), AssetType);
    FString AssetSource = TEXT("");
    AssetObject->TryGetStringField(TEXT("source"), AssetSource);
    FString CategoryFolder = TEXT("Others");
    AssetObject->TryGetStringField(TEXT("categoryFolder"), CategoryFolder);
    FString AssetFolderName = TEXT("");
    AssetObject->TryGetStringField(TEXT("assetFolderName"), AssetFolderName);
    AssetType = AssetType.ToLower();
    AssetSource = AssetSource.ToLower();
    bool bCreateFoliageForAsset = false;
    const bool bHasCreateFoliageOverride = AssetObject->TryGetBoolField(
        TEXT("createFoliage"), bCreateFoliageForAsset);
    if (!bHasCreateFoliageOverride) {
      bCreateFoliageForAsset = bCreateFoliageDefault;
    }
    const bool bIsSurface = AssetType == TEXT("surface");
    const bool bIsDecal = AssetType == TEXT("decal");
    const bool bIsHdri = AssetType == TEXT("hdri");
    const bool bIsModelAsset =
        AssetType == TEXT("3d") || AssetType == TEXT("3dplant");
    const bool bIsCustomAsset = AssetSource == TEXT("custom");
    const bool bIs3DAsset = AssetType == TEXT("3d");
    if (AssetId.IsEmpty()) {
      AssetId = TEXT("UnknownId");
    }
    FEnvironmentAssetProfile EnvironmentProfile;
    FPlantAssetProfile PlantProfile;
    TArray<FString> AssetStandardTags;
    if (bIs3DAsset || AssetType == TEXT("3dplant")) {
      CollectAssetTags(AssetObject, AssetStandardTags);
    }
    if (bIs3DAsset) {
      EnvironmentProfile = ResolveEnvironmentAssetProfile(AssetStandardTags);
      CategoryFolder = EnvironmentProfile.FolderName;
    } else if (AssetType == TEXT("3dplant")) {
      // 3D Plants（md §5.2）：按标准 Asset Tag 落到 Vegetation/<子类>/。
      PlantProfile = ResolvePlantAssetProfile(AssetStandardTags);
      CategoryFolder = PlantProfile.SubtypeFolder;
    }
    // Third party scans follow the Dressing triangle budget even when their
    // tags route them into another asset folder.
    const bool bScanSourceAsset =
        bIs3DAsset &&
        (HasAssetTag(AssetStandardTags, TEXT("Megascans")) ||
         HasAssetTag(AssetStandardTags, TEXT("PBRMAX")) ||
         EnvironmentProfile.TypeKey == TEXT("Megascans") ||
         EnvironmentProfile.TypeKey == TEXT("PBRMAX"));
    FString AssetDestinationPath =
        bIs3DAsset ? TEXT("/Game/Environment/Asset") : DestinationPath;
    if (AssetType == TEXT("3dplant")) {
      // md §5.2：Content/Environment/Asset/Vegetation/<Tree|Bush|Grass|Micro|HeroFoliage>/
      AssetDestinationPath = TEXT("/Game/Environment/Asset/Vegetation");
    }
    FString SurfaceExportRootPath;
    if ((bIsSurface || bIsDecal) && AssetObject->TryGetStringField(TEXT("exportRootPath"),
                                                     SurfaceExportRootPath) &&
        UAssetHiveSettings::IsValidImportRootPath(SurfaceExportRootPath)) {
      AssetDestinationPath = SurfaceExportRootPath;
    }
    const FString SafeAssetName = MakeSafeObjectName(AssetName);
    const FString SafeAssetId = MakeSafeObjectName(AssetId);
    const FString SafeCategoryFolder = MakeSafeObjectName(CategoryFolder);
    FString SafeAssetFolderName = MakeSafeObjectName(
        AssetFolderName.IsEmpty() ? SafeAssetName : AssetFolderName);
    // Keep the on-disk asset folder aligned with every other exported object:
    // the internal asset ID is part of the folder name as well.
    if (!SafeAssetId.IsEmpty() &&
        !SafeAssetFolderName.EndsWith(
            FString::Printf(TEXT("_%s"), *SafeAssetId),
            ESearchCase::IgnoreCase)) {
      SafeAssetFolderName += TEXT("_") + SafeAssetId;
    }
    const FString AssetStem =
        FString::Printf(TEXT("%s_%s"), *SafeAssetName, *SafeAssetId);
    // 3D Plants（2026-09-30 定稿）：资产名不入名，改用 标准 Asset Tag + 资产ID；
    // 该命名模板只对原始资产为 FBX 的资产生效（st9 保持原有命名）。
    const bool bFbxPlantNaming =
        AssetType == TEXT("3dplant") && AssetHasFbxPlantModels(AssetObject);
    const FString PlantObjectStem =
        bFbxPlantNaming ? BuildPlantObjectStem(AssetStandardTags, SafeAssetId)
                        : FString();
    const FString EnvironmentStem =
        bIs3DAsset ? BuildEnvironmentObjectStem(EnvironmentProfile, AssetName,
                                                AssetId)
                   : FString();
    const FString AssetFolder =
        SafeCategoryFolder.IsEmpty()
            ? AssetDestinationPath / SafeAssetFolderName
            : AssetDestinationPath / SafeCategoryFolder / SafeAssetFolderName;
    const bool bUsesMaterialFolders = bIsSurface || bIsDecal;
    const FString TextureFolder = bUsesMaterialFolders ? AssetFolder / TEXT("Tex") : AssetFolder;
    const FString MaterialFolder = bUsesMaterialFolders ? AssetFolder / TEXT("MI") : AssetFolder;
    const bool bUseVT = IsAssetVirtualTextureImportEnabled();
    double SurfaceBaseTiling = 1.0;
    FString DecalParentMode = TEXT("decal");
    const TSharedPtr<FJsonObject> *MaterialParams = nullptr;
    if (AssetObject->TryGetObjectField(TEXT("materialParams"), MaterialParams) &&
        MaterialParams && MaterialParams->IsValid()) {
      double ParsedBaseTiling = 1.0;
      if ((*MaterialParams)->TryGetNumberField(TEXT("baseTiling"), ParsedBaseTiling) &&
          FMath::IsFinite(ParsedBaseTiling) && ParsedBaseTiling > 0.0) {
        SurfaceBaseTiling = ParsedBaseTiling;
      }
      FString ParsedDecalParentMode;
      if ((*MaterialParams)->TryGetStringField(TEXT("decalParentType"),
                                               ParsedDecalParentMode)) {
        DecalParentMode =
            UAssetHiveSettings::NormalizeDecalParentMode(ParsedDecalParentMode);
      }
    }

    TMap<int32, TMap<FString, FString>> SourceTextureSlotMapByGroup;
    TMap<int32, TMap<FString, FString>> SourceTextureNormalFormatMapByGroup;
    TMap<int32, FString> SourceTextureMaterialRoleByGroup;
    TMap<FString, int32> SourceTextureGroupByPath;
    TMap<FString, FString> SourceTextureObjectNameByPath;
    TMap<FString, FString> SourceTextureResolutionByPath;
    const TArray<TSharedPtr<FJsonValue>> *TextureSlots = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("textureSlots"), TextureSlots) &&
        TextureSlots != nullptr) {
      for (const TSharedPtr<FJsonValue> &SlotValue : *TextureSlots) {
        if (!SlotValue.IsValid() || SlotValue->Type != EJson::Object) {
          continue;
        }
        const TSharedPtr<FJsonObject> SlotObject = SlotValue->AsObject();
        if (!SlotObject.IsValid()) {
          continue;
        }
        FString SourceFile;
        FString SlotName;
        FString NormalMapFormat;
        FString ObjectName;
        FString Resolution;
        FString MaterialRole;
        int32 GroupId = 1;
        double GroupIdValue = 1.0;
        SlotObject->TryGetStringField(TEXT("file"), SourceFile);
        SlotObject->TryGetStringField(TEXT("slot"), SlotName);
        SlotObject->TryGetStringField(TEXT("objectName"), ObjectName);
        SlotObject->TryGetStringField(TEXT("resolution"), Resolution);
        SlotObject->TryGetStringField(TEXT("materialRole"), MaterialRole);
        if (SlotObject->TryGetNumberField(TEXT("groupId"), GroupIdValue)) {
          GroupId = FMath::Max(1, static_cast<int32>(GroupIdValue));
        }
        if (!SourceFile.IsEmpty() && !SlotName.IsEmpty()) {
          GroupId = FMath::Max(1, GroupId);
          const FString SourceKey = NormalizePathLower(SourceFile);
          FString NormalizedSlotName = SlotName.ToLower();
          if (bIsDecal && (NormalizedSlotName == TEXT("d") ||
                           NormalizedSlotName == TEXT("diffuse") ||
                           NormalizedSlotName == TEXT("albedo") ||
                           NormalizedSlotName == TEXT("basecolor"))) {
            NormalizedSlotName = TEXT("albedo");
          } else if (bIsDecal && (NormalizedSlotName == TEXT("n") ||
                                  NormalizedSlotName == TEXT("normal"))) {
            NormalizedSlotName = TEXT("normal");
          } else if (bIsDecal && (NormalizedSlotName == TEXT("o") ||
                                  NormalizedSlotName == TEXT("opacity") ||
                                  NormalizedSlotName == TEXT("opacitymasked") ||
                                  NormalizedSlotName == TEXT("opacitymask"))) {
            NormalizedSlotName = TEXT("opacity");
          } else if (bIsDecal && (NormalizedSlotName == TEXT("orm") ||
                                  NormalizedSlotName == TEXT("ormh"))) {
            NormalizedSlotName = TEXT("orm");
          }
          const bool bIsPlantAsset = AssetType == TEXT("3dplant");
          if (bIsPlantAsset &&
              (NormalizedSlotName == TEXT("al") ||
               NormalizedSlotName == TEXT("d") ||
               NormalizedSlotName == TEXT("bc") ||
               NormalizedSlotName == TEXT("diffuse") ||
               NormalizedSlotName == TEXT("basecolor") ||
               NormalizedSlotName == TEXT("albedo"))) {
            NormalizedSlotName = TEXT("albedo");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("n") ||
                      NormalizedSlotName == TEXT("normal") ||
                      NormalizedSlotName == TEXT("nrm"))) {
            NormalizedSlotName = TEXT("normal");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("orm") ||
                      NormalizedSlotName == TEXT("ormh"))) {
            NormalizedSlotName = TEXT("orm");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("o") ||
                      NormalizedSlotName == TEXT("opacity") ||
                      NormalizedSlotName == TEXT("opacitymask") ||
                      NormalizedSlotName == TEXT("opacitymasked") ||
                      NormalizedSlotName == TEXT("opacity_masked") ||
                      NormalizedSlotName == TEXT("alpha"))) {
            NormalizedSlotName = TEXT("opacity");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("ao") ||
                      NormalizedSlotName == TEXT("occlusion"))) {
            NormalizedSlotName = TEXT("ao");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("m") ||
                      NormalizedSlotName == TEXT("metalness") ||
                      NormalizedSlotName == TEXT("metallic"))) {
            NormalizedSlotName = TEXT("metalness");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("r") ||
                      NormalizedSlotName == TEXT("roughness"))) {
            NormalizedSlotName = TEXT("roughness");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("e") ||
                      NormalizedSlotName == TEXT("emissive"))) {
            NormalizedSlotName = TEXT("emissive");
          } else if (bIsPlantAsset &&
                     (NormalizedSlotName == TEXT("t") ||
                      NormalizedSlotName == TEXT("translucency") ||
                      NormalizedSlotName == TEXT("subsurface") ||
                      NormalizedSlotName == TEXT("ssc"))) {
            NormalizedSlotName = TEXT("translucency");
          } else if (bIsSurface && NormalizedSlotName == TEXT("bcr")) {
            NormalizedSlotName = TEXT("bcr");
          } else if (bIsSurface && (NormalizedSlotName == TEXT("n") || NormalizedSlotName == TEXT("normal"))) {
            NormalizedSlotName = TEXT("normal");
          } else if (bIsSurface && (NormalizedSlotName == TEXT("m") || NormalizedSlotName == TEXT("metalness") || NormalizedSlotName == TEXT("metallic"))) {
            NormalizedSlotName = TEXT("metalness");
          } else if (bIsSurface && (NormalizedSlotName == TEXT("e") || NormalizedSlotName == TEXT("emissive") || NormalizedSlotName == TEXT("emission"))) {
            NormalizedSlotName = TEXT("emissive");
          } else if (NormalizedSlotName == TEXT("orm") ||
                     NormalizedSlotName == TEXT("ormh")) {
            NormalizedSlotName = TEXT("orm");
          } else if (NormalizedSlotName == TEXT("m") ||
                     NormalizedSlotName == TEXT("ordp")) {
            NormalizedSlotName = TEXT("mask");
          } else if (NormalizedSlotName == TEXT("mask")) {
            NormalizedSlotName = TEXT("mask");
          }
          SourceTextureSlotMapByGroup.FindOrAdd(GroupId).Add(
              SourceKey, NormalizedSlotName);
          SourceTextureGroupByPath.Add(SourceKey, GroupId);
          if (!ObjectName.IsEmpty()) {
            SourceTextureObjectNameByPath.Add(SourceKey, ObjectName);
          }
          if (!Resolution.IsEmpty()) {
            SourceTextureResolutionByPath.Add(SourceKey, Resolution.ToUpper());
          }
          if (!MaterialRole.IsEmpty()) {
            SourceTextureMaterialRoleByGroup.Add(GroupId, MaterialRole.ToLower());
          }
          if (SlotObject->TryGetStringField(TEXT("normalMapFormat"),
                                            NormalMapFormat) &&
              !NormalMapFormat.IsEmpty()) {
            SourceTextureNormalFormatMapByGroup.FindOrAdd(GroupId).Add(
                SourceKey, NormalMapFormat.ToLower());
          }
        }
      }
    }

    const TArray<TSharedPtr<FJsonValue>> *MaterialGroups = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("materialGroups"), MaterialGroups) && MaterialGroups != nullptr) {
      for (const TSharedPtr<FJsonValue> &GroupValue : *MaterialGroups) {
        if (!GroupValue.IsValid() || GroupValue->Type != EJson::Object) continue;
        const TSharedPtr<FJsonObject> GroupObject = GroupValue->AsObject();
        if (!GroupObject.IsValid()) continue;
        FString MaterialRole;
        double GroupIdValue = 1.0;
        GroupObject->TryGetStringField(TEXT("materialRole"), MaterialRole);
        GroupObject->TryGetNumberField(TEXT("groupId"), GroupIdValue);
        if (!MaterialRole.IsEmpty()) {
          SourceTextureMaterialRoleByGroup.Add(FMath::Max(1, static_cast<int32>(GroupIdValue)), MaterialRole.ToLower());
        }
      }
    }

    TArray<UStaticMesh *> ImportedMeshes;
    TMap<UStaticMesh *, FString> SourceFileByMesh;
    // Model variant key per imported mesh; keys ending with OPAQUE mark cut
    // foliage variants that must use a true Opaque material.
    TMap<UStaticMesh *, FString> VariantKeyByMesh;
    // Variant keys the library export flagged as cut OPAQUE bases. Newer export
    // payloads keep the plain key (for example 01) and add an explicit flag.
    TSet<FString> OpaqueModelVariantKeys;
    TMap<FString, TArray<FPlantModelLodEntry>> PlantLodsByVariant;
    TMap<FString, bool> FbxSmoothingGroupCache;
    TMap<int32, TMap<FString, UTexture *>> TextureBySlotByGroup;
    TMap<int32, TMap<FString, FString>> SourceTextureBySlotByGroup;
    const bool bMultipleTextureGroups = SourceTextureSlotMapByGroup.Num() > 1;

    FString PrimaryModelVariantKey = TEXT("01");
    bool bPrimaryModelVariantResolved = false;
    const TArray<TSharedPtr<FJsonValue>> *ModelFiles = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("modelFiles"), ModelFiles) &&
        ModelFiles != nullptr) {
      TMap<FString, FString> ExplicitVariantByFile;
      bool bHasExplicitModelVariants = false;
      const TArray<TSharedPtr<FJsonValue>> *ModelVariants = nullptr;
      if (AssetObject->TryGetArrayField(TEXT("modelVariants"), ModelVariants) &&
          ModelVariants != nullptr) {
        for (const TSharedPtr<FJsonValue> &VariantValue : *ModelVariants) {
          if (!VariantValue.IsValid() || VariantValue->Type != EJson::Object) {
            continue;
          }
          const TSharedPtr<FJsonObject> VariantObject = VariantValue->AsObject();
          if (!VariantObject.IsValid()) {
            continue;
          }
          FString SourceFile;
          if (!VariantObject->TryGetStringField(TEXT("file"), SourceFile) &&
              !VariantObject->TryGetStringField(TEXT("path"), SourceFile) &&
              !VariantObject->TryGetStringField(TEXT("uri"), SourceFile)) {
            continue;
          }
          FString VariantKey;
          if (SourceFile.IsEmpty() ||
              !ReadModelVariantKey(VariantObject, VariantKey)) {
            continue;
          }
          bool bOpaqueVariant = false;
          VariantObject->TryGetBoolField(TEXT("opaque"), bOpaqueVariant);
          if (bOpaqueVariant || IsOpaqueModelVariantKey(VariantKey)) {
            OpaqueModelVariantKeys.Add(NormalizeModelVariantKey(VariantKey));
          }
          ExplicitVariantByFile.Add(NormalizeModelSourceKey(SourceFile), VariantKey);
          bHasExplicitModelVariants = true;
          if (!bPrimaryModelVariantResolved) {
            PrimaryModelVariantKey = NormalizeModelVariantKey(VariantKey);
            bPrimaryModelVariantResolved = true;
          }
        }
      }

      bool bHandledModelImport = false;
      if (AssetType == TEXT("3dplant") && bIsModelAsset) {
        struct FPlantModelEntry {
          FString SourceFile;
          FString VariantKey = TEXT("01");
          int32 LodIndex = 0;
        };

        TArray<FPlantModelEntry> PlantModels;
        for (const TSharedPtr<FJsonValue> &FileValue : *ModelFiles) {
          if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
            continue;
          }
          const FString SourceFile = FileValue->AsString();
          if (!FPaths::FileExists(SourceFile)) {
            UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"),
                   *SourceFile);
            continue;
          }
          if (FPaths::GetExtension(SourceFile).ToLower() == TEXT("abc")) {
            continue;
          }
          const FString VariantKey = ResolveModelVariantKey(
              SourceFile, ExplicitVariantByFile, PlantModels.Num() + 1);
          FString ParsedVariantKey = VariantKey;
          int32 LodIndex = 0;
          ExtractPlantVariantAndLod(SourceFile, ParsedVariantKey, LodIndex);
          FPlantModelEntry Entry;
          Entry.SourceFile = SourceFile;
          Entry.VariantKey = VariantKey;
          Entry.LodIndex = LodIndex;
          PlantModels.Add(Entry);
        }

        TMap<FString, TArray<FPlantModelEntry>> ByVariant;
        for (const FPlantModelEntry &Entry : PlantModels) {
          ByVariant.FindOrAdd(Entry.VariantKey).Add(Entry);
        }

        TArray<FString> VariantKeys;
        ByVariant.GetKeys(VariantKeys);
        VariantKeys.Sort([](const FString &A, const FString &B) {
          const bool bANumeric = A.IsNumeric();
          const bool bBNumeric = B.IsNumeric();
          if (bANumeric && bBNumeric) {
            return FCString::Atoi(*A) < FCString::Atoi(*B);
          }
          return A.Compare(B) < 0;
        });

        CollectPlantModelLodPlan(AssetObject, PlantLodsByVariant);

        for (const FString &VariantKey : VariantKeys) {
          TArray<FPlantModelEntry> &Entries = ByVariant.FindChecked(VariantKey);
          Entries.Sort(
              [](const FPlantModelEntry &A, const FPlantModelEntry &B) {
                if (A.LodIndex != B.LodIndex)
                  return A.LodIndex < B.LodIndex;
                return A.SourceFile < B.SourceFile;
              });

          int32 BaseIndex = INDEX_NONE;
          for (int32 Index = 0; Index < Entries.Num(); Index++) {
            if (Entries[Index].LodIndex == 0) {
              BaseIndex = Index;
              break;
            }
          }
          if (BaseIndex == INDEX_NONE) {
            BaseIndex = 0;
          }
          const FString BaseFile = Entries.IsValidIndex(BaseIndex)
                                       ? Entries[BaseIndex].SourceFile
                                       : FString();
          if (BaseFile.IsEmpty()) {
            continue;
          }

          const bool bFbxPlantAsset = !PlantObjectStem.IsEmpty();
          // The cut mesh is also recognised from its own file name
          // (SM_..._01OPAQUE.fbx) so a missing variant flag cannot turn an
          // opaque export back into a masked one.
          const bool bOpaqueMeshVariant =
              bFbxPlantAsset &&
              (IsOpaqueModelVariantKey(VariantKey) ||
               IsOpaqueModelVariantKey(FPaths::GetBaseFilename(BaseFile)) ||
               OpaqueModelVariantKeys.Contains(
                   NormalizeModelVariantKey(VariantKey)));
          const bool bNeedsVariantSuffix =
              bFbxPlantAsset || bHasExplicitModelVariants ||
              VariantKeys.Num() > 1;
          const FString VariantStem =
              bNeedsVariantSuffix
                  ? FString::Printf(
                        TEXT("%s_%s"),
                        bFbxPlantAsset ? *PlantObjectStem : *AssetStem,
                        *VariantKey)
                  : (bFbxPlantAsset ? PlantObjectStem : AssetStem);
          const FString BaseMeshName =
              bOpaqueMeshVariant
                  ? FString::Printf(TEXT("SM_%s_OPAQUE"), *VariantStem)
                  : FString::Printf(TEXT("SM_%s"), *VariantStem);

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 8, 0, 99)),
              FString::Printf(TEXT("导入植物模型: %s"),
                              *FPaths::GetCleanFilename(BaseFile)));
          UStaticMesh *BaseMesh = ImportStaticMeshAsset(
              AssetToolsModule, BaseFile, AssetFolder, BaseMeshName,
              PlantProfile.bNanite);
          if (!BaseMesh) {
            continue;
          }

          // LOD 版本导出的 Masked 植被：把该变体自己的 LOD 链合并成 custom LOD，
          // 合并后才配置材质，整个 LOD 栈只重建一次。OPAQUE 版本不带任何 LOD。
          if (const TArray<FPlantModelLodEntry> *VariantLods =
                  PlantLodsByVariant.Find(VariantKey)) {
            ImportPlantCustomLods(AssetToolsModule, BaseMesh, *VariantLods,
                                  AssetFolder, BaseMeshName);
          }

          // Nanite and materials are configured together after texture import.
          ImportedMeshes.Add(BaseMesh);
          VariantKeyByMesh.Add(BaseMesh, VariantKey);

          if (bCreateFoliageForAsset) {
            CreateFoliageTypeAsset(AssetFolder, VariantStem, BaseMesh);
          }
        }
        bHandledModelImport = ImportedMeshes.Num() > 0;
      }

      if (!bHandledModelImport) {
        int32 ValidModelCount = 0;
        for (const TSharedPtr<FJsonValue> &FileValue : *ModelFiles) {
          if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
            continue;
          }
          const FString SourceFile = FileValue->AsString();
          if (!FPaths::FileExists(SourceFile)) {
            continue;
          }
          if (FPaths::GetExtension(SourceFile).ToLower() == TEXT("abc")) {
            continue;
          }
          ValidModelCount += 1;
        }
        int32 ImportedModelIndex = 0;
        for (const TSharedPtr<FJsonValue> &FileValue : *ModelFiles) {
          if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
            continue;
          }
          const FString SourceFile = FileValue->AsString();
          if (!FPaths::FileExists(SourceFile)) {
            UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"),
                   *SourceFile);
            continue;
          }
          if (FPaths::GetExtension(SourceFile).ToLower() == TEXT("abc")) {
            continue;
          }
          UAssetImportTask *Task = NewObject<UAssetImportTask>();
          Task->Filename = SourceFile;
          Task->DestinationPath = AssetFolder;
          FString ModelAssetName;
          if (bIs3DAsset) {
            const FString VariantKey = ResolveModelVariantKey(
                SourceFile, ExplicitVariantByFile, ImportedModelIndex + 1);
            ModelAssetName = FString::Printf(TEXT("SM_%s_%s"),
                                             *EnvironmentStem, *VariantKey);
          } else if (bIsCustomAsset && bIsModelAsset) {
            const FString VariantKey = ResolveModelVariantKey(
                SourceFile, ExplicitVariantByFile, ImportedModelIndex + 1);
            const bool bFbxPlantAsset = !PlantObjectStem.IsEmpty();
            const bool bOpaqueMeshVariant =
                bFbxPlantAsset &&
                (IsOpaqueModelVariantKey(VariantKey) ||
                 IsOpaqueModelVariantKey(FPaths::GetBaseFilename(SourceFile)) ||
                 OpaqueModelVariantKeys.Contains(
                     NormalizeModelVariantKey(VariantKey)));
            const bool bNeedsVariantSuffix =
                bFbxPlantAsset || bHasExplicitModelVariants ||
                ValidModelCount > 1;
            ModelAssetName =
                bNeedsVariantSuffix
                    ? FString::Printf(
                          TEXT("SM_%s_%s"),
                          bFbxPlantAsset ? *PlantObjectStem : *AssetStem,
                          *VariantKey)
                    : FString::Printf(
                          TEXT("SM_%s"),
                          bFbxPlantAsset ? *PlantObjectStem : *AssetStem);
            if (bOpaqueMeshVariant) {
              ModelAssetName += TEXT("_OPAQUE");
            }
          } else {
            ModelAssetName = FString::Printf(TEXT("SM_%s_%s"), *AssetStem,
                                             *DetectModelSuffix(SourceFile));
          }
          Task->DestinationName = ModelAssetName;
          Task->bReplaceExisting = true;
          Task->bAutomated = true;
          Task->bAsync = false;
          Task->bSave = false;
          Task->Options = MakeStaticMeshImportOptions(
              AssetType == TEXT("3dplant") ? PlantProfile.bNanite : true);
          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 8, 0, 99)),
              FString::Printf(TEXT("导入模型: %s"),
                              *FPaths::GetCleanFilename(SourceFile)));
          AssetToolsModule.Get().ImportAssetTasks({Task});

          TArray<UObject *> ImportedObjects;
          AppendImportedObjects(Task, ImportedObjects);
          for (UObject *ImportedObject : ImportedObjects) {
            if (UStaticMesh *StaticMesh = Cast<UStaticMesh>(ImportedObject)) {
              ImportedMeshes.Add(StaticMesh);
              if (bIs3DAsset) {
                SourceFileByMesh.Add(StaticMesh, SourceFile);
                VariantKeyByMesh.Add(
                    StaticMesh,
                    ResolveModelVariantKey(SourceFile, ExplicitVariantByFile,
                                           ImportedModelIndex + 1));
              }
            }
          }
          ImportedModelIndex += 1;
        }
      }
    }

    if (bIs3DAsset && !bPrimaryModelVariantResolved && ModelFiles) {
      TMap<FString, FString> EmptyVariantMap;
      for (const TSharedPtr<FJsonValue> &FileValue : *ModelFiles) {
        if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
          continue;
        }
        const FString SourceFile = FileValue->AsString();
        PrimaryModelVariantKey =
            ResolveModelVariantKey(SourceFile, EmptyVariantMap, 1);
        bPrimaryModelVariantResolved = true;
        break;
      }
    }
    // OPAQUE foliage exports must not carry billboard textures or
    // MI_Billboard_* instances. New desktop payloads set billboardExcluded
    // explicitly, so a masked fallback variant cannot re-enable billboard.
    // Keep the legacy all-opaque heuristic for older payloads.
    bool bBillboardExcluded = false;
    AssetObject->TryGetBoolField(TEXT("billboardExcluded"), bBillboardExcluded);
    bool bAllModelVariantsOpaque = false;
    if (AssetType == TEXT("3dplant") && VariantKeyByMesh.Num() > 0) {
      bAllModelVariantsOpaque = true;
      for (const TPair<UStaticMesh *, FString> &VariantPair : VariantKeyByMesh) {
        const FString NormalizedVariantKey =
            NormalizeModelVariantKey(VariantPair.Value);
        if (!IsOpaqueModelVariantKey(VariantPair.Value) &&
            !OpaqueModelVariantKeys.Contains(NormalizedVariantKey)) {
          bAllModelVariantsOpaque = false;
          break;
        }
      }
    }
    const bool bSuppressBillboardExport =
        bBillboardExcluded || bAllModelVariantsOpaque;
    const TArray<TSharedPtr<FJsonValue>> *TextureFiles = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("textureFiles"), TextureFiles) &&
        TextureFiles != nullptr) {
      for (const TSharedPtr<FJsonValue> &FileValue : *TextureFiles) {
        if (!FileValue.IsValid() || FileValue->Type != EJson::String) {
          continue;
        }
        const FString SourceFile = FileValue->AsString();
        if (!FPaths::FileExists(SourceFile)) {
          UE_LOG(LogTemp, Warning, TEXT("Source file missing: %s"),
                 *SourceFile);
          continue;
        }
        const FString SourceKey = NormalizePathLower(SourceFile);
        const int32 GroupId =
            SourceTextureGroupByPath.Contains(SourceKey)
                ? FMath::Max(1, SourceTextureGroupByPath[SourceKey])
                : 1;
        if (bSuppressBillboardExport) {
          const FString *GroupMaterialRole =
              SourceTextureMaterialRoleByGroup.Find(GroupId);
          if (GroupMaterialRole &&
              GroupMaterialRole->Equals(TEXT("billboard"),
                                        ESearchCase::IgnoreCase)) {
            continue;
          }
        }
        const TMap<FString, FString> &GroupSlotMap =
            SourceTextureSlotMapByGroup.FindOrAdd(GroupId);
        const FString SlotName = GroupSlotMap.Contains(SourceKey)
                                     ? GroupSlotMap[SourceKey]
                                     : DetectTextureSlot(SourceFile);
        const bool bNormalSlot = SlotName == TEXT("normal");
        const bool bFlipGreenForOpenGL =
            bNormalSlot &&
            SourceTextureNormalFormatMapByGroup.FindOrAdd(GroupId).Contains(
                SourceKey) &&
            SourceTextureNormalFormatMapByGroup.FindOrAdd(GroupId)[SourceKey] ==
                TEXT("opengl");
        if (!SlotName.IsEmpty() &&
            !SourceTextureBySlotByGroup.FindOrAdd(GroupId).Contains(SlotName)) {
          SourceTextureBySlotByGroup.FindOrAdd(GroupId).Add(SlotName,
                                                            SourceFile);
        }
        const bool bAllowDisplacementSlot = AssetType == TEXT("surface") ||
                                            AssetType == TEXT("3d") ||
                                            AssetType == TEXT("3dplant");
        const bool bIsPlant = AssetType == TEXT("3dplant");
        const bool bAllowAsset3DSlots =
            bIs3DAsset &&
            (SlotName == TEXT("albedo") || SlotName == TEXT("normal") ||
             SlotName == TEXT("orm") || SlotName == TEXT("mask") ||
             SlotName == TEXT("opacity") || SlotName == TEXT("emissive") ||
             SlotName == TEXT("metalness") || SlotName == TEXT("roughness") ||
             SlotName == TEXT("ao") || SlotName == TEXT("displacement") ||
             SlotName == TEXT("fuzz") || SlotName == TEXT("subsurfacecolor") ||
             SlotName == TEXT("translucency"));
        const bool bAllowPlantSlots =
            bIsPlant &&
            (SlotName == TEXT("albedo") || SlotName == TEXT("normal") ||
             SlotName == TEXT("orm") || SlotName == TEXT("opacity") ||
             SlotName == TEXT("ao") || SlotName == TEXT("metalness") ||
             SlotName == TEXT("emissive") || SlotName == TEXT("fuzz") ||
             SlotName == TEXT("mask") || SlotName == TEXT("roughness") ||
             SlotName == TEXT("subsurfacecolor") ||
             ((SlotName == TEXT("displacement") ||
               SlotName == TEXT("translucency")) &&
              bAllowDisplacementSlot));
        const bool bAllow3DSlots = bAllowAsset3DSlots || bAllowPlantSlots;
        const bool bAllowDecalSlots =
            SlotName == TEXT("albedo") || SlotName == TEXT("normal") ||
            SlotName == TEXT("opacity") || SlotName == TEXT("orm") ||
            SlotName == TEXT("displacement");
        const bool bAllowHdriSlots = SlotName == TEXT("hdr");
        const bool bAllowSurfaceSlots =
            bIsSurface && (SlotName == TEXT("bcr") || SlotName == TEXT("normal") ||
                           SlotName == TEXT("metalness") ||
                           SlotName == TEXT("emissive"));
        if ((bIsSurface && !bAllowSurfaceSlots) ||
            (!bIsSurface &&
             ((bIsHdri && !bAllowHdriSlots) ||
              (bIsDecal && !bAllowDecalSlots) ||
              (!bIsHdri && !bIsDecal && !bAllow3DSlots)))) {
          continue;
        }
        const bool bPlantTextureAssetOnly =
            bIsPlant && SlotName != TEXT("albedo");
        // 3dplant albedo is rebuilt post-loop to pack opacity into the alpha
        // channel.
        const bool bPlantAlbedoDeferred =
            bIsPlant && SlotName == TEXT("albedo");
        if (bPlantTextureAssetOnly || bPlantAlbedoDeferred) {
          continue;
        }
        SetStageProgress(
            static_cast<float>(FMath::Clamp(AssetBaseProgress + 16, 0, 99)),
            FString::Printf(TEXT("导入贴图: %s"),
                            *FPaths::GetCleanFilename(SourceFile)));
        UAssetImportTask *Task = NewObject<UAssetImportTask>();
        Task->Filename = SourceFile;
        Task->DestinationPath = TextureFolder;
        FString ConfiguredObjectName;
        if ((bIsSurface || bIsDecal) && SourceTextureObjectNameByPath.Contains(SourceKey)) {
          ConfiguredObjectName = MakeSafeObjectName(SourceTextureObjectNameByPath[SourceKey]);
        }
        FString ImportedTextureName = ConfiguredObjectName;
        if (ImportedTextureName.IsEmpty()) {
          if (bIs3DAsset) {
            const FString GroupSegment =
                bMultipleTextureGroups
                    ? FString::Printf(TEXT("_G%03d"), GroupId)
                    : FString();
            ImportedTextureName = FString::Printf(
                TEXT("T_%s_%s%s_%s"), *EnvironmentStem,
                *PrimaryModelVariantKey, *GroupSegment,
                *To3DTextureSlotSuffix(SlotName));
          } else {
            ImportedTextureName =
                bMultipleTextureGroups
                    ? FString::Printf(TEXT("T_%s_%03d_%s"), *AssetStem,
                                      GroupId, *ToSlotSuffix(SlotName))
                    : FString::Printf(TEXT("T_%s_%s"), *AssetStem,
                                      *ToSlotSuffix(SlotName));
          }
        }
        Task->DestinationName = ImportedTextureName;
        Task->bReplaceExisting = true;
        Task->bAutomated = true;
        Task->bAsync = false;
        Task->bSave = false;

        if (SlotName == TEXT("displacement")) {
          UTextureFactory *Factory = NewObject<UTextureFactory>();
          Factory->CompressionSettings = TC_Masks;
          Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
          Task->Factory = Factory;
        } else if (bIsSurface || bIsDecal) {
          UTextureFactory *Factory = NewObject<UTextureFactory>();
          if (SlotName == TEXT("normal")) {
            Factory->CompressionSettings = TC_Normalmap;
            Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
          } else if (bIsSurface && SlotName == TEXT("metalness")) {
            Factory->CompressionSettings = TC_Masks;
            Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
          } else if (bIsDecal && (SlotName == TEXT("opacity") || SlotName == TEXT("orm"))) {
            Factory->CompressionSettings = TC_Masks;
            Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
          } else {
            Factory->CompressionSettings = TC_Default;
          }
          Task->Factory = Factory;
        } else if (SlotName == TEXT("mask")) {
          UTextureFactory *Factory = NewObject<UTextureFactory>();
          Factory->CompressionSettings = TC_Masks;
          Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
          Task->Factory = Factory;
        }

        AssetToolsModule.Get().ImportAssetTasks({Task});

        TArray<UObject *> ImportedObjects;
        AppendImportedObjects(Task, ImportedObjects);
        UE_LOG(LogTemp, Log, TEXT("AssetHive: imported %d texture object(s), %d paths"), ImportedObjects.Num(), Task->ImportedObjectPaths.Num());
        for (UObject *ImportedObject : ImportedObjects) {
          if (UTexture *Texture = Cast<UTexture>(ImportedObject)) {
            // Finish the import build before changing VT/compression settings.
            // Otherwise the in-flight DerivedData can be cancelled while the
            // texture's VT flag already has the new value, tripping UE's
            // (VTData != nullptr) == VirtualTextureStreaming assertion.
            ForceTextureDataReady(Texture);
            Texture->PreEditChange(nullptr);
            if (bIs3DAsset) {
              Apply3DAssetTexturePreset(Texture, SlotName, EnvironmentProfile,
                                        bUseVT);
            } else if (bIsSurface || bIsDecal) {
              const FString Resolution = SourceTextureResolutionByPath.Contains(SourceKey)
                                             ? SourceTextureResolutionByPath[SourceKey]
                                             : TEXT("");
              const bool bDisplacement = SlotName == TEXT("displacement");
              Texture->VirtualTextureStreaming = bUseVT && !bDisplacement;
              if (bDisplacement) {
                Texture->CompressionSettings = TC_Masks;
                Texture->SRGB = false;
                Texture->CompressionNoAlpha = true;
                Texture->MipGenSettings = TMGS_FromTextureGroup;
                Texture->LODGroup = TEXTUREGROUP_World;
                Texture->LossyCompressionAmount = TLCA_Medium;
                Texture->MaxTextureSize =
                    bIsDecal ? 1024 : (Resolution == TEXT("2K") ? 2048 : 4096);
              } else if (SlotName == TEXT("bcr") || (bIsDecal && SlotName == TEXT("albedo"))) {
                Texture->CompressionSettings = TC_Default;
                Texture->SRGB = true;
                Texture->CompressionNoAlpha = false;
                // Surface 纹理不自动锐化：Mip 生成设置交给 Texture Group。
                Texture->MipGenSettings = TMGS_FromTextureGroup;
                Texture->LODGroup = TEXTUREGROUP_World;
                Texture->LossyCompressionAmount = TLCA_Low;
                Texture->MaxTextureSize = Resolution == TEXT("2K") ? 2048 : 4096;
              } else if (SlotName == TEXT("normal")) {
                Texture->CompressionSettings = TC_Normalmap;
                Texture->SRGB = false;
                Texture->CompressionNoAlpha = true;
                Texture->MipGenSettings = TMGS_FromTextureGroup;
                Texture->LODGroup = TEXTUREGROUP_WorldNormalMap;
                Texture->LossyCompressionAmount = TLCA_Low;
                Texture->MaxTextureSize = Resolution == TEXT("2K") ? 2048 : 4096;
              } else if (bIsDecal && (SlotName == TEXT("opacity") || SlotName == TEXT("orm"))) {
                Texture->CompressionSettings = TC_Masks;
                Texture->SRGB = false;
                Texture->CompressionNoAlpha = true;
                Texture->MipGenSettings = TMGS_FromTextureGroup;
                Texture->LODGroup = TEXTUREGROUP_World;
                Texture->LossyCompressionAmount = TLCA_Medium;
                Texture->MaxTextureSize = 2048;
              } else if (bIsSurface && SlotName == TEXT("metalness")) {
                Texture->CompressionSettings = TC_Masks;
                Texture->SRGB = false;
                Texture->CompressionNoAlpha = true;
                Texture->LODGroup = TEXTUREGROUP_World;
                Texture->LossyCompressionAmount = TLCA_Medium;
                Texture->MaxTextureSize = 2048;
              } else if (bIsSurface && SlotName == TEXT("emissive")) {
                Texture->CompressionSettings = TC_Default;
                Texture->SRGB = true;
                Texture->CompressionNoAlpha = true;
                Texture->LODGroup = TEXTUREGROUP_World;
                Texture->LossyCompressionAmount = TLCA_Medium;
                Texture->MaxTextureSize = 2048;
              }
            } else if (SlotName == TEXT("albedo")) {
              Texture->CompressionSettings = TC_Default;
              Texture->SRGB = true;
              Texture->MipGenSettings = TMGS_Sharpen7;
            } else if (SlotName == TEXT("normal")) {
              Texture->CompressionSettings = TC_Normalmap;
              Texture->SRGB = false;
              Texture->MipGenSettings = TMGS_Sharpen4;
            } else if (SlotName == TEXT("roughness") ||
                       SlotName == TEXT("subsurfacecolor")) {
              Texture->CompressionSettings = TC_Masks;
              Texture->SRGB = false;
            } else if (SlotName == TEXT("fuzz") || SlotName == TEXT("mask")) {
              Texture->CompressionSettings = TC_Masks;
              Texture->SRGB = false;
              Texture->CompressionNoAlpha = true;
            }
            if (bNormalSlot) {
              Texture->bFlipGreenChannel = bFlipGreenForOpenGL;
            }
            Texture->PostEditChange();
            Texture->MarkPackageDirty();
            // Complete texture resources without serializing the package.
            ForceTextureDataReady(Texture);
            FinalizeImportedAsset(Texture);
            FAssetRegistryModule::AssetCreated(Texture);
            if (!SlotName.IsEmpty() &&
                !TextureBySlotByGroup.FindOrAdd(GroupId).Contains(SlotName)) {
              TextureBySlotByGroup.FindOrAdd(GroupId).Add(SlotName, Texture);
            }
          }
        }
      }
    }

    TSet<int32> GroupIdSet;
    {
      TArray<int32> TextureGroupIds;
      TextureBySlotByGroup.GetKeys(TextureGroupIds);
      for (const int32 Value : TextureGroupIds) {
        GroupIdSet.Add(Value);
      }
      TArray<int32> SourceGroupIds;
      SourceTextureBySlotByGroup.GetKeys(SourceGroupIds);
      for (const int32 Value : SourceGroupIds) {
        GroupIdSet.Add(Value);
      }
    }
    TArray<int32> GroupIds = GroupIdSet.Array();
    if (GroupIds.Num() == 0) {
      GroupIds.Add(1);
    }
    // md §5.2（2026-09-30）：3D Plants 的材质槽顺序对齐 FBX 材质顺序——Atlas
    // （非 billboard）组排在 billboard 组之前。Megascans 的 Billboard 贴图条目
    // 可能先于 Atlas 入库并拿到更小的 groupId，若直接按 groupId 排序，网格首个
    // 材质槽会被 Billboard 的 MI_Billboard_* 材质占用。其它 Asset Type 没有
    // billboard 角色，排序结果与原 GroupIds.Sort() 一致。
    GroupIds.Sort(
        [&SourceTextureMaterialRoleByGroup](const int32 A, const int32 B) {
          const bool bABillboard =
              SourceTextureMaterialRoleByGroup.FindRef(A).Equals(
                  TEXT("billboard"), ESearchCase::IgnoreCase);
          const bool bBBillboard =
              SourceTextureMaterialRoleByGroup.FindRef(B).Equals(
                  TEXT("billboard"), ESearchCase::IgnoreCase);
          if (bABillboard != bBBillboard) {
            return !bABillboard;
          }
          return A < B;
        });
    TArray<UMaterialInstanceConstant *> MaterialInstances;
    TArray<UMaterialInstanceConstant *> OpaqueMaterialInstances;
    // 与上面两个材质列表一一对应的材质组 ID：槽位指派按材质角色判定，不再用列表
    // 下标反推组 ID（Megascans 的 billboard 组会被 OPAQUE 变体整体剔除）。
    TArray<int32> MaterialGroupIds;
    TArray<int32> OpaqueMaterialGroupIds;
    bool bHasOpaqueModelVariant = !OpaqueModelVariantKeys.IsEmpty();
    for (const TPair<UStaticMesh *, FString> &VariantPair : VariantKeyByMesh) {
      if (bHasOpaqueModelVariant) {
        break;
      }
      if (IsOpaqueModelVariantKey(VariantPair.Value)) {
        bHasOpaqueModelVariant = true;
      }
    }
    for (const int32 GroupId : GroupIds) {
      const FString MaterialRole = SourceTextureMaterialRoleByGroup.FindRef(GroupId).ToLower();
      if (bSuppressBillboardExport &&
          MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase)) {
        continue;
      }
      const TMap<FString, UTexture *> &TextureBySlot =
          TextureBySlotByGroup.FindOrAdd(GroupId);
      const TMap<FString, FString> &SourceTextureBySlot =
          SourceTextureBySlotByGroup.FindOrAdd(GroupId);
      const FString GroupStem =
          GroupIds.Num() > 1
              ? FString::Printf(TEXT("%s_%03d"), *AssetStem, GroupId)
              : AssetStem;
      // 3D Plants 命名（2026-09-30 定稿，仅 FBX 原始资产）：纹理不带变体号、
      // 不带分辨率；材质实例带主变体号；多变体组沿用 _00N 组号段。
      const bool bFbxPlantAsset = !PlantObjectStem.IsEmpty();
      const FString PlantTextureStem =
          bFbxPlantAsset
              ? (GroupIds.Num() > 1
                     ? FString::Printf(TEXT("%s_%03d"), *PlantObjectStem,
                                       GroupId)
                     : PlantObjectStem)
              : GroupStem;
      const FString PlantMaterialStem =
          bFbxPlantAsset
              ? (GroupIds.Num() > 1
                     ? FString::Printf(TEXT("%s_%03d_%s"), *PlantObjectStem,
                                       GroupId, *PrimaryModelVariantKey)
                     : FString::Printf(TEXT("%s_%s"), *PlantObjectStem,
                                       *PrimaryModelVariantKey))
              : GroupStem;
      UMaterialInstanceConstant *MaterialInstance = nullptr;
      UMaterialInstanceConstant *OpaqueMaterialInstance = nullptr;
      if (bIsHdri) {
        continue;
      } else if (bIsDecal) {
        SetStageProgress(
            static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
            FString::Printf(TEXT("创建 Decal 材质实例: %s"), *AssetName), false);
        MaterialInstance = CreateDecalMaterialInstance(
            MaterialFolder, AssetStem,
            TextureBySlot.FindRef(TEXT("albedo")),
            TextureBySlot.FindRef(TEXT("normal")),
            TextureBySlot.FindRef(TEXT("opacity")),
            TextureBySlot.FindRef(TEXT("orm")),
            TextureBySlot.FindRef(TEXT("displacement")),
            DecalParentMode);
      } else if (bIsSurface) {
        MaterialInstance = CreateSurfaceMaterialInstance(
            MaterialFolder, AssetStem, GroupId,
            TextureBySlot.FindRef(TEXT("bcr")),
            TextureBySlot.FindRef(TEXT("normal")),
            TextureBySlot.FindRef(TEXT("metalness")),
            TextureBySlot.FindRef(TEXT("emissive")), SurfaceBaseTiling);
      } else if (bIs3DAsset) {
        SetStageProgress(
            static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
            FString::Printf(TEXT("Composite Textures: %s"), *AssetName), false);

        auto Resolve3DSourceTexture = [&](const TCHAR *SlotName) -> UTexture2D * {
          const FString SlotKey(SlotName);
          UTexture2D *ImportedTexture =
              Cast<UTexture2D>(TextureBySlot.FindRef(SlotKey));
          if (ImportedTexture) {
            return ImportedTexture;
          }
          if (SourceTextureBySlot.Contains(SlotKey)) {
            return FImageUtils::ImportFileAsTexture2D(
                SourceTextureBySlot[SlotKey]);
          }
          return nullptr;
        };

        UTexture2D *ORMTexture =
            Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("orm")));
        if (!ORMTexture) {
          UTexture2D *AOTexture = Resolve3DSourceTexture(TEXT("ao"));
          UTexture2D *RoughnessTexture =
              Resolve3DSourceTexture(TEXT("roughness"));
          UTexture2D *MetallicTexture =
              Resolve3DSourceTexture(TEXT("metalness"));
          if (AOTexture || RoughnessTexture || MetallicTexture) {
            const FString ORMGroup =
                GroupIds.Num() > 1
                    ? FString::Printf(TEXT("_G%03d"), GroupId)
                    : FString();
            const FString ORMAssetName = FString::Printf(
                TEXT("T_%s_%s%s_ORM"), *EnvironmentStem,
                *PrimaryModelVariantKey, *ORMGroup);
            ORMTexture = CreatePackedORMTexture(
                AssetFolder, ORMAssetName, AOTexture, RoughnessTexture,
                MetallicTexture,
                Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("albedo"))),
                Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("normal"))));
          }
        }
        if (ORMTexture) {
          Apply3DAssetTexturePreset(ORMTexture, TEXT("orm"),
                                    EnvironmentProfile, bUseVT);
        }

        UTexture2D *MegaMaskTexture = Resolve3DSourceTexture(TEXT("mask"));
        UTexture2D *OpacityTexture = Resolve3DSourceTexture(TEXT("opacity"));
        const bool bMasked = OpacityTexture != nullptr;
        const FString MaterialName = BuildEnvironmentAssetMaterialName(
            EnvironmentStem, GroupId, GroupIds.Num() > 1,
            PrimaryModelVariantKey);
        MaterialInstance = CreateEnvironmentAssetMaterialInstance(
            AssetFolder, MaterialName, EnvironmentProfile,
            TextureBySlot.FindRef(TEXT("albedo")),
            TextureBySlot.FindRef(TEXT("normal")), ORMTexture, MegaMaskTexture,
            OpacityTexture, TextureBySlot.FindRef(TEXT("emissive")), bMasked);
      } else {
        if (AssetType == TEXT("3dplant")) {
          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 22, 0, 99)),
              FString::Printf(TEXT("合成 Albedo+Opacity 贴图: %s"),
                              *AssetName), false);
          auto ImportPlantSource = [&SourceTextureBySlot](
                                       const TCHAR *SlotKey) -> UTexture2D * {
            return SourceTextureBySlot.Contains(SlotKey)
                       ? FImageUtils::ImportFileAsTexture2D(
                             SourceTextureBySlot[SlotKey])
                       : nullptr;
          };
          const FString TextureStem = PlantTextureStem;

          // md §5.2：植被贴图按 Diffuse（albedo 代用，D）/ Normal（N）/
          // ORM / OpacityMasked（O，masked 资产）分张导出，类型用缩写。
          UTexture2D *DiffuseTexture = CreatePlantTextureAsset(
              AssetFolder, FString::Printf(TEXT("T_%s_D"), *TextureStem),
              ImportPlantSource(TEXT("albedo")), EPlantTextureKind::Diffuse,
              PlantProfile, bUseVT);

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 24, 0, 99)),
              FString::Printf(TEXT("导出 Normal 贴图: %s"), *AssetName), false);
          UTexture2D *NormalTexture = CreatePlantTextureAsset(
              AssetFolder, FString::Printf(TEXT("T_%s_N"), *TextureStem),
              ImportPlantSource(TEXT("normal")), EPlantTextureKind::Normal,
              PlantProfile, bUseVT);

          UTexture2D *ORMTexture = nullptr;
          if (SourceTextureBySlot.Contains(TEXT("orm"))) {
            ORMTexture = CreatePlantTextureAsset(
                AssetFolder, FString::Printf(TEXT("T_%s_ORM"), *TextureStem),
                ImportPlantSource(TEXT("orm")), EPlantTextureKind::ORM,
                PlantProfile, bUseVT);
          } else {
            UTexture2D *AOSource = ImportPlantSource(TEXT("ao"));
            UTexture2D *RoughnessSource = ImportPlantSource(TEXT("roughness"));
            UTexture2D *MetallicSource = ImportPlantSource(TEXT("metalness"));
            if (AOSource || RoughnessSource || MetallicSource) {
              ORMTexture = CreatePackedORMTexture(
                  AssetFolder, FString::Printf(TEXT("T_%s_ORM"), *TextureStem),
                  AOSource, RoughnessSource, MetallicSource, DiffuseTexture,
                  NormalTexture);
              if (ORMTexture) {
                ORMTexture->PreEditChange(nullptr);
                ApplyPlantTexturePreset(ORMTexture, EPlantTextureKind::ORM,
                                        PlantProfile, bUseVT);
                ORMTexture->PostEditChange();
                ORMTexture->MarkPackageDirty();
                ForceTextureDataReady(ORMTexture);
                FinalizeImportedAsset(ORMTexture);
              }
            }
          }

          UTexture2D *OpacityMaskTexture = nullptr;
          if (SourceTextureBySlot.Contains(TEXT("opacity"))) {
            OpacityMaskTexture = CreatePlantTextureAsset(
                AssetFolder, FString::Printf(TEXT("T_%s_O"), *TextureStem),
                ImportPlantSource(TEXT("opacity")),
                EPlantTextureKind::OpacityMasked, PlantProfile, bUseVT);
          }

          // SSC（SubsurfaceColor）：Megascans 植被的 Translucency（T）贴图，
          // 导出 Job 用 subsurfacecolor 槽位传递，旧 Job 保留 translucency 兜底。
          FString SubsurfaceSourcePath =
              SourceTextureBySlot.FindRef(TEXT("subsurfacecolor"));
          if (SubsurfaceSourcePath.IsEmpty()) {
            SubsurfaceSourcePath =
                SourceTextureBySlot.FindRef(TEXT("translucency"));
          }
          UTexture2D *SubsurfaceTexture = nullptr;
          if (!SubsurfaceSourcePath.IsEmpty()) {
            SubsurfaceTexture = CreatePlantTextureAsset(
                AssetFolder, FString::Printf(TEXT("T_%s_SSC"), *TextureStem),
                FImageUtils::ImportFileAsTexture2D(SubsurfaceSourcePath),
                EPlantTextureKind::SubsurfaceColor, PlantProfile, bUseVT);
          }

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 26, 0, 99)),
              FString::Printf(TEXT("创建植被材质实例: %s"), *AssetName), false);
          MaterialInstance = CreatePlantMaterialInstance(
              AssetFolder, PlantMaterialStem, DiffuseTexture, NormalTexture,
              ORMTexture, OpacityMaskTexture, SubsurfaceTexture, MaterialRole,
              bUseVT);
          // md §5.2：OPAQUE 裁切变体不创建 billboard 材质实例。
          if (bHasOpaqueModelVariant &&
              !MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase)) {
            OpaqueMaterialInstance = CreatePlantMaterialInstance(
                AssetFolder, PlantMaterialStem, DiffuseTexture, NormalTexture,
                ORMTexture, nullptr, SubsurfaceTexture, MaterialRole, bUseVT,
                /*bOpaque=*/true);
          }
        } else {
          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
              FString::Printf(TEXT("Composite Textures: %s"), *AssetName), false);
          UTexture2D *MaskTexture =
              Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("mask")));
          if (MaskTexture) {
            // The import loop already applies these mask settings. Do not
            // restart its build immediately before creating the material.
            FinalizeImportedAsset(MaskTexture);
          } else {
            UTexture2D *AOSourceTexture =
                SourceTextureBySlot.Contains(TEXT("ao"))
                    ? FImageUtils::ImportFileAsTexture2D(
                          SourceTextureBySlot[TEXT("ao")])
                    : nullptr;
            UTexture2D *RoughnessSourceTexture =
                SourceTextureBySlot.Contains(TEXT("roughness"))
                    ? FImageUtils::ImportFileAsTexture2D(
                          SourceTextureBySlot[TEXT("roughness")])
                    : nullptr;
            UTexture2D *DisplacementSourceTexture =
                SourceTextureBySlot.Contains(TEXT("displacement"))
                    ? FImageUtils::ImportFileAsTexture2D(
                          SourceTextureBySlot[TEXT("displacement")])
                    : nullptr;
            UTexture2D *MaskSourceTexture =
                SourceTextureBySlot.Contains(TEXT("mask"))
                    ? FImageUtils::ImportFileAsTexture2D(
                          SourceTextureBySlot[TEXT("mask")])
                    : nullptr;
            UTexture2D *AOTexture =
                AOSourceTexture ? AOSourceTexture : MaskSourceTexture;
            UTexture2D *RoughnessTexture = RoughnessSourceTexture
                                               ? RoughnessSourceTexture
                                               : MaskSourceTexture;
            UTexture2D *DisplacementTexture = DisplacementSourceTexture
                                                  ? DisplacementSourceTexture
                                                  : MaskSourceTexture;
            const int32 AOChannel =
                AOSourceTexture ? 0 : (MaskSourceTexture ? 0 : 0);
            const int32 RoughnessChannel =
                RoughnessSourceTexture ? 0 : (MaskSourceTexture ? 1 : 0);
            const int32 DisplacementChannel =
                DisplacementSourceTexture ? 0 : (MaskSourceTexture ? 2 : 0);
            MaskTexture = CreatePackedMaskTexture(
                AssetFolder, FString::Printf(TEXT("T_%s_M"), *GroupStem),
                AOTexture, AOChannel, RoughnessTexture,
                RoughnessChannel, DisplacementTexture, DisplacementChannel,
                nullptr, 0,
                Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("albedo"))),
                Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("normal"))));
          }
          MaterialInstance = CreateAssetMaterialInstance(
              AssetFolder, GroupStem, TextureBySlot.FindRef(TEXT("albedo")),
              TextureBySlot.FindRef(TEXT("normal")), MaskTexture,
              TextureBySlot.FindRef(TEXT("fuzz")), bUseVT);
        }
      }
      if (MaterialInstance) {
        MaterialInstances.Add(MaterialInstance);
        MaterialGroupIds.Add(GroupId);
      }
      if (OpaqueMaterialInstance) {
        OpaqueMaterialInstances.Add(OpaqueMaterialInstance);
        OpaqueMaterialGroupIds.Add(GroupId);
      }
    }
    for (UStaticMesh *StaticMesh : ImportedMeshes) {
      if (!StaticMesh) {
        continue;
      }
      // Foliage/3D Plants intentionally do not participate in this rule.
      if (bIs3DAsset && FPaths::GetExtension(SourceFileByMesh.FindRef(StaticMesh))
                              .Equals(TEXT("fbx"), ESearchCase::IgnoreCase)) {
        const FString SourceFile = SourceFileByMesh.FindRef(StaticMesh);
        bool bHasSmoothingGroupLayer = false;
        if (const bool *Cached =
                FbxSmoothingGroupCache.Find(SourceFile)) {
          bHasSmoothingGroupLayer = *Cached;
        } else {
          bHasSmoothingGroupLayer = FbxHasSmoothingGroupLayer(SourceFile);
          FbxSmoothingGroupCache.Add(SourceFile, bHasSmoothingGroupLayer);
        }
        if (!bHasSmoothingGroupLayer) {
          FString SmoothingSummary;
          ApplyGeneratedSmoothingGroups(
              StaticMesh, UAssetHiveSettings::GetAsset3DMissingSmoothingAngle(),
              SmoothingSummary);
        }
      }
      SetStageProgress(static_cast<float>(FMath::Clamp(AssetBaseProgress + 30, 0, 99)),
          FString::Printf(TEXT("配置 Nanite 和材质: %s"), *StaticMesh->GetName()));
      // SetMaterial calls Pre/PostEditChange and rebuilds the mesh for EVERY slot.
      // Perform one balanced edit so a high-poly mesh is only rebuilt once here.
      StaticMesh->PreEditChange(nullptr);
      if (bScanSourceAsset) {
        FString TriangleBudgetSummary;
        ApplyNaniteTriangleBudget(StaticMesh, TriangleBudgetSummary);
      }
      if (AssetType == TEXT("3dplant")) {
        // 3D Plants（md §5.2）：Nanite 按子类开关（Micro 不使用），面数按子类
        // 规范压到上限内，并保留面积。
        if (PlantProfile.bNanite && PlantProfile.MaxLOD0Triangles > 0) {
          FString PlantBudgetSummary;
          ApplyPlantNaniteTriangleBudget(StaticMesh,
                                         PlantProfile.MaxLOD0Triangles,
                                         PlantBudgetSummary);
        } else if (PlantProfile.MaxLOD0Triangles > 0) {
          // Micro 不使用 Nanite：面数只能提示，无法靠保留百分比自动收敛。
          const FMeshDescription *BudgetMesh = StaticMesh->GetMeshDescription(0);
          const int32 PlantTriangles =
              BudgetMesh ? BudgetMesh->Triangles().Num() : 0;
          if (PlantTriangles > PlantProfile.MaxLOD0Triangles) {
            UE_LOG(LogTemp, Warning,
                   TEXT("AssetHive import: %s 面数 %d 超出植被规范 %d（Micro 不使用 Nanite，未自动收敛）"),
                   *StaticMesh->GetName(), PlantTriangles,
                   PlantProfile.MaxLOD0Triangles);
          }
        }
        FMeshNaniteSettings *NaniteSettings =
            GetMutableNaniteSettings(StaticMesh);
        NaniteSettings->bEnabled = PlantProfile.bNanite;
        NaniteSettings->ShapePreservation =
            ENaniteShapePreservation::PreserveArea;
      } else {
        GetMutableNaniteSettings(StaticMesh)->bEnabled = true;
      }
      TArray<FStaticMaterial> &Slots = StaticMesh->GetStaticMaterials();
      // Cut _OPAQUE variants use the opaque material set of the same group.
      // The library export flags them on the variant entry instead of the key.
      const FString MeshVariantKey = VariantKeyByMesh.FindRef(StaticMesh);
      const bool bOpaqueMesh =
          IsOpaqueModelVariantKey(MeshVariantKey) ||
          OpaqueModelVariantKeys.Contains(
              NormalizeModelVariantKey(MeshVariantKey));
      const bool bUseOpaqueMaterialSet =
          bOpaqueMesh && OpaqueMaterialInstances.Num() > 0;
      const TArray<UMaterialInstanceConstant *> &SourceMaterials =
          bUseOpaqueMaterialSet ? OpaqueMaterialInstances : MaterialInstances;
      const TArray<int32> &SourceMaterialGroupIds =
          bUseOpaqueMaterialSet ? OpaqueMaterialGroupIds : MaterialGroupIds;
      auto IsBillboardGroup = [&SourceTextureMaterialRoleByGroup](int32 GroupId) {
        return SourceTextureMaterialRoleByGroup.FindRef(GroupId)
            .Equals(TEXT("billboard"), ESearchCase::IgnoreCase);
      };

      // md §5.2（2026-09-30）：Megascans 导入的植被剪切出的 _OPAQUE 裁切网格不带
      // billboard 材质槽——billboard 组既不参与槽位指派，指派结束后引用该槽位的
      // section 会重映射回 Atlas 槽位，槽位本身再从网格材质列表里移除。
      TArray<int32> BillboardSlotIndexes;
      int32 AtlasSlotIndex = INDEX_NONE;
      if (bOpaqueMesh || bSuppressBillboardExport) {
        for (int32 SlotIndex = 0;
             SlotIndex < Slots.Num() && SlotIndex < GroupIds.Num(); ++SlotIndex) {
          if (IsBillboardGroup(GroupIds[SlotIndex])) {
            BillboardSlotIndexes.Add(SlotIndex);
          } else if (AtlasSlotIndex == INDEX_NONE) {
            AtlasSlotIndex = SlotIndex;
          }
        }
      }

      TArray<UMaterialInstanceConstant *> MeshMaterials;
      for (int32 MaterialIndex = 0; MaterialIndex < SourceMaterials.Num();
           ++MaterialIndex) {
        const int32 GroupId =
            SourceMaterialGroupIds.IsValidIndex(MaterialIndex)
                ? SourceMaterialGroupIds[MaterialIndex]
                : INDEX_NONE;
        if ((bOpaqueMesh || bSuppressBillboardExport) &&
            GroupId != INDEX_NONE && IsBillboardGroup(GroupId)) {
          continue;
        }
        MeshMaterials.Add(SourceMaterials[MaterialIndex]);
      }
      if (MeshMaterials.Num() == 0) {
        MeshMaterials = SourceMaterials;
      }
      for (int32 Index = 0; Index < Slots.Num() && MeshMaterials.Num() > 0; ++Index) {
        UMaterialInstanceConstant *Material = MeshMaterials[FMath::Min(Index, MeshMaterials.Num() - 1)];
        Slots[Index].MaterialInterface = Material;
        if (Slots[Index].MaterialSlotName.IsNone()) {
          Slots[Index].MaterialSlotName = Material->GetFName();
        }
        // Preserve imported slot names used by FBX reimport/section matching.
        if (Slots[Index].ImportedMaterialSlotName.IsNone()) {
          FName Candidate = Material->GetFName();
          int32 Suffix = 0;
          auto IsUsed = [&Slots, Index](FName Name) {
            for (int32 Other = 0; Other < Slots.Num(); ++Other) {
              if (Other != Index && Slots[Other].ImportedMaterialSlotName == Name) return true;
            }
            return false;
          };
          while (IsUsed(Candidate)) {
            Candidate = FName(*(Material->GetName() + TEXT("_") + FString::FromInt(++Suffix)));
          }
          Slots[Index].ImportedMaterialSlotName = Candidate;
        }
      }
      if ((bOpaqueMesh || bSuppressBillboardExport) &&
          BillboardSlotIndexes.Num() > 0) {
        // 引用 billboard 槽位的 section 先回落到 Atlas 槽位，再交给引擎移除尾部
        // 未使用材质槽，保证 OPAQUE 网格材质列表里不再出现 billboard 槽位。
        const int32 BillboardRemapTarget =
            AtlasSlotIndex == INDEX_NONE ? 0 : AtlasSlotIndex;
        FMeshSectionInfoMap &SectionInfoMap = StaticMesh->GetSectionInfoMap();
        int32 RemappedSectionCount = 0;
        const int32 SourceModelCount = StaticMesh->GetNumSourceModels();
        for (int32 LodIndex = 0; LodIndex < SourceModelCount; ++LodIndex) {
          const int32 SectionCount = SectionInfoMap.GetSectionNumber(LodIndex);
          for (int32 SectionIndex = 0; SectionIndex < SectionCount;
               ++SectionIndex) {
            FMeshSectionInfo SectionInfo =
                SectionInfoMap.Get(LodIndex, SectionIndex);
            if (BillboardSlotIndexes.Contains(SectionInfo.MaterialIndex)) {
              SectionInfo.MaterialIndex = BillboardRemapTarget;
              SectionInfoMap.Set(LodIndex, SectionIndex, SectionInfo);
              RemappedSectionCount++;
            }
          }
        }
        UStaticMesh::RemoveUnusedMaterialSlots(StaticMesh);
        UE_LOG(LogTemp, Display,
               TEXT("AssetHive import: %s 移除 OPAQUE 变体的 billboard 材质槽 %d 个"
                    "（重映射 section %d 个）"),
               *StaticMesh->GetName(), BillboardSlotIndexes.Num(),
               RemappedSectionCount);
      }
      StaticMesh->PostEditChange();
      StaticMesh->MarkPackageDirty();
      FinalizeImportedAsset(StaticMesh);
      if (bIs3DAsset) {
        Configure3DAssetCollision(StaticMesh);
      } else if (AssetType == TEXT("3dplant")) {
        ConfigurePlantAssetCollision(StaticMesh, PlantProfile);
      }
    }

    // The import target is authoritative here. AssetRegistry visibility can lag
    // behind synchronous imports until later in the frame, so gating this on
    // HasAssets may suppress the post-import Content Browser navigation.
    if (OutImportedFolders) {
      OutImportedFolders->AddUnique(AssetFolder);
    }
    SetStageProgress(static_cast<float>(AssetEndProgress),
                     FString::Printf(TEXT("资产完成: %s"), *AssetName), false);
    AssetIndex++;
  }
  SetStageProgress(100.0f, TEXT("导入完成"), false);
#if (ENGINE_MAJOR_VERSION >= 5 && ENGINE_MINOR_VERSION >= 4)
  if (bNeedRestoreInterchange) {
    if (IConsoleVariable *InterchangeEnable =
            IConsoleManager::Get().FindConsoleVariable(
                TEXT("Interchange.FeatureFlags.Import.Enable"))) {
      InterchangeEnable->Set(bInterchangeOriginalValue);
      UE_LOG(LogTemp, Display,
             TEXT("AssetHive import: restore Interchange flag = %s"),
             bInterchangeOriginalValue ? TEXT("true") : TEXT("false"));
    }
  }
#endif
  UE_LOG(LogTemp, Display, TEXT("AssetHive import completed: %s"),
         *DestinationPath);
  return GAssetHiveImportFailed ? 1 : 0;
}


#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "StaticMeshCompiler.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "UObject/GCObjectScopeGuard.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveMeshImportPerfTest,
    "AssetHive.Import.HighPolyTiming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveMeshImportPerfTest::RunTest(const FString &Parameters) {
  FString Source;
  if (!FParse::Value(FCommandLine::Get(), TEXT("AssetHivePerfSource="), Source)) {
    AddInfo(TEXT("Skipped: supply -AssetHivePerfSource=<fbx> for an isolated high-poly measurement."));
    return true;
  }
  if (!TestTrue(TEXT("Source exists"), FPaths::FileExists(Source))) return false;
  const bool bBaseline = FParse::Param(FCommandLine::Get(), TEXT("AssetHivePerfBaseline"));
  UAssetImportTask *Task = NewObject<UAssetImportTask>();
  FGCObjectScopeGuard TaskGuard(Task);
  Task->Filename = Source;
  Task->DestinationPath = TEXT("/Game/__AssetHivePerf_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Task->DestinationName = TEXT("HighPolyProbe");
  Task->bAutomated = true;
  Task->bAsync = false;
  Task->bSave = false;
  UFbxImportUI *Options = MakeStaticMeshImportOptions();
  if (bBaseline) {
    Options->StaticMeshImportData->bBuildNanite = false;
    Options->StaticMeshImportData->NormalImportMethod = FBXNIM_ComputeNormals;
  }
  Task->Options = Options;
  // Use the same legacy FBX path as ImportJob without changing a project setting.
  IConsoleVariable *Interchange = IConsoleManager::Get().FindConsoleVariable(TEXT("Interchange.FeatureFlags.Import.Enable"));
  const bool bInterchange = Interchange && Interchange->GetBool();
  if (Interchange) Interchange->Set(false);
  const double Start = FPlatformTime::Seconds();
  FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get().ImportAssetTasks({Task});
  if (Interchange) Interchange->Set(bInterchange);
  UStaticMesh *Mesh = nullptr;
  for (UObject *Object : Task->GetObjects()) {
    if (UStaticMesh *Imported = Cast<UStaticMesh>(Object)) { Mesh = Imported; break; }
  }
  if (!TestNotNull(TEXT("Mesh imported"), Mesh)) return false;
  const double ImportedAt = FPlatformTime::Seconds();
  FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
  if (!GetMutableNaniteSettings(Mesh)->bEnabled) {
    Mesh->PreEditChange(nullptr);
    GetMutableNaniteSettings(Mesh)->bEnabled = true;
    Mesh->PostEditChange();
    FStaticMeshCompilingManager::Get().FinishCompilation({Mesh});
  }
  FinalizeImportedAsset(Mesh);
  TestTrue(TEXT("Nanite retained"), GetMutableNaniteSettings(Mesh)->bEnabled);
  TestTrue(TEXT("Mesh stays dirty"), Mesh->GetOutermost()->IsDirty());
  TestNotNull(TEXT("Source geometry retained"), Mesh->GetMeshDescription(0));
  if (!bBaseline) TestFalse(TEXT("Authored normals retained"), Mesh->GetSourceModel(0).BuildSettings.bRecomputeNormals);
  UE_LOG(LogTemp, Display, TEXT("AssetHivePerf: baseline=%d import=%.3fs ready=%.3fs"),
      bBaseline, ImportedAt - Start, FPlatformTime::Seconds() - Start);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveDirtyImportTest,
    "AssetHive.Import.UnsavedPackages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveDirtyImportTest::RunTest(const FString &Parameters) {
  const FString Root = TEXT("/Game/__AssetHiveDirty_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
  // All asset kinds share the explicit dirty finalization contract, including
  // packages that started clean (the reimport case).
  UClass *Classes[] = {UStaticMesh::StaticClass(), UTexture2D::StaticClass(),
      UMaterialInstanceConstant::StaticClass(), UFoliageType_InstancedStaticMesh::StaticClass()};
  for (UClass *Class : Classes) {
    UPackage *Package = CreatePackage(*(Root / Class->GetName()));
    UObject *Object = NewObject<UObject>(Package, Class, FName(TEXT("Probe")), RF_Public | RF_Standalone);
    Package->SetDirtyFlag(false);
    FinalizeImportedAsset(Object);
    TestTrue(TEXT("Imported package is dirty"), Package->IsDirty());
    TestFalse(TEXT("Finalization does not write uasset"), FPaths::FileExists(
        FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension())));
  }
  const FString Source = FPaths::ProjectSavedDir() / TEXT("AssetHiveDirtyProbe.png");
  TArray<FColor> Pixels;
  Pixels.Init(FColor(128, 64, 255, 255), 16);
  TArray64<uint8> Png;
  FImageUtils::PNGCompressImageArray(4, 4, Pixels, Png);
  if (!TestTrue(TEXT("Write source fixture"), FFileHelper::SaveArrayToFile(Png, *Source))) return false;
  auto Asset = MakeShared<FJsonObject>();
  Asset->SetStringField(TEXT("name"), TEXT("DirtyProbe"));
  Asset->SetStringField(TEXT("id"), TEXT("test"));
  Asset->SetStringField(TEXT("assetType"), TEXT("hdri"));
  Asset->SetArrayField(TEXT("textureFiles"), {MakeShared<FJsonValueString>(Source)});
  auto Slot = MakeShared<FJsonObject>();
  Slot->SetStringField(TEXT("file"), Source);
  Slot->SetStringField(TEXT("slot"), TEXT("HDR"));
  Asset->SetArrayField(TEXT("textureSlots"), {MakeShared<FJsonValueObject>(Slot)});
  auto Job = MakeShared<FJsonObject>();
  Job->SetArrayField(TEXT("assets"), {MakeShared<FJsonValueObject>(Asset)});
  UAssetHiveImportCommandlet *Importer = NewObject<UAssetHiveImportCommandlet>();
  FGCObjectScopeGuard ImporterGuard(Importer);
  for (int32 Pass = 0; Pass < 2; ++Pass) {
    TArray<FString> Folders;
    TestEqual(TEXT("Import job completes"), Importer->ImportJob(Job, Root, {}, &Folders), 0);
    if (!TestEqual(TEXT("Imported folder returned"), Folders.Num(), 1)) break;
    TestTrue(TEXT("Imported folder carries asset ID"),
             Folders[0].EndsWith(TEXT("_test"), ESearchCase::IgnoreCase));
    const FString PackageName = Folders[0] / TEXT("T_DirtyProbe_test_HDR");
    UTexture2D *Texture = FindObject<UTexture2D>(nullptr, *(PackageName + TEXT(".T_DirtyProbe_test_HDR")));
    if (!TestNotNull(TEXT("Texture available before saving"), Texture)) break;
    TestTrue(TEXT("Imported texture source ready"), Texture->Source.IsValid());
    TestTrue(TEXT("Imported texture stays dirty"), Texture->GetOutermost()->IsDirty());
    TestFalse(TEXT("Import job never writes uasset"), FPaths::FileExists(
        FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension())));
    Texture->GetOutermost()->SetDirtyFlag(false);
  }
  IFileManager::Get().Delete(*Source);
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveAssetIdNamingTest,
    "AssetHive.Import.AssetIdNaming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveAssetIdNamingTest::RunTest(const FString &Parameters) {
  auto BuildStemForTag = [](const TCHAR *Tag) {
    TArray<FString> Tags;
    if (Tag && *Tag) {
      Tags.Add(FString(Tag));
    }
    return BuildEnvironmentObjectStem(
        ResolveEnvironmentAssetProfile(Tags), TEXT("WoodenBox"),
        TEXT("abc123"));
  };

  TestEqual(TEXT("Props keeps legacy naming"),
            BuildStemForTag(TEXT("Props")),
            FString(TEXT("Env_Prop_WoodenBox_abc123")));
  TestEqual(TEXT("Destructible keeps legacy naming"),
            BuildStemForTag(TEXT("Destructible")),
            FString(TEXT("Env_Dest_WoodenBox_abc123")));
  TestEqual(TEXT("Kits keeps legacy naming"),
            BuildStemForTag(TEXT("Kits")),
            FString(TEXT("Env_Kit_WoodenBox_abc123")));
  TestEqual(TEXT("MEGA keeps legacy naming"),
            BuildStemForTag(TEXT("MEGA")),
            FString(TEXT("Env_MEGA_WoodenBox_abc123")));
  TestEqual(TEXT("PBRMAX keeps legacy naming"),
            BuildStemForTag(TEXT("PBRMAX")),
            FString(TEXT("Env_WoodenBox_abc123")));
  TestEqual(TEXT("Dressing keeps legacy naming"),
            BuildStemForTag(TEXT("Dressing")),
            FString(TEXT("Env_WoodenBox_abc123")));

  const FString MegascansStem = BuildStemForTag(TEXT("Megascans"));
  TestEqual(TEXT("Megascans uses tag and asset ID only"), MegascansStem,
            FString(TEXT("Env_abc123")));
  TestEqual(TEXT("Megascans mesh drops display name"),
            FString::Printf(TEXT("SM_%s_%s"), *MegascansStem, TEXT("01")),
            FString(TEXT("SM_Env_abc123_01")));
  TestEqual(TEXT("Megascans single-group material instance omits group id"),
            BuildEnvironmentAssetMaterialName(MegascansStem, 1, false,
                                               TEXT("01")),
            FString(TEXT("MI_Env_abc123_01")));
  TestEqual(TEXT("Megascans multi-group material instance keeps group id"),
            BuildEnvironmentAssetMaterialName(MegascansStem, 2, true,
                                               TEXT("01")),
            FString(TEXT("MI_Env_abc123_002_01")));

  // 3D Plants 命名（2026-09-30 定稿）：SM_/MI_/T_ 采用
  // Env_<标准AssetTag>_<资产ID> 模板，OPAQUE 变体追加 _OPAQUE 后缀。
  TArray<FString> PlantTags;
  PlantTags.Add(TEXT("Grass"));
  const FString PlantStem = BuildPlantObjectStem(PlantTags, TEXT("abc123"));
  TestEqual(TEXT("Plant stem uses tag and asset ID"), PlantStem,
            FString(TEXT("Env_Grass_abc123")));
  TestEqual(TEXT("Plant masked mesh name"),
            FString::Printf(TEXT("SM_%s_%s"), *PlantStem, TEXT("01")),
            FString(TEXT("SM_Env_Grass_abc123_01")));
  TestEqual(TEXT("Plant opaque mesh name"),
            FString::Printf(TEXT("SM_%s_%s_OPAQUE"), *PlantStem, TEXT("01")),
            FString(TEXT("SM_Env_Grass_abc123_01_OPAQUE")));
  TestEqual(TEXT("Plant texture name uses slot abbreviation"),
            FString::Printf(TEXT("T_%s_%s"), *PlantStem, TEXT("D")),
            FString(TEXT("T_Env_Grass_abc123_D")));
  TestEqual(TEXT("Plant atlas material name"),
            UAssetHiveSettings::GetPlantMaterialName(
                FString::Printf(TEXT("%s_%s"), *PlantStem, TEXT("01")), false,
                false),
            FString(TEXT("MI_Env_Grass_abc123_01")));
  TestEqual(TEXT("Plant opaque material name uses _OPAQUE suffix"),
            UAssetHiveSettings::GetPlantMaterialName(
                FString::Printf(TEXT("%s_%s"), *PlantStem, TEXT("01")), false,
                true),
            FString(TEXT("MI_Env_Grass_abc123_01_OPAQUE")));
  TestEqual(TEXT("Plant billboard material keeps prefix"),
            UAssetHiveSettings::GetPlantMaterialName(
                FString::Printf(TEXT("%s_%s"), *PlantStem, TEXT("01")), true,
                false),
            FString(TEXT("MI_Billboard_Env_Grass_abc123_01")));

  // 命名模板只对 FBX 原始资产生效：st9（SpeedTree）保持原有命名。
  auto FbxPlantObject = MakeShared<FJsonObject>();
  FbxPlantObject->SetArrayField(
      TEXT("modelFiles"),
      {MakeShared<FJsonValueString>(FString(TEXT("D:/Plant/fern_01.fbx")))});
  TestTrue(TEXT("FBX plant uses the tag naming template"),
           AssetHasFbxPlantModels(FbxPlantObject));

  auto St9PlantObject = MakeShared<FJsonObject>();
  St9PlantObject->SetArrayField(
      TEXT("modelFiles"),
      {MakeShared<FJsonValueString>(FString(TEXT("D:/Plant/tree_01.st9")))});
  TestFalse(TEXT("st9 plant keeps legacy naming"),
            AssetHasFbxPlantModels(St9PlantObject));

  // 3D Plants 导出档案（md §5.2）：路径 / 面数 / Nanite / 碰撞 / 纹理预设。
  const FPlantAssetProfile TreeProfile =
      ResolvePlantAssetProfile({FString(TEXT("Tree"))});
  TestEqual(TEXT("Plant tree folder"), TreeProfile.SubtypeFolder,
            FString(TEXT("Tree")));
  TestEqual(TEXT("Plant tree triangle budget"), TreeProfile.MaxLOD0Triangles,
            100000);
  TestTrue(TEXT("Plant tree uses trunk collision"), TreeProfile.bTrunkCollision);
  TestEqual(TEXT("Plant tree texture size"), TreeProfile.TextureMaxSize, 2048);

  const FPlantAssetProfile BushProfile =
      ResolvePlantAssetProfile({FString(TEXT("Bush"))});
  TestEqual(TEXT("Plant bush triangle budget"), BushProfile.MaxLOD0Triangles,
            35000);
  TestTrue(TEXT("Plant bush manages collision"), BushProfile.bHandleCollision);
  TestFalse(TEXT("Plant bush has no trunk collision"),
            BushProfile.bTrunkCollision);

  const FPlantAssetProfile GrassProfile =
      ResolvePlantAssetProfile({FString(TEXT("Grass"))});
  TestEqual(TEXT("Plant grass triangle budget"), GrassProfile.MaxLOD0Triangles,
            10000);

  const FPlantAssetProfile HeroProfile =
      ResolvePlantAssetProfile({FString(TEXT("HeroFoliage"))});
  TestEqual(TEXT("Hero foliage triangle budget"),
            HeroProfile.MaxLOD0Triangles, 200000);
  TestTrue(TEXT("Hero foliage uses trunk collision"),
           HeroProfile.bTrunkCollision);
  TestEqual(TEXT("Hero foliage texture size"), HeroProfile.TextureMaxSize,
            4096);

  const FPlantAssetProfile MicroProfile =
      ResolvePlantAssetProfile({FString(TEXT("MicroFoliage"))});
  TestEqual(TEXT("Plant micro folder"), MicroProfile.SubtypeFolder,
            FString(TEXT("Micro")));
  TestEqual(TEXT("Plant micro triangle budget"), MicroProfile.MaxLOD0Triangles,
            500);
  TestFalse(TEXT("Plant micro disables Nanite"), MicroProfile.bNanite);
  TestEqual(TEXT("Plant micro texture size"), MicroProfile.TextureMaxSize,
            1024);
  TestFalse(TEXT("Plant micro disables virtual textures"),
            MicroProfile.bAllowVirtualTexture);

  const FPlantAssetProfile UnknownProfile =
      ResolvePlantAssetProfile({FString(TEXT("Kit"))});
  TestTrue(TEXT("Unknown plant tag keeps folder empty"),
           UnknownProfile.SubtypeFolder.IsEmpty());
  TestFalse(TEXT("Unknown plant tag leaves collision untouched"),
            UnknownProfile.bHandleCollision);

  TestTrue(TEXT("Trunk slot token matches bark"),
           IsPlantTrunkSlotToken(TEXT("Common_Beech_Bark")));
  TestFalse(TEXT("Trunk slot token ignores atlas"),
            IsPlantTrunkSlotToken(TEXT("Common_Beech_Atlas")));
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveMeshTriangleBudgetTest,
    "AssetHive.Import.MeshTriangleBudget",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveMeshTriangleBudgetTest::RunTest(const FString &Parameters) {
  // A flat quad grid gives a deterministic triangle count to budget against.
  const int32 Grid = 40; // 2 * 40 * 40 = 3200 source triangles
  const int32 SourceTriangles = Grid * Grid * 2;
  const int32 SmallBudget = 320;
  const int32 LargeBudget = SourceTriangles;

  auto MakeGridMesh = [Grid](float Step, const TCHAR *Name) {
    UStaticMesh *Mesh =
        NewObject<UStaticMesh>(GetTransientPackage(), FName(Name), RF_Transient);
    Mesh->AddSourceModel();

    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector2f> UVs =
        Attributes.GetVertexInstanceUVs();
    TVertexInstanceAttributesRef<FVector3f> Normals =
        Attributes.GetVertexInstanceNormals();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));

    TArray<TArray<FVertexInstanceID>> Instances;
    Instances.SetNum((Grid + 1) * (Grid + 1));
    for (int32 Y = 0; Y <= Grid; ++Y) {
      for (int32 X = 0; X <= Grid; ++X) {
        const FVertexID Vertex = Description.CreateVertex();
        Positions[Vertex] = FVector3f(X * Step, Y * Step, 0.0f);
        TArray<FVertexInstanceID> &Corners = Instances[Y * (Grid + 1) + X];
        for (int32 Corner = 0; Corner < 2; ++Corner) {
          const FVertexInstanceID Instance =
              Description.CreateVertexInstance(Vertex);
          UVs.Set(Instance, 0,
                  FVector2f(X / static_cast<float>(Grid),
                            Y / static_cast<float>(Grid)));
          Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
          Corners.Add(Instance);
        }
      }
    }
    auto Corner = [&Instances, Grid](int32 X, int32 Y, int32 Slot) {
      return Instances[Y * (Grid + 1) + X][Slot];
    };
    for (int32 Y = 0; Y < Grid; ++Y) {
      for (int32 X = 0; X < Grid; ++X) {
        Description.CreateTriangle(
            Group,
            {Corner(X, Y, 0), Corner(X + 1, Y, 0), Corner(X + 1, Y + 1, 0)});
        Description.CreateTriangle(
            Group,
            {Corner(X, Y, 1), Corner(X + 1, Y + 1, 1), Corner(X, Y + 1, 1)});
      }
    }
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    Mesh->CommitMeshDescription(0);
    return Mesh;
  };

  UAssetHiveSettings *Settings = GetMutableDefault<UAssetHiveSettings>();
  const int32 SavedMaxTriangles = Settings->Asset3DMaxLOD0Triangles;
  const int32 SavedLargeTriangles = Settings->Asset3DLargeMaxLOD0Triangles;
  const float SavedThreshold = Settings->Asset3DLargeSizeThresholdCm;

  // 4 m asset, well inside the default 300k spec: Nanite stays untouched.
  UStaticMesh *SmallMesh = MakeGridMesh(10.0f, TEXT("AssetHiveSmallProbe"));
  FGCObjectScopeGuard SmallMeshGuard(SmallMesh);
  TestEqual(TEXT("Fixture triangle count"),
            SmallMesh->GetMeshDescription(0)->Triangles().Num(), SourceTriangles);
  FString SmallSummary;
  TestFalse(TEXT("In-budget asset keeps 100% of the source"),
            ApplyNaniteTriangleBudget(SmallMesh, SmallSummary));
  TestTrue(TEXT("Keep percentage stays at 1.0"),
           FMath::IsNearlyEqual(GetMutableNaniteSettings(SmallMesh)->KeepPercentTriangles,
                                1.0f));

  // Tightening the spec makes the same asset shrink through Nanite.
  Settings->Asset3DMaxLOD0Triangles = SmallBudget;
  SmallSummary.Reset();
  TestTrue(TEXT("Oversized asset receives a Nanite keep percentage"),
           ApplyNaniteTriangleBudget(SmallMesh, SmallSummary));
  TestTrue(TEXT("Keep percentage follows the triangle spec"),
           FMath::IsNearlyEqual(GetMutableNaniteSettings(SmallMesh)->KeepPercentTriangles,
                                static_cast<float>(SmallBudget) /
                                    static_cast<float>(SourceTriangles),
                                0.001f));
  TestTrue(TEXT("Summary reports the budget"),
           SmallSummary.Contains(TEXT("keep")));

  // A 40 m asset is routed to the large asset budget instead.
  UStaticMesh *LargeMesh = MakeGridMesh(100.0f, TEXT("AssetHiveLargeProbe"));
  FGCObjectScopeGuard LargeMeshGuard(LargeMesh);
  Settings->Asset3DMaxLOD0Triangles = 100;
  Settings->Asset3DLargeMaxLOD0Triangles = LargeBudget;
  FString LargeSummary;
  TestFalse(TEXT("Large asset ignores the small asset budget"),
            ApplyNaniteTriangleBudget(LargeMesh, LargeSummary));
  TestTrue(TEXT("Large asset keeps 100% of the source"),
           FMath::IsNearlyEqual(GetMutableNaniteSettings(LargeMesh)->KeepPercentTriangles,
                                1.0f));
  TestTrue(TEXT("Summary stays empty when nothing changes"),
           LargeSummary.IsEmpty());

  // The large asset still shrinks when its own budget is exceeded.
  Settings->Asset3DLargeMaxLOD0Triangles = SmallBudget;
  TestTrue(TEXT("Large asset uses its own budget"),
           ApplyNaniteTriangleBudget(LargeMesh, LargeSummary));
  TestTrue(TEXT("Large asset keep percentage follows the large spec"),
           FMath::IsNearlyEqual(GetMutableNaniteSettings(LargeMesh)->KeepPercentTriangles,
                                static_cast<float>(SmallBudget) /
                                    static_cast<float>(SourceTriangles),
                                0.001f));

  Settings->Asset3DMaxLOD0Triangles = SavedMaxTriangles;
  Settings->Asset3DLargeMaxLOD0Triangles = SavedLargeTriangles;
  Settings->Asset3DLargeSizeThresholdCm = SavedThreshold;
  return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveMissingSmoothingGroupsTest,
    "AssetHive.Import.MissingSmoothingGroups",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveMissingSmoothingGroupsTest::RunTest(const FString &Parameters) {
  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveSmoothingProbe")), RF_Transient);
  FGCObjectScopeGuard MeshGuard(Mesh);
  Mesh->AddSourceModel();

  FMeshDescription Description;
  FStaticMeshAttributes Attributes(Description);
  Attributes.Register();
  TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector3f> Normals =
      Attributes.GetVertexInstanceNormals();
  const FPolygonGroupID Group = Description.CreatePolygonGroup();
  Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
      FName(TEXT("Material_0"));

  auto AddCorner = [&Description, &Positions, &Normals](
                       const FVector3f &Position, const FVector3f &Normal) {
    const FVertexID Vertex = Description.CreateVertex();
    Positions[Vertex] = Position;
    const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertex);
    Normals[Instance] = Normal;
    return Instance;
  };

  const FVertexInstanceID A = AddCorner(FVector3f(0.0f, 0.0f, 0.0f),
                                        FVector3f(0.0f, 0.0f, 1.0f));
  const FVertexInstanceID B = AddCorner(FVector3f(1.0f, 0.0f, 0.0f),
                                        FVector3f(0.0f, 0.0f, 1.0f));
  const FVertexInstanceID C = AddCorner(FVector3f(0.0f, 1.0f, 0.0f),
                                        FVector3f(0.0f, 0.0f, 1.0f));
  const FVertexInstanceID D = AddCorner(FVector3f(0.0f, 0.0f, 1.0f),
                                        FVector3f(0.0f, 1.0f, 0.0f));
  const FVertexInstanceID E = AddCorner(FVector3f(1.0f, 1.0f, 0.0f),
                                        FVector3f(0.0f, 0.0f, 1.0f));
  Description.CreateTriangle(Group, {A, B, C});
  Description.CreateTriangle(Group, {B, A, D});
  Description.CreateTriangle(Group, {A, E, C});
  Mesh->CreateMeshDescription(0, MoveTemp(Description));
  Mesh->CommitMeshDescription(0);

  FString Summary;
  TestTrue(TEXT("Missing smoothing groups are generated"),
           ApplyGeneratedSmoothingGroups(Mesh, 60.0f, Summary));
  TestFalse(TEXT("Recompute Normals remains disabled"),
            Mesh->GetSourceModel(0).BuildSettings.bRecomputeNormals);

  FMeshDescription *Generated = Mesh->GetMeshDescription(0);
  if (!TestNotNull(TEXT("Generated mesh description exists"), Generated)) {
    return false;
  }
  FStaticMeshAttributes GeneratedAttributes(*Generated);
  TEdgeAttributesRef<bool> EdgeHardnesses =
      GeneratedAttributes.GetEdgeHardnesses();
  int32 HardEdgeCount = 0;
  for (const FEdgeID EdgeID : Generated->Edges().GetElementIDs()) {
    HardEdgeCount += EdgeHardnesses[EdgeID] ? 1 : 0;
  }
  TestEqual(TEXT("Fixture edge count"), Generated->Edges().Num(), 7);
  TestEqual(TEXT("Right-angle and border edges are hard"), HardEdgeCount, 6);
  TestTrue(TEXT("Summary records the disabled normal recompute"),
           Summary.Contains(TEXT("Recompute Normals remains disabled")));
  return true;
}

#endif
