#include "AssetHiveImportCommandlet.h"
#include "AssetHiveSettings.h"

#include "AssetImportTask.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Internationalization/Regex.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PixelFormat.h"
#include "UObject/SavePackage.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/Package.h"


// Force a texture's resource to be recreated so the live viewport shows valid
// pixels right after import. Paired with SaveAssetPackage (which synchronously
// builds and caches the texture's platform data during serialization), this
// eliminates the first-import race where the async DDC build had not finished
// yet, causing the material to render the default "missing" texture.
static void ForceTextureDataReady(UTexture2D *Texture) {
  if (!Texture) {
    return;
  }
  Texture->UpdateResource();
}

// Imports run on the game thread; keep per-call failure state separate from nested operations.
static thread_local bool GAssetHiveSaveFailed = false;
static void SaveAssetPackage(UObject* Object) {
  if (!Object) return;
  UPackage* Package = Object->GetOutermost();
  const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
  IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
  FSavePackageArgs Args;
  Args.TopLevelFlags = RF_Public | RF_Standalone;
  Args.SaveFlags = SAVE_NoError;
  // Save the requested package directly. The editor checkout/prompt helper can skip
  // newly imported packages while dirty marking is suppressed during load/automation.
  if (!UPackage::SavePackage(Package, Object, *Filename, Args)) {
    GAssetHiveSaveFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: failed to save package %s"), *Package->GetName());
  }
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

static FString NormalizePathLower(const FString &Value) {
  FString Result = Value.Replace(TEXT("\\"), TEXT("/"));
  return Result.ToLower();
}

static FString DetectTextureSlot(const FString &SourceFile) {
  const FString Name = FPaths::GetBaseFilename(SourceFile).ToLower();
  if (Name.Contains(TEXT("albedo")) || Name.Contains(TEXT("basecolor")) ||
      Name.Contains(TEXT("base_color")) || Name.Contains(TEXT("diffuse")) ||
      Name.Contains(TEXT("color")))
    return TEXT("albedo");
  if (Name.Contains(TEXT("hdr")) || Name.Contains(TEXT("hdri")) ||
      Name.EndsWith(TEXT(".hdr")) || Name.EndsWith(TEXT(".exr")))
    return TEXT("hdr");
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
  if (Name.Contains(TEXT("displacement")) || Name.Contains(TEXT("height")))
    return TEXT("displacement");
  if (Name.Contains(TEXT("fuzz")))
    return TEXT("fuzz");
  if (Name.Contains(TEXT("mask")) || Name.Contains(TEXT("ordp")))
    return TEXT("mask");
  if (Name.Contains(TEXT("specular")) || Name.Contains(TEXT("spec")))
    return TEXT("specular");
  if (Name.Contains(TEXT("opacity")) || Name.Contains(TEXT("alpha")) ||
      Name.Contains(TEXT("transparency")))
    return TEXT("opacity");
  if (Name.Contains(TEXT("translucency")) ||
      Name.Contains(TEXT("translucent")) ||
      Name.Contains(TEXT("transmission")) || Name.Contains(TEXT("sss")))
    return TEXT("translucency");
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

static UFbxImportUI *MakeStaticMeshImportOptions() {
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
    ImportOptions->StaticMeshImportData->NormalImportMethod = FBXNIM_ComputeNormals;
  }
  return ImportOptions;
}

static UStaticMesh *ImportStaticMeshAsset(FAssetToolsModule &AssetToolsModule,
                                          const FString &SourceFile,
                                          const FString &DestinationPath,
                                          const FString &DestinationName) {
  UAssetImportTask *Task = NewObject<UAssetImportTask>();
  Task->Filename = SourceFile;
  Task->DestinationPath = DestinationPath;
  Task->DestinationName = DestinationName;
  Task->bReplaceExisting = true;
  Task->bAutomated = true;
  Task->bAsync = false;
  Task->bSave = false;
  Task->Options = MakeStaticMeshImportOptions();
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

static bool ExtractPlantVariantAndLod(const FString &SourceFile,
                                      int32 &OutVariantId, int32 &OutLodIndex) {
  const FString Base = FPaths::GetBaseFilename(SourceFile).ToLower();
  const FString FullPath = SourceFile.Replace(TEXT("\\"), TEXT("/")).ToLower();
  OutVariantId = 1;
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
        TEXT("(?:^|[/_\\-.])var(?:iant)?_?(\\d+)(?=$|[/_\\-.])"));
    FRegexMatcher VariantNamedMatcher(VariantNamedPattern, FullPath);
    if (VariantNamedMatcher.FindNext()) {
      const FString Token = VariantNamedMatcher.GetCaptureGroup(1);
      const int32 Parsed = FCString::Atoi(*Token);
      if (Parsed > 0) {
        OutVariantId = Parsed;
        return true;
      }
    }
  }

  {
    const FRegexPattern VariantPattern(
        TEXT("(^|[_\\-.])(\\d{1,3})(?=($|[_\\-.]))"));
    FRegexMatcher VariantMatcher(VariantPattern, Base);
    if (VariantMatcher.FindNext()) {
      const FString Token = VariantMatcher.GetCaptureGroup(2);
      const int32 Parsed = FCString::Atoi(*Token);
      if (Parsed > 0) {
        OutVariantId = Parsed;
      }
    }
  }

  return true;
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
    const FString &AssetFolder, const FString &AssetName, UTexture2D *AOTexture,
    int32 AOChannel, UTexture2D *RoughnessTexture, int32 RoughnessChannel,
    UTexture2D *DisplacementTexture, int32 DisplacementChannel,
    UTexture2D *SizeRefA, UTexture2D *SizeRefB) {
  const bool HasAOInput = AOTexture != nullptr;
  const bool HasRoughnessInput = RoughnessTexture != nullptr;
  const bool HasDisplacementInput = DisplacementTexture != nullptr;
  FTexturePixels AOPixels;
  FTexturePixels RoughnessPixels;
  FTexturePixels DisplacementPixels;
  const bool HasAO = ReadTexturePixels(AOTexture, AOPixels);
  const bool HasRoughness =
      ReadTexturePixels(RoughnessTexture, RoughnessPixels);
  const bool HasDisplacement =
      ReadTexturePixels(DisplacementTexture, DisplacementPixels);
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

  const FString TextureAssetName = FString::Printf(TEXT("T_%s_M"), *AssetName);
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
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = DisplacementValue;
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
  SaveAssetPackage(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
}

static UTexture2D *
CreatePackedDROTexture(const FString &AssetFolder, const FString &AssetName,
                       UTexture2D *DisplacementTexture,
                       int32 DisplacementChannel, UTexture2D *RoughnessTexture,
                       int32 RoughnessChannel, UTexture2D *OpacityTexture,
                       int32 OpacityChannel, UTexture2D *SizeRefA,
                       UTexture2D *SizeRefB) {
  FTexturePixels DisplacementPixels;
  FTexturePixels RoughnessPixels;
  FTexturePixels OpacityPixels;
  const bool HasDisplacement =
      ReadTexturePixels(DisplacementTexture, DisplacementPixels);
  const bool HasRoughness =
      ReadTexturePixels(RoughnessTexture, RoughnessPixels);
  const bool HasOpacity = ReadTexturePixels(OpacityTexture, OpacityPixels);
  int32 Width = 0;
  int32 Height = 0;
  if (HasDisplacement) {
    Width = DisplacementPixels.Width;
    Height = DisplacementPixels.Height;
  } else if (HasRoughness) {
    Width = RoughnessPixels.Width;
    Height = RoughnessPixels.Height;
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

  const FString TextureAssetName =
      FString::Printf(TEXT("T_%s_DRO"), *AssetName);
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
      const uint8 DisplacementValue =
          SampleChannel(HasDisplacement ? &DisplacementPixels : nullptr, U, V,
                        DisplacementChannel, static_cast<uint8>(128));
      const uint8 RoughnessValue =
          SampleChannel(HasRoughness ? &RoughnessPixels : nullptr, U, V,
                        RoughnessChannel, static_cast<uint8>(204));
      const uint8 OpacityValue =
          SampleChannel(HasOpacity ? &OpacityPixels : nullptr, U, V,
                        OpacityChannel, static_cast<uint8>(255));
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = OpacityValue;
      DestData[DestIndex + 1] = RoughnessValue;
      DestData[DestIndex + 2] = DisplacementValue;
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
  SaveAssetPackage(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
}

static UTexture2D *CreatePackedPlantAlbedoTexture(
    const FString &AssetFolder, const FString &AssetName,
    UTexture2D *AlbedoSourceTexture, UTexture2D *OpacitySourceTexture) {
  FTexturePixels AlbedoPixels;
  FTexturePixels OpacityPixels;
  const bool HasAlbedo = ReadTexturePixels(AlbedoSourceTexture, AlbedoPixels);
  if (!HasAlbedo) {
    return nullptr;
  }
  const bool HasOpacity =
      ReadTexturePixels(OpacitySourceTexture, OpacityPixels);

  const int32 Width = AlbedoPixels.Width;
  const int32 Height = AlbedoPixels.Height;

  const FString TextureAssetName =
      FString::Printf(TEXT("T_%s_Albedo"), *AssetName);
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
      const FColor &AlbedoPixel = AlbedoPixels.Pixels[Y * Width + X];
      uint8 AlphaValue;
      if (HasOpacity) {
        const float U =
            Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1)
                      : 0.0f;
        const float V =
            Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1)
                       : 0.0f;
        AlphaValue = SampleLuminance(&OpacityPixels, U, V, 255);
      } else {
        // No separate opacity source 鈥?keep whatever alpha the albedo already
        // has (the JS exporter pre-merges opacity into the albedo alpha when
        // possible).
        AlphaValue = AlbedoPixel.A;
      }
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = AlbedoPixel.B;
      DestData[DestIndex + 1] = AlbedoPixel.G;
      DestData[DestIndex + 2] = AlbedoPixel.R;
      DestData[DestIndex + 3] = AlphaValue;
    }
  }
  PackedTexture->Source.UnlockMip(0);
  PackedTexture->CompressionSettings = TC_Default;
  PackedTexture->SRGB = true;
  PackedTexture->MipGenSettings = TMGS_Sharpen7;
  PackedTexture->CompressionNoAlpha = false;
  PackedTexture->PostEditChange();
  PackedTexture->MarkPackageDirty();
  ForceTextureDataReady(PackedTexture);
  SaveAssetPackage(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
}

static UTexture2D *
CreatePackedNRSTexture(const FString &AssetFolder, const FString &AssetName,
                       UTexture2D *NormalTexture, UTexture2D *RoughnessTexture,
                       UTexture2D *TranslucencyTexture, UTexture2D *SizeRefA,
                       UTexture2D *SizeRefB) {
  FTexturePixels NormalPixels;
  FTexturePixels RoughnessPixels;
  FTexturePixels TranslucencyPixels;
  const bool HasNormal = ReadTexturePixels(NormalTexture, NormalPixels);
  const bool HasRoughness =
      ReadTexturePixels(RoughnessTexture, RoughnessPixels);
  const bool HasTranslucency =
      ReadTexturePixels(TranslucencyTexture, TranslucencyPixels);
  int32 Width = 0;
  int32 Height = 0;
  if (HasNormal) {
    Width = NormalPixels.Width;
    Height = NormalPixels.Height;
  } else if (HasRoughness) {
    Width = RoughnessPixels.Width;
    Height = RoughnessPixels.Height;
  } else if (HasTranslucency) {
    Width = TranslucencyPixels.Width;
    Height = TranslucencyPixels.Height;
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

  const FString TextureAssetName =
      FString::Printf(TEXT("T_%s_NRS"), *AssetName);
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
  const uint8 DefaultNormal = 0;
  const uint8 DefaultRoughness = static_cast<uint8>(204);
  const uint8 DefaultTranslucency = static_cast<uint8>(255);
  for (int32 Y = 0; Y < Height; Y++) {
    for (int32 X = 0; X < Width; X++) {
      const float U =
          Width > 1 ? static_cast<float>(X) / static_cast<float>(Width - 1)
                    : 0.0f;
      const float V =
          Height > 1 ? static_cast<float>(Y) / static_cast<float>(Height - 1)
                     : 0.0f;
      const uint8 NormalR = SampleChannel(HasNormal ? &NormalPixels : nullptr,
                                          U, V, 0, DefaultNormal);
      const uint8 NormalG = SampleChannel(HasNormal ? &NormalPixels : nullptr,
                                          U, V, 1, DefaultNormal);
      const uint8 Roughness = SampleChannel(
          HasRoughness ? &RoughnessPixels : nullptr, U, V, 0, DefaultRoughness);
      const uint8 Translucency =
          SampleLuminance(HasTranslucency ? &TranslucencyPixels : nullptr, U, V,
                          DefaultTranslucency);
      const int32 DestIndex = (Y * Width + X) * 4;
      DestData[DestIndex + 0] = Roughness;
      DestData[DestIndex + 1] = NormalG;
      DestData[DestIndex + 2] = NormalR;
      DestData[DestIndex + 3] = Translucency;
    }
  }
  PackedTexture->Source.UnlockMip(0);
  PackedTexture->CompressionSettings = TC_Masks;
  PackedTexture->SRGB = false;
  PackedTexture->CompressionNoAlpha = false;
  PackedTexture->PostEditChange();
  PackedTexture->MarkPackageDirty();
  ForceTextureDataReady(PackedTexture);
  SaveAssetPackage(PackedTexture);
  FAssetRegistryModule::AssetCreated(PackedTexture);
  return PackedTexture;
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
    GAssetHiveSaveFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing parent material: %s"), *ParentPath);
    return nullptr;
  }
  if (bUseVT) {
    for (UTexture *Texture : Textures) {
      if (Texture && !Texture->VirtualTextureStreaming) {
        Texture->Modify();
        Texture->VirtualTextureStreaming = true;
        Texture->PostEditChange();
        Texture->MarkPackageDirty();
        SaveAssetPackage(Texture);
      }
    }
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
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Albedo")), AlbedoTexture);
  }
  if (MaskTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Mask")), MaskTexture);
  }
  if (NormalTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Normal")), NormalTexture);
  }
  if (FuzzTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("fuzzmap")), FuzzTexture);
  }

  MaterialInstance->PostEditChange();
  MaterialInstance->MarkPackageDirty();
  SaveAssetPackage(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  return MaterialInstance;
}

static UMaterialInstanceConstant *
CreateGrassMaterialInstance(const FString &AssetFolder,
                            const FString &AssetName, UTexture *AlbedoTexture,
                            UTexture *NRSTexture, bool bUseVT) {
  UMaterialInterface *ParentMaterial = LoadAssetMaterialParent(
      TEXT("MMI_Grass"), bUseVT, {AlbedoTexture, NRSTexture});
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
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Albedo")), AlbedoTexture);
  }
  if (NRSTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("NRS")), NRSTexture);
  }

  MaterialInstance->PostEditChange();
  MaterialInstance->MarkPackageDirty();
  SaveAssetPackage(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
  return MaterialInstance;
}

static UMaterialInstanceConstant *
CreateDecalMaterialInstance(const FString &AssetFolder,
                            const FString &AssetName, UTexture *AlbedoTexture,
                            UTexture *NormalTexture, UTexture *DROTexture,
                            bool bUseVT) {
  UMaterialInterface *ParentMaterial = LoadAssetMaterialParent(
      TEXT("MMI_GeneralDecal"), bUseVT, {AlbedoTexture, NormalTexture, DROTexture});
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
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Albedo")), AlbedoTexture);
  }
  if (DROTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("DRO")), DROTexture);
  }
  if (NormalTexture) {
    UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(
        MaterialInstance, FName(TEXT("Normal")), NormalTexture);
  }

  MaterialInstance->PostEditChange();
  MaterialInstance->MarkPackageDirty();
  SaveAssetPackage(MaterialInstance);
  if (bIsNew) {
    FAssetRegistryModule::AssetCreated(MaterialInstance);
  }
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
    const FString& DestinationPath, TFunction<void(int32, const FString&)> OnProgress,
    TArray<FString>* OutImportedFolders) {
  TGuardValue<bool> SaveFailureGuard(GAssetHiveSaveFailed, false);
  if (OutImportedFolders) OutImportedFolders->Reset();
  if (!Root.IsValid() || !UAssetHiveSettings::IsValidImportRootPath(DestinationPath)) {
    UE_LOG(LogTemp, Error, TEXT("Invalid import job or import root path: %s"), *DestinationPath);
    return 1;
  }
  const auto SetStageProgress = [&OnProgress](float Target, const FString& Stage) {
    const int32 Percent = FMath::Clamp(FMath::RoundToInt(Target), 0, 100);
    UE_LOG(LogTemp, Display, TEXT("[AssetHiveProgress]%d|%s"), Percent, *Stage);
    if (OnProgress) OnProgress(Percent, Stage);
  };
  SetStageProgress(2.0f, TEXT("读取导入任务"));
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
        FString::Printf(TEXT("处理资产 %d/%d"), AssetIndex + 1, AssetCount));

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
    const bool bIsDecal = AssetType == TEXT("decal");
    const bool bIsHdri = AssetType == TEXT("hdri");
    const bool bIsModelAsset =
        AssetType == TEXT("3d") || AssetType == TEXT("3dplant");
    const bool bIsCustomAsset = AssetSource == TEXT("custom");
    if (AssetId.IsEmpty()) {
      AssetId = TEXT("UnknownId");
    }
    const FString SafeAssetName = MakeSafeObjectName(AssetName);
    const FString SafeAssetId = MakeSafeObjectName(AssetId);
    const FString SafeCategoryFolder = MakeSafeObjectName(CategoryFolder);
    const FString SafeAssetFolderName = MakeSafeObjectName(
        AssetFolderName.IsEmpty() ? SafeAssetName : AssetFolderName);
    const FString AssetStem =
        FString::Printf(TEXT("%s_%s"), *SafeAssetName, *SafeAssetId);
    const FString AssetFolder =
        DestinationPath / SafeCategoryFolder / SafeAssetFolderName;

    TMap<int32, TMap<FString, FString>> SourceTextureSlotMapByGroup;
    TMap<int32, TMap<FString, FString>> SourceTextureNormalFormatMapByGroup;
    TMap<FString, int32> SourceTextureGroupByPath;
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
        int32 GroupId = 1;
        double GroupIdValue = 1.0;
        SlotObject->TryGetStringField(TEXT("file"), SourceFile);
        SlotObject->TryGetStringField(TEXT("slot"), SlotName);
        if (SlotObject->TryGetNumberField(TEXT("groupId"), GroupIdValue)) {
          GroupId = FMath::Max(1, static_cast<int32>(GroupIdValue));
        }
        if (!SourceFile.IsEmpty() && !SlotName.IsEmpty()) {
          GroupId = FMath::Max(1, GroupId);
          const FString SourceKey = NormalizePathLower(SourceFile);
          FString NormalizedSlotName = SlotName.ToLower();
          if (NormalizedSlotName == TEXT("m") || NormalizedSlotName == TEXT("ordp")) {
            NormalizedSlotName = TEXT("mask");
          } else if (NormalizedSlotName == TEXT("mask")) {
            NormalizedSlotName = TEXT("mask");
          }
          SourceTextureSlotMapByGroup.FindOrAdd(GroupId).Add(
              SourceKey, NormalizedSlotName);
          SourceTextureGroupByPath.Add(SourceKey, GroupId);
          if (SlotObject->TryGetStringField(TEXT("normalMapFormat"),
                                            NormalMapFormat) &&
              !NormalMapFormat.IsEmpty()) {
            SourceTextureNormalFormatMapByGroup.FindOrAdd(GroupId).Add(
                SourceKey, NormalMapFormat.ToLower());
          }
        }
      }
    }

    TArray<UStaticMesh *> ImportedMeshes;
    TMap<int32, TMap<FString, UTexture *>> TextureBySlotByGroup;
    TMap<int32, TMap<FString, FString>> SourceTextureBySlotByGroup;
    const bool bMultipleTextureGroups = SourceTextureSlotMapByGroup.Num() > 1;

    const TArray<TSharedPtr<FJsonValue>> *ModelFiles = nullptr;
    if (AssetObject->TryGetArrayField(TEXT("modelFiles"), ModelFiles) &&
        ModelFiles != nullptr) {
      bool bHandledModelImport = false;
      if (AssetType == TEXT("3dplant") && bIsModelAsset) {
        struct FPlantModelEntry {
          FString SourceFile;
          int32 VariantId = 1;
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
          int32 VariantId = 1;
          int32 LodIndex = 0;
          ExtractPlantVariantAndLod(SourceFile, VariantId, LodIndex);
          FPlantModelEntry Entry;
          Entry.SourceFile = SourceFile;
          Entry.VariantId = VariantId;
          Entry.LodIndex = LodIndex;
          PlantModels.Add(Entry);
        }

        TMap<int32, TArray<FPlantModelEntry>> ByVariant;
        for (const FPlantModelEntry &Entry : PlantModels) {
          ByVariant.FindOrAdd(Entry.VariantId).Add(Entry);
        }

        TArray<int32> VariantIds;
        ByVariant.GetKeys(VariantIds);
        VariantIds.Sort();

        int32 VariantCounter = 0;
        for (const int32 VariantId : VariantIds) {
          VariantCounter += 1;
          TArray<FPlantModelEntry> &Entries = ByVariant.FindChecked(VariantId);
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

          const FString VariantStem =
              VariantIds.Num() > 1 ? FString::Printf(TEXT("%s_Var%02d"),
                                                     *AssetStem, VariantCounter)
                                   : AssetStem;
          const FString BaseMeshName =
              FString::Printf(TEXT("SM_%s"), *VariantStem);

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 8, 0, 99)),
              FString::Printf(TEXT("导入植物模型: %s"),
                              *FPaths::GetCleanFilename(BaseFile)));
          UStaticMesh *BaseMesh = ImportStaticMeshAsset(
              AssetToolsModule, BaseFile, AssetFolder, BaseMeshName);
          if (!BaseMesh) {
            continue;
          }

          BaseMesh->NaniteSettings.bEnabled = true;
          BaseMesh->PostEditChange();
          BaseMesh->MarkPackageDirty();
          ImportedMeshes.Add(BaseMesh);

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
          if (bIsCustomAsset && bIsModelAsset) {
            if (ValidModelCount <= 1) {
              ModelAssetName = FString::Printf(TEXT("SM_%s"), *AssetStem);
            } else {
              ModelAssetName = FString::Printf(
                  TEXT("SM_%s_Var%02d"), *AssetStem, ImportedModelIndex + 1);
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
          Task->Options = MakeStaticMeshImportOptions();
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
            }
          }
          ImportedModelIndex += 1;
        }
      }
    }

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
        const bool bAllowPlantExtras =
            bIsPlant &&
            (SlotName == TEXT("roughness") || SlotName == TEXT("translucency"));
        const bool bAllow3DSlots =
            SlotName == TEXT("albedo") || SlotName == TEXT("normal") ||
            SlotName == TEXT("fuzz") || SlotName == TEXT("mask") ||
            (SlotName == TEXT("displacement") && bAllowDisplacementSlot) ||
            bAllowPlantExtras;
        const bool bAllowDecalSlots =
            SlotName == TEXT("albedo") || SlotName == TEXT("normal");
        const bool bAllowHdriSlots = SlotName == TEXT("hdr");
        if ((bIsHdri && !bAllowHdriSlots) || (bIsDecal && !bAllowDecalSlots) ||
            (!bIsHdri && !bIsDecal && !bAllow3DSlots)) {
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
        Task->DestinationPath = AssetFolder;
        Task->DestinationName =
            bMultipleTextureGroups
                ? FString::Printf(TEXT("T_%s_%03d_%s"), *AssetStem, GroupId,
                                  *ToSlotSuffix(SlotName))
                : FString::Printf(TEXT("T_%s_%s"), *AssetStem,
                                  *ToSlotSuffix(SlotName));
        Task->bReplaceExisting = true;
        Task->bAutomated = true;
        Task->bAsync = false;
        Task->bSave = false;

        if (SlotName == TEXT("displacement")) {
          UTextureFactory *Factory = NewObject<UTextureFactory>();
          Factory->CompressionSettings = TC_Displacementmap;
          Factory->ColorSpaceMode = ETextureSourceColorSpace::Linear;
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
            if (SlotName == TEXT("albedo")) {
              Texture->CompressionSettings = TC_Default;
              Texture->SRGB = true;
              Texture->MipGenSettings = TMGS_Sharpen7;
            } else if (SlotName == TEXT("normal")) {
              Texture->CompressionSettings = TC_Normalmap;
              Texture->SRGB = false;
              Texture->MipGenSettings = TMGS_Sharpen4;
            } else if (SlotName == TEXT("roughness") ||
                       SlotName == TEXT("translucency")) {
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
            // Root-cause fix: the texture's platform data is built
            // asynchronously by the DDC. Force it synchronously and persist the
            // package so the material samples valid pixels on the first import
            // instead of showing the default "missing" texture.
            ForceTextureDataReady(Cast<UTexture2D>(Texture));
            SaveAssetPackage(Texture);
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
    GroupIds.Sort();
    const bool bUseVT = IsAssetVirtualTextureImportEnabled();
    TArray<UMaterialInstanceConstant *> MaterialInstances;
    for (const int32 GroupId : GroupIds) {
      const TMap<FString, UTexture *> &TextureBySlot =
          TextureBySlotByGroup.FindOrAdd(GroupId);
      const TMap<FString, FString> &SourceTextureBySlot =
          SourceTextureBySlotByGroup.FindOrAdd(GroupId);
      const FString GroupStem =
          GroupIds.Num() > 1
              ? FString::Printf(TEXT("%s_%03d"), *AssetStem, GroupId)
              : AssetStem;
      UMaterialInstanceConstant *MaterialInstance = nullptr;
      if (bIsHdri) {
        continue;
      } else if (bIsDecal) {
        SetStageProgress(
            static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
            FString::Printf(TEXT("合成 DRO 贴图: %s"), *AssetName));
        UTexture2D *DisplacementSourceTexture =
            SourceTextureBySlot.Contains(TEXT("displacement"))
                ? FImageUtils::ImportFileAsTexture2D(
                      SourceTextureBySlot[TEXT("displacement")])
                : nullptr;
        UTexture2D *RoughnessSourceTexture =
            SourceTextureBySlot.Contains(TEXT("roughness"))
                ? FImageUtils::ImportFileAsTexture2D(
                      SourceTextureBySlot[TEXT("roughness")])
                : nullptr;
        UTexture2D *OpacitySourceTexture =
            SourceTextureBySlot.Contains(TEXT("opacity"))
                ? FImageUtils::ImportFileAsTexture2D(
                      SourceTextureBySlot[TEXT("opacity")])
                : nullptr;
        UTexture2D *DROTexture = CreatePackedDROTexture(
            AssetFolder, GroupStem, DisplacementSourceTexture, 0,
            RoughnessSourceTexture, 0, OpacitySourceTexture, 0,
            Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("albedo"))),
            Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("normal"))));
        MaterialInstance = CreateDecalMaterialInstance(
            AssetFolder, GroupStem, TextureBySlot.FindRef(TEXT("albedo")),
            TextureBySlot.FindRef(TEXT("normal")), DROTexture, bUseVT);
      } else {
        if (AssetType == TEXT("3dplant")) {
          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 22, 0, 99)),
              FString::Printf(TEXT("合成 Albedo+Opacity 贴图: %s"),
                              *AssetName));
          UTexture2D *AlbedoSourceTexture =
              SourceTextureBySlot.Contains(TEXT("albedo"))
                  ? FImageUtils::ImportFileAsTexture2D(
                        SourceTextureBySlot[TEXT("albedo")])
                  : nullptr;
          UTexture2D *OpacitySourceTexture =
              SourceTextureBySlot.Contains(TEXT("opacity"))
                  ? FImageUtils::ImportFileAsTexture2D(
                        SourceTextureBySlot[TEXT("opacity")])
                  : nullptr;
          UTexture2D *PlantAlbedoTexture = CreatePackedPlantAlbedoTexture(
              AssetFolder, GroupStem, AlbedoSourceTexture,
              OpacitySourceTexture);

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
              FString::Printf(TEXT("合成 NRS 贴图: %s"), *AssetName));
          UTexture2D *NormalSourceTexture =
              SourceTextureBySlot.Contains(TEXT("normal"))
                  ? FImageUtils::ImportFileAsTexture2D(
                        SourceTextureBySlot[TEXT("normal")])
                  : nullptr;
          UTexture2D *RoughnessSourceTexture =
              SourceTextureBySlot.Contains(TEXT("roughness"))
                  ? FImageUtils::ImportFileAsTexture2D(
                        SourceTextureBySlot[TEXT("roughness")])
                  : nullptr;
          UTexture2D *TranslucencySourceTexture =
              SourceTextureBySlot.Contains(TEXT("translucency"))
                  ? FImageUtils::ImportFileAsTexture2D(
                        SourceTextureBySlot[TEXT("translucency")])
                  : nullptr;
          UTexture2D *NRSTexture = CreatePackedNRSTexture(
              AssetFolder, GroupStem, NormalSourceTexture,
              RoughnessSourceTexture, TranslucencySourceTexture,
              PlantAlbedoTexture, NormalSourceTexture);
          MaterialInstance = CreateGrassMaterialInstance(
              AssetFolder, GroupStem, PlantAlbedoTexture, NRSTexture, bUseVT);
        } else {
          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 25, 0, 99)),
              FString::Printf(TEXT("Composite Textures: %s"), *AssetName));
          UTexture2D *MaskTexture =
              Cast<UTexture2D>(TextureBySlot.FindRef(TEXT("mask")));
          if (MaskTexture) {
            MaskTexture->CompressionSettings = TC_Masks;
            MaskTexture->CompressionNoAlpha = true;
            MaskTexture->SRGB = false;
            MaskTexture->PostEditChange();
            MaskTexture->MarkPackageDirty();
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
                AssetFolder, GroupStem, AOTexture, AOChannel, RoughnessTexture,
                RoughnessChannel, DisplacementTexture, DisplacementChannel,
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
      }
    }
    for (UStaticMesh *StaticMesh : ImportedMeshes) {
      if (!StaticMesh) {
        continue;
      }
      StaticMesh->NaniteSettings.bEnabled = true;
      if (MaterialInstances.Num() > 0) {
        const int32 SlotCount =
            FMath::Max(1, StaticMesh->GetStaticMaterials().Num());
        for (int32 Index = 0; Index < SlotCount; Index++) {
          const int32 MaterialIndex =
              FMath::Min(Index, MaterialInstances.Num() - 1);
          StaticMesh->SetMaterial(Index, MaterialInstances[MaterialIndex]);
        }
      }
      StaticMesh->PostEditChange();
      StaticMesh->MarkPackageDirty();
      SaveAssetPackage(StaticMesh);
    }

    if (OutImportedFolders && FAssetRegistryModule::GetRegistry().HasAssets(FName(*AssetFolder), true)) {
      OutImportedFolders->AddUnique(AssetFolder);
    }
    SetStageProgress(static_cast<float>(AssetEndProgress),
                     FString::Printf(TEXT("资产完成: %s"), *AssetName));
    AssetIndex++;
  }
  SetStageProgress(100.0f, TEXT("导入完成"));
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
  return GAssetHiveSaveFailed ? 1 : 0;
}
