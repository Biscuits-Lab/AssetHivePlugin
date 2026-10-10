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
      // Megascans 源缺少细分植被标签时，导出名使用 Foliage，
      // 与供应商名解耦（例如 SM_Env_Foliage_<资产ID>_01）。
      return NormalizeAssetTagToken(Tag) == TEXT("megascans")
                 ? FString(TEXT("Foliage"))
                 : SafeTag;
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
// ---------------------------------------------------------------------------
// 合成 SpeedTree 风（Megascans 3D Plants，2026-10-07）
//
// 项目材质 MF_Foliage_Wind_ST 的读取约定（材质侧已确认）：
//   Branch1：UV1=(PackedPosition, PackedDirection)，UV2.r=Weight（恒定读取）
//   Branch2：勾选 "Use Remapped UV3" 后读 UV3=(lo, hi)，HLSL 解码：
//     lo = round(UV.x * 16383); hi = round(UV.y * 16383);
//     xq = floor(hi / 16); yqHigh = fmod(hi, 16); yqLow = floor(lo / 256); wq = fmod(lo, 256);
//     yq = yqHigh * 64 + yqLow;
//     x = XMin + (xq / 1023) * XRange; y = YMin + (yq / 1023) * YRange; w = WMin + (wq / 255) * WRange;
//   其中 xMin/xMax/yMin/yMax/wMin/wMax 是材质实例标量参数——注意 xMax/yMax/wMax
//   承载的是 Range（范围）而不是最大值，必须与这里的编码范围保持一致。
//
// PackedPosition 按 UnpackInteger3（系数 6/6/7）解码为 (s1/5, q1/5, q0/6)，即归一化
//   锚点位置 x/y 各 6 档、z 7 档；编码时写回 (q0*36 + q1*6 + s1)/256。
// PackedDirection 按 UnpackDirection 解码为 normalize(frac(In/(16,1,0.0625))*2-1)，
//   In∈[0,16) 只有 1 个自由度，编码时枚举档位取最接近目标方向的解。
// SpeedTree 语义（SpeedTreeCommon.ush）：UV1.y 同时是摆动方向（UnpackNormalFromFloat(fOffset)
//   → vWindVector）与振荡相位（Oscillate 的 fOffset）。实测 ThickGrassTall 每片叶的 f 散布在
//   6.43..9.62（相位差最多 3.2 弧度），对应解码方向相对生长方向约 ±15°。草叶生长方向几乎一致，
//   若直接编码生长方向，整簇相位相同 -> 同步摆动（“太整体”）。Grass 因此对每片叶的编码值加
//   确定性扰动（JitterGrassWindDirection），Fern/Bush 保持原样。
// UV1/UV2 逐叶对齐 SpeedTree（2026-10-08）：
//   UV1 = (PackedPosition, PackedDirection)  —— PackedPosition 取叶片顶部（ST9 实测），
//     PackedDirection 取逐叶生长方向 + 相位扰动；
//   UV2 = (Branch1Weight, RippleWeight)      —— Weight 取整簇高度归一化（ST9 实测线性），
//     RippleWeight（UV2.g）按风单元锚点散列到 ST 实测幅度区间 0.60~0.90。
//   母材质 MF_SpeedTreeWind_cUSTOM 用 CoordinateIndex=2 的 G 通道缩放 ripple 项，
//   权重恒 0 时整丛只剩 shared/branch1 运动，视觉上就是“叶片粘住”。Grass 的相位锚点取单叶，
//   Fern/Bush 取叶片簇（同簇叶轴与子叶共享相位，避免接缝分离）。
// ---------------------------------------------------------------------------
enum class EPlantSyntheticWind : uint8 {
  None,
  // Megascans Grass：只写 branch1（UV1/UV2），branch2 关闭。
  Branch1,
  // Megascans Bush：branch1 + branch2 重映射到 UV3（Use Remapped UV3）。
  Branch1Branch2UV3,
  // Megascans 蕨类（Fern）：最多 branch1；所有的接触连通片（叶轴 + 全部子叶）强制合并
  // 为同一风单元，与 Grass 的“叶片簇判定”和 Bush 的不合并都不同。
  Fern,
};

namespace AssetHivePlantWind {
// 编码范围与材质实例参数一一对应（固定范围，跨资产共享，便于母材质复用）。
constexpr float PositionMin = 0.0f;
constexpr float PositionRange = 1.0f;
constexpr float DirectionMin = 0.0f;
constexpr float DirectionRange = 16.0f;
constexpr float WeightMin = 0.0f;
constexpr float WeightRange = 1.0f;

// UnpackInteger3 系数 6/6/7 → x/y 各 6 档、z 7 档；档位基数为 6。
constexpr int32 PositionStepsX = 5;
constexpr int32 PositionStepsY = 5;
constexpr int32 PositionStepsZ = 6;
constexpr int32 PositionLevelBase = 6;

// UV3 的分档：位置/方向 10bit，权重 8bit。
constexpr int32 UV3CodeMax = 1023;
constexpr int32 UV3WeightCodeMax = 255;

const TCHAR *const SwitchUseRemappedUV3 = TEXT("Use Remapped UV3");
const TCHAR *const SwitchBranch1Enable = TEXT("Branch1Enable");
const TCHAR *const SwitchBranch2Enable = TEXT("Branch2Enable");
const TCHAR *const SwitchHasBranch2Data = TEXT("HasBranch2Data_Internal");
const TCHAR *const ScalarPositionMin = TEXT("xMin");
const TCHAR *const ScalarPositionRange = TEXT("xMax");
const TCHAR *const ScalarDirectionMin = TEXT("yMin");
const TCHAR *const ScalarDirectionRange = TEXT("yMax");
const TCHAR *const ScalarWeightMin = TEXT("wMin");
const TCHAR *const ScalarWeightRange = TEXT("wMax");
// UV2.g = RippleWeight。SpeedTree 原生把 ripple 权重写在 UV2.g
// （Engine 侧 SpeedTreeImportFactory.cpp:3069 -> UV2 = (BranchWind1.z, RippleWeight)）；
// 本项目母材质 MF_SpeedTreeWind_cUSTOM 用 CoordinateIndex=2 的 G 通道缩放 ripple 项
// （Multiply_19 / Multiply_7）。权重恒 0 时整丛只剩 shared/branch1 运动，叶片就会"粘住"。
const TCHAR *const SwitchRippleEnable = TEXT("RippleEnable");
// 基础风力标量：由材质实例直接写入，不再依赖母材质默认值与 TA 手填；
// 与项目 GrassBend / MF_SpeedTreeWind_cUSTOM 的参数名逐字对应。
const TCHAR *const ScalarBranch1Bend = TEXT("Branch1Bend");
const TCHAR *const ScalarBranch1Oscillation = TEXT("Branch1Oscillation");
const TCHAR *const ScalarBranch1StretchLimit = TEXT("Branch1StretchLimit");
const TCHAR *const ScalarSharedBend = TEXT("SharedBend");
const TCHAR *const ScalarSharedOscillation = TEXT("SharedOscillation");
constexpr float DefaultBranch1Bend = 2.0f;
constexpr float DefaultBranch1Oscillation = 20.0f;
constexpr float DefaultBranch1StretchLimit = 0.5f;
constexpr float DefaultSharedBend = 10.0f;
constexpr float DefaultSharedOscillation = 35.0f;
// 合成 ripple 幅度区间：对齐 SpeedTree 实测（ThickGrassTall 每片叶 0.54~0.86、均值 0.73；
// Grass3/3_001 短草 0.24~0.50），取草类高段并按锚点散列拉开每片叶的幅度。
constexpr float GrassRippleWeightMin = 0.60f;
constexpr float GrassRippleWeightMax = 0.90f;

// 复刻材质侧 frac()（x - floor(x)）。
inline float MaterialFrac(float Value) {
  return Value - FMath::FloorToFloat(Value);
}

inline FVector3f DecodeDirection(float PackedDirection) {
  FVector3f Direction(MaterialFrac(PackedDirection / 16.0f) * 2.0f - 1.0f,
                      MaterialFrac(PackedDirection) * 2.0f - 1.0f,
                      MaterialFrac(PackedDirection * 16.0f) * 2.0f - 1.0f);
  if (!Direction.Normalize()) {
    return FVector3f(0.0f, 0.0f, 1.0f);
  }
  return Direction;
}

// bQuantized=true：只能落在 UV3 的 10bit 档位（branch2）；
// bQuantized=false：UV1.y 是连续浮点，再细化一次搜索（branch1）。
inline float PackDirection(const FVector3f &Direction, bool bQuantized) {
  const FVector3f Target = Direction.GetSafeNormal();
  if (Target.IsNearlyZero()) {
    return 0.0f;
  }
  int32 BestCode = 0;
  float BestDot = -1.0f;
  for (int32 Code = 0; Code <= UV3CodeMax; ++Code) {
    const float Candidate =
        DirectionRange * static_cast<float>(Code) / static_cast<float>(UV3CodeMax);
    const float Dot = FVector3f::DotProduct(DecodeDirection(Candidate), Target);
    if (Dot > BestDot) {
      BestDot = Dot;
      BestCode = Code;
    }
  }
  float Packed =
      DirectionRange * static_cast<float>(BestCode) / static_cast<float>(UV3CodeMax);
  if (bQuantized) {
    return Packed;
  }
  const float Step = DirectionRange / static_cast<float>(UV3CodeMax);
  constexpr int32 RefineSteps = 32;
  for (int32 Index = -RefineSteps; Index <= RefineSteps; ++Index) {
    const float Candidate = FMath::Clamp(
        Packed + Step * static_cast<float>(Index) / RefineSteps, 0.0f,
        DirectionRange);
    const float Dot = FVector3f::DotProduct(DecodeDirection(Candidate), Target);
    if (Dot > BestDot) {
      BestDot = Dot;
      Packed = Candidate;
    }
  }
  return Packed;
}

// 锚点位置 → 32bit 哈希（FNV-1a）。位置量化到 0.01cm，保证同一资产重复导入得到同一相位，
// 裁切网格也能用卡片锚点取回与未裁切网格一致的相位。
// 接触带混合后的方向打包：从原打包值出发做「由粗到细」的局部爬山搜索，
// 避免 PackDirection 全码表扫描（1024+65 次解码）在逐顶点调用时的开销。
inline float PackDirectionNear(const FVector3f &Direction, float Hint) {
  const FVector3f Target = Direction.GetSafeNormal();
  if (Target.IsNearlyZero()) {
    return Hint;
  }
  int32 CenterCode = FMath::Clamp(
      FMath::RoundToInt(Hint / DirectionRange * static_cast<float>(UV3CodeMax)), 0,
      UV3CodeMax);
  float Best = DirectionRange * static_cast<float>(CenterCode) /
               static_cast<float>(UV3CodeMax);
  float BestDot = FVector3f::DotProduct(DecodeDirection(Best), Target);
  static const int32 SearchSteps[] = {64, 16, 4, 1};
  for (const int32 Step : SearchSteps) {
    for (int32 Iteration = 0; Iteration < 8; ++Iteration) {
      int32 NextCode = CenterCode;
      for (const int32 DirectionSign : {-1, 1}) {
        const int32 Code =
            FMath::Clamp(CenterCode + DirectionSign * Step, 0, UV3CodeMax);
        if (Code == CenterCode) {
          continue;
        }
        const float Candidate = DirectionRange * static_cast<float>(Code) /
                                static_cast<float>(UV3CodeMax);
        const float Dot =
            FVector3f::DotProduct(DecodeDirection(Candidate), Target);
        if (Dot > BestDot) {
          BestDot = Dot;
          NextCode = Code;
        }
      }
      if (NextCode == CenterCode) {
        break;
      }
      CenterCode = NextCode;
      Best = DirectionRange * static_cast<float>(CenterCode) /
             static_cast<float>(UV3CodeMax);
    }
  }
  return Best;
}

inline uint32 HashPlantWindAnchor(const FVector3f &Anchor) {
  const uint32 Values[3] = {
      static_cast<uint32>(FMath::RoundToInt(Anchor.X * 100.0f)),
      static_cast<uint32>(FMath::RoundToInt(Anchor.Y * 100.0f)),
      static_cast<uint32>(FMath::RoundToInt(Anchor.Z * 100.0f))};
  uint32 Hash = 2166136261u;
  for (const uint32 Value : Values) {
    for (int32 Shift = 0; Shift < 32; Shift += 8) {
      Hash = (Hash ^ ((Value >> Shift) & 0xFFu)) * 16777619u;
    }
  }
  return Hash;
}

inline float HashToUnitFloat(uint32 Hash) {
  return static_cast<float>(Hash & 0x00FFFFFFu) /
         static_cast<float>(0x01000000u);
}

// UV2.g（RippleWeight）合成值：按风单元锚点确定性散列，落在 SpeedTree 实测区间，
// 使同一片草丛里每片叶的 ripple 幅度不同（SpeedTree 是逐叶烘焙的 per-frond 值）。
inline float PackGrassRippleWeight(const FVector3f &Anchor) {
  return FMath::Lerp(GrassRippleWeightMin, GrassRippleWeightMax,
                     HashToUnitFloat(HashPlantWindAnchor(Anchor) ^ 0x9E3779B9u));
}

// Grass 叶片相位抖动：SpeedTree 的 UV1.y 同时决定摆动方向与振荡相位，草叶生长方向一致时
// 必须靠它错开，否则整簇同步摆动。在生长方向的正交平面内取确定性随机偏移（倾角 <=18°），
// 复现实测 ThickGrassTall 每片叶 f 值 6.43..9.62 的散布；方向仍以生长方向为主，不会乱弯。
inline FVector3f JitterGrassWindDirection(const FVector3f &Anchor,
                                          const FVector3f &Direction) {
  const FVector3f Axis = Direction.GetSafeNormal();
  if (Axis.IsNearlyZero()) {
    return Direction;
  }
  const uint32 Hash = HashPlantWindAnchor(Anchor);
  FVector3f Side = FVector3f::ZeroVector;
  FVector3f Up = FVector3f::ZeroVector;
  Axis.FindBestAxisVectors(Side, Up);
  // 正交平面内两个独立分量：复现 SpeedTree x/y 各自成档的散布，比单一旋转角覆盖更多相位档位。
  constexpr float GrassWindJitterScale = 0.22f;
  const float OffsetX =
      (HashToUnitFloat(Hash) * 2.0f - 1.0f) * GrassWindJitterScale;
  const float OffsetY =
      (HashToUnitFloat(Hash * 2654435761u) * 2.0f - 1.0f) * GrassWindJitterScale;
  const FVector3f Offset = Side * OffsetX + Up * OffsetY;
  const FVector3f Jittered = (Axis + Offset).GetSafeNormal();
  return Jittered.IsNearlyZero() ? Direction : Jittered;
}

// 归一化锚点位置 → UnpackInteger3 的打包值（取值 0..251/256）。
inline float PackPosition(const FVector3f &NormalizedPosition) {
  const int32 LevelZ = FMath::Clamp(
      FMath::RoundToInt(NormalizedPosition.Z * static_cast<float>(PositionStepsZ)), 0,
      PositionStepsZ);
  const int32 LevelY = FMath::Clamp(
      FMath::RoundToInt(NormalizedPosition.Y * static_cast<float>(PositionStepsY)), 0,
      PositionStepsY);
  const int32 LevelX = FMath::Clamp(
      FMath::RoundToInt(NormalizedPosition.X * static_cast<float>(PositionStepsX)), 0,
      PositionStepsX);
  const int32 Packed = LevelZ * PositionLevelBase * PositionLevelBase +
                       LevelY * PositionLevelBase + LevelX;
  return static_cast<float>(Packed) / 256.0f;
}

inline int32 ToUV3Code(float Value, float Min, float Range, int32 CodeMax) {
  if (Range <= KINDA_SMALL_NUMBER) {
    return 0;
  }
  const float Normalized = (Value - Min) / Range;
  return FMath::Clamp(FMath::RoundToInt(Normalized * static_cast<float>(CodeMax)), 0,
                      CodeMax);
}

// UV3.x = lo：方向低 6 位在高 8 位、权重在低 8 位；
// UV3.y = hi：位置 10 位在高 10 位、方向高 4 位在低 4 位。
inline FVector2f EncodeUV3(int32 PositionCode, int32 DirectionCode, int32 WeightCode) {
  const int32 Low = (DirectionCode % 64) * 256 + WeightCode;
  const int32 High = PositionCode * 16 + (DirectionCode / 64);
  return FVector2f(static_cast<float>(Low) / 16383.0f,
                   static_cast<float>(High) / 16383.0f);
}

// 位置焊接：把距离 <= WeldEpsilon 的重复顶点并入同一并查集组，返回被合并的顶点数量。
// 只用于连通片判定（合成风按片写数据），不修改网格顶点；共享顶点已正常的网格不会有任何变化。
// 逐角拆分的网格（顶点数 = 角数）在这里被还原成按位置连通的片，避免“每三角形一片”导致顶点撕裂。
template <typename TGetPosition>
inline int32 WeldCoincidentVertices(int32 VertexCount, TGetPosition &&GetPosition,
                                   TArray<int32> &Parents, float WeldEpsilon) {
  if (VertexCount <= 0 || WeldEpsilon <= 0.0f) {
    return 0;
  }
  auto FindRoot = [&Parents](int32 Index) {
    while (Parents[Index] != Index) {
      Parents[Index] = Parents[Parents[Index]];
      Index = Parents[Index];
    }
    return Index;
  };
  const float WeldEpsilonSq = WeldEpsilon * WeldEpsilon;
  const float CellSize = WeldEpsilon;
  TMap<FIntVector, TArray<int32>> Grid;
  Grid.Reserve(VertexCount);
  int32 MergedVertices = 0;
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const FVector3f Position = GetPosition(VertexIndex);
    const FIntVector Key(FMath::FloorToInt(Position.X / CellSize),
                         FMath::FloorToInt(Position.Y / CellSize),
                         FMath::FloorToInt(Position.Z / CellSize));
    bool bWelded = false;
    for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
      for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
        for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
          const TArray<int32> *Candidates =
              Grid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
          if (!Candidates) {
            continue;
          }
          for (const int32 Candidate : *Candidates) {
            if (FVector3f::DistSquared(Position, GetPosition(Candidate)) >
                WeldEpsilonSq) {
              continue;
            }
            const int32 RootA = FindRoot(VertexIndex);
            const int32 RootB = FindRoot(Candidate);
            if (RootA != RootB) {
              Parents[RootB] = RootA;
            }
            bWelded = true;
          }
        }
      }
    }
    if (bWelded) {
      MergedVertices += 1;
    } else {
      Grid.FindOrAdd(Key).Add(VertexIndex);
    }
  }
  return MergedVertices;
}
// --- 合成风的“原始卡片”参考（2026-10-07）-------------------------------------
// 不透明裁切（_OPAQUE）把原始卡片切成大量毫米级碎片：碎片自身的投影跨度极小，若按碎片
// 归一化，权重会在碎片内瞬间走完 0..1，整片被拉成条状（视觉上的顶点撕裂）。因此裁切网格
// 必须回到未裁切的原始卡片网格，用“卡片级”的锚点/方向/投影推导权重。
struct FPlantWindCard {
  FVector3f BasePosition = FVector3f::ZeroVector;
  FVector3f Direction = FVector3f(0.0f, 0.0f, 1.0f);
  // UV1.x（PackedPosition）用：SpeedTree 实测编码的是叶片“顶部”位置，不是基部。
  FVector3f WindPosition = FVector3f::ZeroVector;
  float MaxProjection = 1.0f;
};

// 原始卡片网格的最近点查询结构（均匀网格 + 点到三角形最近距离）。
// 裁切网格与原始卡片同处一个局部空间，裁切顶点必定落在卡片表面上，最近三角形距离≈0，
// 因此“最近三角形 -> 卡片”的映射是稳定且无歧义的（重叠卡片处只在距离同为 0 时才需取舍）。
struct FPlantWindCardSurface {
  TArray<FVector3f> Positions;
  TArray<uint32> TriangleIndices;  // 每个三角形 3 个顶点索引
  TArray<int32> CardOfTriangle;
  FVector3f BoundsMin = FVector3f::ZeroVector;
  FVector3f BoundsMax = FVector3f::ZeroVector;
  float CellSize = 1.0f;
  float CoarseCellSize = 1.0f;
  TMap<FIntVector, TArray<int32>> TriangleGrid;
  // 二级粗网格（对角线 / 12）：细网格 8 环内未命中时的兜底（覆盖半径约对角线 / 3）。
  TMap<FIntVector, TArray<int32>> CoarseTriangleGrid;
  // UV0 反查（opaque 迁移，2026-10-07）：不透明裁切网格完整继承原始卡片 UV0（实测命中
  // 100%），用「UV 候选 + 3D 位置消歧」确定卡片归属，可以区分间距仅 0.3~0.9mm 的贴合
  // 双层卡片——纯 3D 最近三角形在这些位置会选到相邻层，造成相邻碎片两套参数（WPO 撕裂）。
  TArray<FVector2f> TriangleUVs;  // 每个三角形 3 个顶点的 UV0
  TMap<FIntPoint, TArray<int32>> UVTriangleGrid;
  FVector2f UVBoundsMin = FVector2f::ZeroVector;
  FVector2f UVBoundsMax = FVector2f::ZeroVector;
  float UVCellSize = 1.0f;

  bool IsValid() const { return TriangleIndices.Num() >= 3; }
  int32 NumTriangles() const { return TriangleIndices.Num() / 3; }
  float GetDiagonal() const { return (BoundsMax - BoundsMin).Size(); }

  FIntVector GetCellKeyFor(const FVector3f &Position, float InCellSize) const {
    return FIntVector(
        FMath::FloorToInt((Position.X - BoundsMin.X) / InCellSize),
        FMath::FloorToInt((Position.Y - BoundsMin.Y) / InCellSize),
        FMath::FloorToInt((Position.Z - BoundsMin.Z) / InCellSize));
  }

  FIntVector GetCellKey(const FVector3f &Position) const {
    return GetCellKeyFor(Position, CellSize);
  }

  void BuildGrid() {
    TriangleGrid.Reset();
    CoarseTriangleGrid.Reset();
    if (Positions.Num() == 0 || NumTriangles() == 0) {
      return;
    }
    BoundsMin = FVector3f(TNumericLimits<float>::Max());
    BoundsMax = FVector3f(-TNumericLimits<float>::Max());
    for (const FVector3f &Position : Positions) {
      for (int32 Axis = 0; Axis < 3; ++Axis) {
        BoundsMin[Axis] = FMath::Min(BoundsMin[Axis], Position[Axis]);
        BoundsMax[Axis] = FMath::Max(BoundsMax[Axis], Position[Axis]);
      }
    }
    // 网格分辨率与离线验证一致（对角线 / 96）：裁切顶点基本都在 1 环内命中。
    // 另建对角线 / 12 的粗网格作为兜底（4 环覆盖半径约对角线 / 3）。
    constexpr int32 CellsPerAxis = 96;
    constexpr int32 CoarseCellsPerAxis = 12;
    const float Diagonal = (BoundsMax - BoundsMin).Size();
    CellSize = FMath::Max(Diagonal / static_cast<float>(CellsPerAxis), 1e-3f);
    CoarseCellSize = FMath::Max(Diagonal / static_cast<float>(CoarseCellsPerAxis), 1e-3f);
    TriangleGrid.Reserve(NumTriangles());
    CoarseTriangleGrid.Reserve(NumTriangles());
    for (int32 Triangle = 0; Triangle < NumTriangles(); ++Triangle) {
      FVector3f TriangleMin(TNumericLimits<float>::Max());
      FVector3f TriangleMax(-TNumericLimits<float>::Max());
      for (int32 Corner = 0; Corner < 3; ++Corner) {
        const FVector3f &Position =
            Positions[TriangleIndices[Triangle * 3 + Corner]];
        for (int32 Axis = 0; Axis < 3; ++Axis) {
          TriangleMin[Axis] = FMath::Min(TriangleMin[Axis], Position[Axis]);
          TriangleMax[Axis] = FMath::Max(TriangleMax[Axis], Position[Axis]);
        }
      }
      const FIntVector MinCell = GetCellKey(TriangleMin);
      const FIntVector MaxCell = GetCellKey(TriangleMax);
      for (int32 X = MinCell.X; X <= MaxCell.X; ++X) {
        for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y) {
          for (int32 Z = MinCell.Z; Z <= MaxCell.Z; ++Z) {
            TriangleGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(Triangle);
          }
        }
      }
      const FIntVector CoarseMinCell = GetCellKeyFor(TriangleMin, CoarseCellSize);
      const FIntVector CoarseMaxCell = GetCellKeyFor(TriangleMax, CoarseCellSize);
      for (int32 X = CoarseMinCell.X; X <= CoarseMaxCell.X; ++X) {
        for (int32 Y = CoarseMinCell.Y; Y <= CoarseMaxCell.Y; ++Y) {
          for (int32 Z = CoarseMinCell.Z; Z <= CoarseMaxCell.Z; ++Z) {
            CoarseTriangleGrid.FindOrAdd(FIntVector(X, Y, Z)).Add(Triangle);
          }
        }
      }
    }
  }
  FIntPoint GetUVKeyFor(const FVector2f &UV, float InCellSize) const {
    return FIntPoint(FMath::FloorToInt((UV.X - UVBoundsMin.X) / InCellSize),
                     FMath::FloorToInt((UV.Y - UVBoundsMin.Y) / InCellSize));
  }

  FIntPoint GetUVKey(const FVector2f &UV) const {
    return GetUVKeyFor(UV, UVCellSize);
  }

  // UV 空间均匀网格：每个三角形登记到它 UV 包围盒覆盖的所有格子。
  void BuildUVGrid() {
    UVTriangleGrid.Reset();
    if (NumTriangles() == 0 || TriangleUVs.Num() != NumTriangles() * 3) {
      return;
    }
    UVBoundsMin = FVector2f(TNumericLimits<float>::Max());
    UVBoundsMax = FVector2f(-TNumericLimits<float>::Max());
    for (const FVector2f &UV : TriangleUVs) {
      UVBoundsMin.X = FMath::Min(UVBoundsMin.X, UV.X);
      UVBoundsMin.Y = FMath::Min(UVBoundsMin.Y, UV.Y);
      UVBoundsMax.X = FMath::Max(UVBoundsMax.X, UV.X);
      UVBoundsMax.Y = FMath::Max(UVBoundsMax.Y, UV.Y);
    }
    constexpr int32 UVCellsPerAxis = 128;
    const float Diagonal = (UVBoundsMax - UVBoundsMin).Size();
    UVCellSize = FMath::Max(Diagonal / static_cast<float>(UVCellsPerAxis), 1e-4f);
    UVTriangleGrid.Reserve(NumTriangles());
    for (int32 Triangle = 0; Triangle < NumTriangles(); ++Triangle) {
      FVector2f TriangleMin(TNumericLimits<float>::Max());
      FVector2f TriangleMax(-TNumericLimits<float>::Max());
      for (int32 Corner = 0; Corner < 3; ++Corner) {
        const FVector2f &UV = TriangleUVs[Triangle * 3 + Corner];
        TriangleMin.X = FMath::Min(TriangleMin.X, UV.X);
        TriangleMin.Y = FMath::Min(TriangleMin.Y, UV.Y);
        TriangleMax.X = FMath::Max(TriangleMax.X, UV.X);
        TriangleMax.Y = FMath::Max(TriangleMax.Y, UV.Y);
      }
      const FIntPoint MinCell = GetUVKey(TriangleMin);
      const FIntPoint MaxCell = GetUVKey(TriangleMax);
      const int64 CellCount = static_cast<int64>(MaxCell.X - MinCell.X + 1) *
                              static_cast<int64>(MaxCell.Y - MinCell.Y + 1);
      if (CellCount <= 4096) {
        for (int32 X = MinCell.X; X <= MaxCell.X; ++X) {
          for (int32 Y = MinCell.Y; Y <= MaxCell.Y; ++Y) {
            UVTriangleGrid.FindOrAdd(FIntPoint(X, Y)).Add(Triangle);
          }
        }
      } else {
        // 异常 UV（整图三角形/退化 UV）：只登记角点与中心，避免网格条目爆炸。
        for (int32 Corner = 0; Corner < 3; ++Corner) {
          UVTriangleGrid
              .FindOrAdd(GetUVKey(TriangleUVs[Triangle * 3 + Corner]))
              .AddUnique(Triangle);
        }
        UVTriangleGrid.FindOrAdd(GetUVKey((TriangleMin + TriangleMax) * 0.5f))
            .AddUnique(Triangle);
      }
    }
  }

  // 点是否落在三角形的 UV 区域内（重心坐标，带容差）。
  bool IsInsideTriangleUV(const FVector2f &UV, int32 Triangle) const {
    const FVector2f &A = TriangleUVs[Triangle * 3 + 0];
    const FVector2f &B = TriangleUVs[Triangle * 3 + 1];
    const FVector2f &C = TriangleUVs[Triangle * 3 + 2];
    const float Denominator = (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
    if (FMath::Abs(Denominator) < 1e-9f) {
      return false;
    }
    const FVector2f AP = UV - A;
    const float WeightB = (AP.X * (C.Y - A.Y) - AP.Y * (C.X - A.X)) / Denominator;
    const float WeightC = ((B.X - A.X) * AP.Y - (B.Y - A.Y) * AP.X) / Denominator;
    constexpr float Tolerance = 1e-3f;
    return WeightB >= -Tolerance && WeightC >= -Tolerance &&
           (WeightB + WeightC) <= 1.0f + Tolerance;
  }

  // UV0 反查卡片：UV 候选内取 3D 最近三角形（贴合双层卡片靠 UV 区域区分，靠 3D 距离
  // 在候选中消歧）。返回卡片索引；无 UV 网格或 UV 未命中时返回 INDEX_NONE，调用方回退
  // 到 FindNearestTriangle 的 3D 最近三角形。
  int32 FindCardByUV(const FVector2f &UV, const FVector3f &Point,
                     float &OutDistance) const {
    OutDistance = TNumericLimits<float>::Max();
    if (UVTriangleGrid.Num() == 0 || NumTriangles() == 0) {
      return INDEX_NONE;
    }
    const TArray<int32> *Bucket = UVTriangleGrid.Find(GetUVKey(UV));
    if (!Bucket) {
      return INDEX_NONE;
    }
    int32 BestCard = INDEX_NONE;
    float BestDistanceSquared = TNumericLimits<float>::Max();
    for (const int32 Triangle : *Bucket) {
      if (!IsInsideTriangleUV(UV, Triangle)) {
        continue;
      }
      const float DistanceSquared = PointTriangleDistanceSquared(Point, Triangle);
      if (DistanceSquared < BestDistanceSquared) {
        BestDistanceSquared = DistanceSquared;
        BestCard = CardOfTriangle[Triangle];
      }
    }
    if (BestCard != INDEX_NONE) {
      OutDistance = FMath::Sqrt(BestDistanceSquared);
    }
    return BestCard;
  }

  // 点到三角形的最近距离平方（Ericson, Real-Time Collision Detection）。
  float PointTriangleDistanceSquared(const FVector3f &Point, int32 Triangle) const {
    const FVector3f &A = Positions[TriangleIndices[Triangle * 3 + 0]];
    const FVector3f &B = Positions[TriangleIndices[Triangle * 3 + 1]];
    const FVector3f &C = Positions[TriangleIndices[Triangle * 3 + 2]];
    const FVector3f AB = B - A;
    const FVector3f AC = C - A;
    const FVector3f AP = Point - A;
    const float D1 = FVector3f::DotProduct(AB, AP);
    const float D2 = FVector3f::DotProduct(AC, AP);
    if (D1 <= 0.0f && D2 <= 0.0f) {
      return FVector3f::DistSquared(Point, A);
    }
    const FVector3f BP = Point - B;
    const float D3 = FVector3f::DotProduct(AB, BP);
    const float D4 = FVector3f::DotProduct(AC, BP);
    if (D3 >= 0.0f && D4 <= D3) {
      return FVector3f::DistSquared(Point, B);
    }
    const float VC = D1 * D4 - D3 * D2;
    if (VC <= 0.0f && D1 >= 0.0f && D3 <= 0.0f) {
      const float Denominator = D1 - D3;
      const float T = FMath::IsNearlyZero(Denominator) ? 0.0f : D1 / Denominator;
      return FVector3f::DistSquared(Point, A + AB * T);
    }
    const FVector3f CP = Point - C;
    const float D5 = FVector3f::DotProduct(AB, CP);
    const float D6 = FVector3f::DotProduct(AC, CP);
    if (D6 >= 0.0f && D5 <= D6) {
      return FVector3f::DistSquared(Point, C);
    }
    const float VB = D5 * D2 - D1 * D6;
    if (VB <= 0.0f && D2 >= 0.0f && D6 <= 0.0f) {
      const float Denominator = D2 - D6;
      const float T = FMath::IsNearlyZero(Denominator) ? 0.0f : D2 / Denominator;
      return FVector3f::DistSquared(Point, A + AC * T);
    }
    const float VA = D3 * D6 - D5 * D4;
    if (VA <= 0.0f && (D4 - D3) >= 0.0f && (D5 - D6) >= 0.0f) {
      const float Denominator = (D4 - D3) + (D5 - D6);
      const float T = FMath::IsNearlyZero(Denominator) ? 0.0f : (D4 - D3) / Denominator;
      return FVector3f::DistSquared(Point, B + (C - B) * T);
    }
    const float Denominator = VA + VB + VC;
    if (FMath::IsNearlyZero(Denominator)) {
      return FVector3f::DistSquared(Point, A);
    }
    const float V = VB / Denominator;
    const float W = VC / Denominator;
    return FVector3f::DistSquared(Point, A + AB * V + AC * W);
  }

  // 单层网格的环形最近三角形搜索（找到且距离 <= 环半径即提前结束）。
  int32 FindNearestTriangleInGrid(const FVector3f &Point,
                                  const TMap<FIntVector, TArray<int32>> &Grid,
                                  float InCellSize, int32 MaxRings,
                                  float &InOutBestDistanceSquared) const {
    int32 BestTriangle = INDEX_NONE;
    float BestDistanceSquared = InOutBestDistanceSquared;
    if (Grid.Num() == 0) {
      return INDEX_NONE;
    }
    const FIntVector Center = GetCellKeyFor(Point, InCellSize);
    for (int32 Ring = 0; Ring <= MaxRings; ++Ring) {
      for (int32 X = Center.X - Ring; X <= Center.X + Ring; ++X) {
        for (int32 Y = Center.Y - Ring; Y <= Center.Y + Ring; ++Y) {
          for (int32 Z = Center.Z - Ring; Z <= Center.Z + Ring; ++Z) {
            if (Ring > 0 && FMath::Abs(X - Center.X) != Ring &&
                FMath::Abs(Y - Center.Y) != Ring &&
                FMath::Abs(Z - Center.Z) != Ring) {
              continue;
            }
            const TArray<int32> *Bucket = Grid.Find(FIntVector(X, Y, Z));
            if (!Bucket) {
              continue;
            }
            for (const int32 Triangle : *Bucket) {
              const float DistanceSquared =
                  PointTriangleDistanceSquared(Point, Triangle);
              if (DistanceSquared < BestDistanceSquared) {
                BestDistanceSquared = DistanceSquared;
                BestTriangle = Triangle;
              }
            }
          }
        }
      }
      if (BestTriangle != INDEX_NONE &&
          BestDistanceSquared <= FMath::Square(Ring * InCellSize)) {
        break;
      }
    }
    InOutBestDistanceSquared = BestDistanceSquared;
    return BestTriangle;
  }

  // 最近三角形：细网格（对角线 / 96，8 环）优先；未命中时退到粗网格（对角线 / 12，4 环，
  // 覆盖半径约对角线 / 3）兜底——裁切/清理阶段轻微移动过的顶点也能映射回原始卡片，
  // 避免个别顶点回退碎片级推导造成片内两套参数（WPO 撕裂）。
  int32 FindNearestTriangle(const FVector3f &Point, float &OutDistance) const {
    OutDistance = TNumericLimits<float>::Max();
    float BestDistanceSquared = TNumericLimits<float>::Max();
    int32 BestTriangle = FindNearestTriangleInGrid(
        Point, TriangleGrid, CellSize, /*MaxRings=*/8, BestDistanceSquared);
    if (BestTriangle == INDEX_NONE) {
      BestTriangle = FindNearestTriangleInGrid(
          Point, CoarseTriangleGrid, CoarseCellSize, /*MaxRings=*/4,
          BestDistanceSquared);
    }
    if (BestTriangle != INDEX_NONE) {
      OutDistance = FMath::Sqrt(BestDistanceSquared);
    }
    return BestTriangle;
  }

  // 收集 Point 周围 MaxDistance 内的所有卡片（卡 -> 该卡到点的最小距离平方）。
  // 贴合/叠放的多张卡片会同时命中——用于把“同一表面”的裁切碎片统一到同一张卡。
  void CollectNearCards(const FVector3f &Point, float MaxDistance,
                        TMap<int32, float> &OutCardDistanceSquared) const {
    OutCardDistanceSquared.Reset();
    if (MaxDistance <= 0.0f || NumTriangles() == 0 || TriangleGrid.Num() == 0) {
      return;
    }
    const float MaxDistanceSquared = MaxDistance * MaxDistance;
    const int32 MaxRings = FMath::Max(FMath::CeilToInt(MaxDistance / CellSize), 0);
    const FIntVector Center = GetCellKey(Point);
    for (int32 Ring = 0; Ring <= MaxRings; ++Ring) {
      for (int32 X = Center.X - Ring; X <= Center.X + Ring; ++X) {
        for (int32 Y = Center.Y - Ring; Y <= Center.Y + Ring; ++Y) {
          for (int32 Z = Center.Z - Ring; Z <= Center.Z + Ring; ++Z) {
            if (Ring > 0 && FMath::Abs(X - Center.X) != Ring &&
                FMath::Abs(Y - Center.Y) != Ring &&
                FMath::Abs(Z - Center.Z) != Ring) {
              continue;
            }
            const TArray<int32> *Bucket = TriangleGrid.Find(FIntVector(X, Y, Z));
            if (!Bucket) {
              continue;
            }
            for (const int32 Triangle : *Bucket) {
              const float DistanceSquared =
                  PointTriangleDistanceSquared(Point, Triangle);
              if (DistanceSquared > MaxDistanceSquared) {
                continue;
              }
              const int32 CardIndex = CardOfTriangle[Triangle];
              float &BestDistance =
                  OutCardDistanceSquared.FindOrAdd(CardIndex, DistanceSquared);
              BestDistance = FMath::Min(BestDistance, DistanceSquared);
            }
          }
        }
      }
    }
  }
};

// 风单元（连通片 / 卡片）成对键：高 32 位为较小索引，低 32 位为较大索引。
inline uint64 PlantWindUnitPairKey(int32 A, int32 B) {
  const uint32 Low = static_cast<uint32>(A < B ? A : B);
  const uint32 High = static_cast<uint32>(A < B ? B : A);
  return (static_cast<uint64>(Low) << 32) | static_cast<uint64>(High);
}

// 两轴夹角（度）：按 |dot| 取角（反平行视为 0），与原型 angleDeg 同口径。
inline float PlantWindAngleDegrees(const FVector3f &A, const FVector3f &B) {
  const FVector3f NormalizedA = A.GetSafeNormal();
  const FVector3f NormalizedB = B.GetSafeNormal();
  if (NormalizedA.IsNearlyZero() || NormalizedB.IsNearlyZero()) {
    return 0.0f;
  }
  const float Dot = FMath::Clamp(FMath::Abs(FVector3f::DotProduct(NormalizedA, NormalizedB)), 0.0f, 1.0f);
  return FMath::RadiansToDegrees(FMath::Acos(Dot));
}

// 结构接缝顶点对（attach / axial / 被容量判据拒绝的边）：低权重区直接对插，消除茎秆-子叶分离。
struct FPlantWindStructuralSeam {
  int32 VertexA = INDEX_NONE;
  int32 VertexB = INDEX_NONE;
  float BlendWeight = 0.0f;
};

// F5 结构接缝焊接（与原型 fern-rule2.cjs 的 weldSeams 同口径）：
//   1) 两遍扫描（增量网格 + 就地更新）：顶点与贴合半径内、属于其它风单元的已完成顶点做加权平均，
//      同一接缝两侧在第二遍后收敛到同一方向/权重/ripple；非结构接触只在两侧原始权重都 <= 0.7 时
//      参与（叶尖保护），结构接缝不受该门控。
//   2) 结构顶点对（合并边/被拒边/attach/axial）在低权重区（两侧原始权重 <= 0.6）直接对插。
// 返回：被改动的顶点数。
static int32 BlendPlantWindSeams(int32 VertexCount,
                                 const TArray<FVector3f> &Positions,
                                 const TArray<int32> &UnitOfVertex,
                                 const TArray<int32> *ClusterOfUnit,
                                 const TSet<uint64> &StructuralUnitPairs,
                                 const TArray<FPlantWindStructuralSeam> &StructuralSeams,
                                 float ContactDistance,
                                 TArray<FVector3f> &Direction,
                                 TArray<float> &Weight,
                                 TArray<float> &Ripple,
                                 TArray<uint8> &OutBlended,
                                 float NonStructuralTipWeightLimit = 0.7f,
                                 bool bBlendExplicitSeams = true) {
  OutBlended.Init(0, VertexCount);
  if (VertexCount <= 0 || ContactDistance <= 0.0f ||
      Positions.Num() < VertexCount || UnitOfVertex.Num() < VertexCount ||
      Direction.Num() < VertexCount || Weight.Num() < VertexCount ||
      Ripple.Num() < VertexCount) {
    return 0;
  }
  constexpr float StructuralSeamWeightLimit = 0.6f;
  const TArray<float> OriginalWeight = Weight;
  const float Cell = FMath::Max(ContactDistance, 1e-3f);
  const float RadiusSquared = ContactDistance * ContactDistance;
  const bool bHasClusterMap = ClusterOfUnit != nullptr && ClusterOfUnit->Num() >= UnitOfVertex.Num();
  auto ClusterIndexOfUnit = [&UnitOfVertex, ClusterOfUnit, bHasClusterMap](int32 VertexIndex) -> int32 {
    const int32 UnitIndex = UnitOfVertex[VertexIndex];
    if (UnitIndex == INDEX_NONE) {
      return INDEX_NONE;
    }
    if (bHasClusterMap && ClusterOfUnit->IsValidIndex(UnitIndex)) {
      return (*ClusterOfUnit)[UnitIndex];
    }
    return UnitIndex;
  };
  for (int32 Pass = 0; Pass < 2; ++Pass) {
    TMap<FIntVector, TArray<int32>> Grid;
    Grid.Reserve(VertexCount);
    for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
      const FVector3f Position = Positions[VertexIndex];
      const FIntVector Key(FMath::FloorToInt(Position.X / Cell),
                           FMath::FloorToInt(Position.Y / Cell),
                           FMath::FloorToInt(Position.Z / Cell));
      const int32 UnitIndex = UnitOfVertex[VertexIndex];
      const int32 ClusterIndex = ClusterIndexOfUnit(VertexIndex);
      float SumWeight = 1.0f;
      FVector3f SumDirection = Direction[VertexIndex];
      float SumRipple = Ripple[VertexIndex];
      float SumUnitWeight = Weight[VertexIndex];
      int32 NeighborCount = 0;
      if (UnitIndex != INDEX_NONE) {
        for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
          for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
            for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
              const TArray<int32> *Bucket = Grid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
              if (!Bucket) {
                continue;
              }
              for (const int32 OtherVertex : *Bucket) {
                const int32 OtherUnit = UnitOfVertex[OtherVertex];
                if (OtherUnit == INDEX_NONE || OtherUnit == UnitIndex) {
                  continue;
                }
                if (ClusterIndexOfUnit(OtherVertex) == ClusterIndex) {
                  continue;
                }
                const float DistanceSquared = FVector3f::DistSquared(Position, Positions[OtherVertex]);
                if (DistanceSquared > RadiusSquared) {
                  continue;
                }
                const bool bStructural =
                    StructuralUnitPairs.Contains(PlantWindUnitPairKey(UnitIndex, OtherUnit));
                if (!bStructural &&
                    FMath::Max(OriginalWeight[VertexIndex], OriginalWeight[OtherVertex]) >
                        NonStructuralTipWeightLimit) {
                  continue;
                }
                const float NeighborWeight = 1.0f - FMath::Sqrt(DistanceSquared) / ContactDistance;
                FVector3f OtherDirection = Direction[OtherVertex];
                if (FVector3f::DotProduct(OtherDirection, Direction[VertexIndex]) < 0.0f) {
                  OtherDirection = -OtherDirection;
                }
                SumWeight += NeighborWeight;
                SumDirection += OtherDirection * NeighborWeight;
                SumRipple += Ripple[OtherVertex] * NeighborWeight;
                SumUnitWeight += Weight[OtherVertex] * NeighborWeight;
                NeighborCount += 1;
              }
            }
          }
        }
      }
      if (NeighborCount > 0) {
        const FVector3f AveragedDirection = SumDirection.GetSafeNormal();
        if (!AveragedDirection.IsNearlyZero()) {
          Direction[VertexIndex] = AveragedDirection;
        }
        Ripple[VertexIndex] = SumRipple / SumWeight;
        Weight[VertexIndex] = SumUnitWeight / SumWeight;
        OutBlended[VertexIndex] = 1;
      }
      Grid.FindOrAdd(Key).Add(VertexIndex);
    }
  }
  for (const FPlantWindStructuralSeam &Seam : StructuralSeams) {
    if (!bBlendExplicitSeams) {
      break;
    }
    if (!Direction.IsValidIndex(Seam.VertexA) || !Direction.IsValidIndex(Seam.VertexB) ||
        Seam.VertexA == Seam.VertexB) {
      continue;
    }
    if (UnitOfVertex[Seam.VertexA] == UnitOfVertex[Seam.VertexB]) {
      continue;
    }
    if (ClusterIndexOfUnit(Seam.VertexA) == ClusterIndexOfUnit(Seam.VertexB)) {
      continue;
    }
    if (FMath::Max(OriginalWeight[Seam.VertexA], OriginalWeight[Seam.VertexB]) >
        StructuralSeamWeightLimit) {
      continue;
    }
    const float BlendWeight = FMath::Clamp(Seam.BlendWeight, 0.0f, 1.0f);
    FVector3f DirectionA = Direction[Seam.VertexA];
    FVector3f DirectionB = Direction[Seam.VertexB];
    if (FVector3f::DotProduct(DirectionA, DirectionB) < 0.0f) {
      DirectionB = -DirectionB;
    }
    Direction[Seam.VertexA] = (DirectionA * (1.0f - BlendWeight) + DirectionB * BlendWeight).GetSafeNormal();
    Direction[Seam.VertexB] = (DirectionB * (1.0f - BlendWeight) + DirectionA * BlendWeight).GetSafeNormal();
    const float WeightA = Weight[Seam.VertexA];
    const float WeightB = Weight[Seam.VertexB];
    Weight[Seam.VertexA] = WeightA * (1.0f - BlendWeight) + WeightB * BlendWeight;
    Weight[Seam.VertexB] = WeightB * (1.0f - BlendWeight) + WeightA * BlendWeight;
    const float RippleA = Ripple[Seam.VertexA];
    const float RippleB = Ripple[Seam.VertexB];
    Ripple[Seam.VertexA] = RippleA * (1.0f - BlendWeight) + RippleB * BlendWeight;
    Ripple[Seam.VertexB] = RippleB * (1.0f - BlendWeight) + RippleA * BlendWeight;
    OutBlended[Seam.VertexA] = 1;
    OutBlended[Seam.VertexB] = 1;
  }
  int32 BlendedCount = 0;
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    BlendedCount += OutBlended[VertexIndex] != 0 ? 1 : 0;
  }
  return BlendedCount;
}

// 卡片刚性场下的接缝处理：**不做跨卡平均**（2026-10-10 定稿）。
//
// 卡片映射（UV0 反查 + 一致性分组）已把每个碎片固定到唯一一张卡片，取该卡片的锚点/方向/沿轴
// 权重场。这张场本身是空间连续的：方向逐卡常量，权重 = 顶点在卡片轴上的投影比例，**同一张卡
// 相邻顶点的权重差实测仅 0.006**；跨卡的相邻碎片方向也基本一致（实测跨接缝 |Δ位移| 0.002~0.024）。
//
// 上一版在这里对贴合的跨卡顶点对做窄带加权平均，但平均的对象是一组**不在同一梯度上的量**：
// 不同卡片的权重场彼此独立、方向编码又存在量化误差，平均结果与带外未被改写的卡片值无法拼接，
// 于是在等化带边缘造出 0.35~0.4 的权重跳变与 20°~40° 的方向跳变，位移差被放大到 0.44~0.93
// （换算成 Branch1Bend=2 时是 0.9~1.9cm 的可见裂口）——这正是"蕨类茎秆与叶片分离、Cotton
// 整株蠕虫状扭曲"的直接来源。离线回归（Lady_Fern_h9ecnwd_03/11/14、Desert_Cotton_l9uj50a_01/05）
// 显示：去掉该等化后跨接缝最大位移差降到 0.010~0.024，且不受资产与卡片数量影响。
//
// 因此本函数保留为占位（调用点仍记录日志），不再改动任何属性。
static int32 EqualizePlantWindCardSeams(int32 VertexCount,
                                        const TArray<FVector3f> &Positions,
                                        const TArray<int32> &UnitOfVertex,
                                        float Radius,
                                        TArray<FVector3f> &Direction,
                                        TArray<float> &Weight,
                                        TArray<float> &Ripple,
                                        TArray<uint8> &OutWelded) {
  // 参数保持原签名，便于需要时恢复等化实现；当前一律不使用。
  (void)Positions;
  (void)UnitOfVertex;
  (void)Radius;
  (void)Direction;
  (void)Weight;
  (void)Ripple;
  // 保持与原实现一致的输出形状（逐顶点 0），调用方仍按 VertexCount 索引。
  OutWelded.Init(0, FMath::Max(VertexCount, 0));
  return 0;
}

} // namespace AssetHivePlantWind

struct FPlantAssetProfile {
  FString SubtypeFolder;  // Vegetation 下的子类目录，未识别时留空
  int32 MaxLOD0Triangles = 0;
  bool bNanite = true;
  bool bHandleCollision = false;
  bool bTrunkCollision = false;
  int32 TextureMaxSize = 2048;
  int32 TextureVTSize = 2048;
  bool bAllowVirtualTexture = true;
  // 合成 SpeedTree 风：仅 Megascans 3D Plants（Grass=branch1；Bush=branch1+branch2→UV3）。
  EPlantSyntheticWind SyntheticWind = EPlantSyntheticWind::None;
};

static FPlantAssetProfile ResolvePlantAssetProfile(const TArray<FString> &Tags) {
  FPlantAssetProfile Profile;
  // 合成风只针对 Megascans 导入的 3D Plants；st9 等原生资产保持不动。
  const bool bMegascansPlant = HasAssetTag(Tags, TEXT("Megascans"));
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
    if (bMegascansPlant) {
      Profile.SyntheticWind = EPlantSyntheticWind::Branch1Branch2UV3;
    }
    return Profile;
  }
  if (HasAssetTag(Tags, TEXT("Grass"))) {
    Profile.SubtypeFolder = TEXT("Grass");
    Profile.MaxLOD0Triangles = 10000;
    Profile.bHandleCollision = true;
    if (bMegascansPlant) {
      Profile.SyntheticWind = EPlantSyntheticWind::Branch1;
    }
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

// 合成风的片参数：按连通片（叶片/枝条卡片）推导锚点/方向/最大投影。
// 连通片 = 共享顶点并查集 + 位置焊接：不透明裁切等管线导出的 FBX 会把同一位置的顶点逐角拆开
//   （顶点数 = 角数，每个三角形各自持有 3 个独立顶点），只按共享顶点判定会把每个三角形当成一片，
//   各片的锚点/方向/权重都不同，片与片之间即出现 WPO 顶点撕裂。因此连通片判定前先按位置合并重复顶点。
//   焊接只参与连通性判定，不修改网格本身；正常的卡片式 FBX（顶点已共享）不受影响。
// 每片：锚点 = 片内最低 25% 顶点质心，方向 = 底 25% → 顶 25% 质心的方向，
//   最大投影 = 片内顶点沿方向相对锚点的投影最大值（权重归一化到 0..1 的分母）。
struct FPlantWindIslandParameters {
  TArray<int32> IslandOfVertex;
  TArray<FVector3f> BasePosition;
  // 顶部 25% 质心：SpeedTree 的 UV1.x（PackedPosition）编码的是叶片顶端位置。
  TArray<FVector3f> TipPosition;
  TArray<FVector3f> Direction;
  TArray<float> MaxProjection;
  FVector3f MeshMin = FVector3f(TNumericLimits<float>::Max());
  FVector3f MeshMax = FVector3f(-TNumericLimits<float>::Max());
  int32 IslandsBeforeWeld = 0;
  int32 WeldMergedVertices = 0;

  int32 Num() const { return BasePosition.Num(); }
};

static bool ComputePlantWindIslands(FMeshDescription &Mesh,
                                    FPlantWindIslandParameters &Out) {
  Out = FPlantWindIslandParameters();
  const int32 VertexCount = Mesh.Vertices().Num();
  if (VertexCount == 0 || Mesh.Triangles().Num() == 0) {
    return false;
  }
  FStaticMeshAttributes Attributes(Mesh);
  Attributes.Register(true);
  TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();

  // 连通片：共享顶点即同片（并查集 + 路径压缩）。
  TArray<int32> Parents;
  Parents.SetNumUninitialized(VertexCount);
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    Parents[VertexIndex] = VertexIndex;
  }
  auto FindRoot = [&Parents](int32 Index) {
    while (Parents[Index] != Index) {
      Parents[Index] = Parents[Parents[Index]];
      Index = Parents[Index];
    }
    return Index;
  };
  for (const FTriangleID TriangleID : Mesh.Triangles().GetElementIDs()) {
    const TArrayView<const FVertexID> Corners = Mesh.GetTriangleVertices(TriangleID);
    if (Corners.Num() < 3) {
      continue;
    }
    const int32 First = Corners[0].GetValue();
    for (int32 Corner = 1; Corner < 3; ++Corner) {
      const int32 RootA = FindRoot(First);
      const int32 RootB = FindRoot(Corners[Corner].GetValue());
      if (RootA != RootB) {
        Parents[RootB] = RootA;
      }
    }
  }

  // 焊接前的连通片数量：仅用于诊断逐角拆分的源网格（正常网格前后一致）。
  {
    TSet<int32> RootsBeforeWeld;
    RootsBeforeWeld.Reserve(VertexCount);
    for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
      RootsBeforeWeld.Add(FindRoot(VertexIndex));
    }
    Out.IslandsBeforeWeld = RootsBeforeWeld.Num();
  }

  // 位置焊接：把逐角拆开的重复顶点合并回同一片，避免每三角形各成一片产生 WPO 撕裂。
  const FBox MeshBounds = Mesh.ComputeBoundingBox();
  const float WeldEpsilon = FMath::Max(
      static_cast<float>(MeshBounds.GetSize().Size()) * 1e-5f, 1e-3f);
  Out.WeldMergedVertices = AssetHivePlantWind::WeldCoincidentVertices(
      VertexCount,
      [&Positions](int32 VertexIndex) {
        return Positions[FVertexID(VertexIndex)];
      },
      Parents, WeldEpsilon);

  struct FIslandAccumulator {
    int32 Count = 0;
    float MinZ = TNumericLimits<float>::Max();
    float MaxZ = -TNumericLimits<float>::Max();
    FVector3f Min = FVector3f(TNumericLimits<float>::Max());
    FVector3f Max = FVector3f(-TNumericLimits<float>::Max());
    FVector3d Sum = FVector3d::ZeroVector;
    FVector3d LowSum = FVector3d::ZeroVector;
    int32 LowCount = 0;
    FVector3d HighSum = FVector3d::ZeroVector;
    int32 HighCount = 0;
  };

  Out.IslandOfVertex.SetNumUninitialized(VertexCount);
  TArray<int32> IslandIndexByRoot;
  IslandIndexByRoot.Init(INDEX_NONE, VertexCount);
  TArray<FIslandAccumulator> Accumulators;
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const FVector3f Position = Positions[FVertexID(VertexIndex)];
    const int32 Root = FindRoot(VertexIndex);
    int32 IslandIndex = IslandIndexByRoot[Root];
    if (IslandIndex == INDEX_NONE) {
      IslandIndex = Accumulators.AddDefaulted();
      IslandIndexByRoot[Root] = IslandIndex;
    }
    Out.IslandOfVertex[VertexIndex] = IslandIndex;
    FIslandAccumulator &Accumulator = Accumulators[IslandIndex];
    Accumulator.Count += 1;
    Accumulator.MinZ = FMath::Min(Accumulator.MinZ, Position.Z);
    Accumulator.MaxZ = FMath::Max(Accumulator.MaxZ, Position.Z);
    for (int32 Axis = 0; Axis < 3; ++Axis) {
      Accumulator.Min[Axis] = FMath::Min(Accumulator.Min[Axis], Position[Axis]);
      Accumulator.Max[Axis] = FMath::Max(Accumulator.Max[Axis], Position[Axis]);
      Out.MeshMin[Axis] = FMath::Min(Out.MeshMin[Axis], Position[Axis]);
      Out.MeshMax[Axis] = FMath::Max(Out.MeshMax[Axis], Position[Axis]);
    }
    Accumulator.Sum += FVector3d(Position);
  }
  // 底/顶 25%（按片内 Z 范围）质心。
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    FIslandAccumulator &Accumulator =
        Accumulators[Out.IslandOfVertex[VertexIndex]];
    const float HeightRange = Accumulator.MaxZ - Accumulator.MinZ;
    const FVector3f Position = Positions[FVertexID(VertexIndex)];
    if (Position.Z <= Accumulator.MinZ + HeightRange * 0.25f) {
      Accumulator.LowSum += FVector3d(Position);
      Accumulator.LowCount += 1;
    }
    if (Position.Z >= Accumulator.MaxZ - HeightRange * 0.25f) {
      Accumulator.HighSum += FVector3d(Position);
      Accumulator.HighCount += 1;
    }
  }

  const int32 IslandCount = Accumulators.Num();
  Out.BasePosition.SetNum(IslandCount);
  Out.TipPosition.SetNum(IslandCount);
  Out.Direction.SetNum(IslandCount);
  Out.MaxProjection.Init(0.0f, IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    const FIslandAccumulator &Accumulator = Accumulators[IslandIndex];
    const FVector3d Center = Accumulator.Count > 0
                                 ? Accumulator.Sum / Accumulator.Count
                                 : FVector3d::ZeroVector;
    FVector3f Base = FVector3f(Center);
    FVector3f Tip = FVector3f(Center);
    if (Accumulator.LowCount > 0) {
      Base = FVector3f(Accumulator.LowSum / Accumulator.LowCount);
    }
    if (Accumulator.HighCount > 0) {
      Tip = FVector3f(Accumulator.HighSum / Accumulator.HighCount);
    }
    const float HeightRange = Accumulator.MaxZ - Accumulator.MinZ;
    const FVector3f Delta = Tip - Base;
    const float MinDirectionLength =
        FMath::Max(HeightRange * 0.05f, KINDA_SMALL_NUMBER);
    if (Delta.SizeSquared() > MinDirectionLength * MinDirectionLength) {
      Out.Direction[IslandIndex] = Delta.GetSafeNormal();
    } else {
      // 退化片（例如水平卡片）：取该片最长轴作为方向。
      const FVector3f Extent = Accumulator.Max - Accumulator.Min;
      int32 Axis = 0;
      if (Extent.Y > Extent.X) {
        Axis = 1;
      }
      if (Extent.Z > Extent[Axis]) {
        Axis = 2;
      }
      FVector3f Fallback = FVector3f::ZeroVector;
      Fallback[Axis] = 1.0f;
      Out.Direction[IslandIndex] = Fallback;
    }
    Out.BasePosition[IslandIndex] = Base;
    // SpeedTree UV1.x 实测：ThickGrass* / Grass3 三个资产的平均误差，顶部质心 8~12cm
    // （≈一个量化格距），基部质心 36~51cm。因此 UV1.x 用顶部位置而非基部。
    Out.TipPosition[IslandIndex] = Tip;
  }

  // 每片沿方向的最大投影，用于把权重归一化到 0..1。
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const int32 IslandIndex = Out.IslandOfVertex[VertexIndex];
    const float Projection = FVector3f::DotProduct(
        Positions[FVertexID(VertexIndex)] - Out.BasePosition[IslandIndex],
        Out.Direction[IslandIndex]);
    Out.MaxProjection[IslandIndex] =
        FMath::Max(Out.MaxProjection[IslandIndex], Projection);
  }
  return IslandCount > 0;
}

// 风单元合并模式（2026-10-07）：
//   None             —— 每个连通片各为一组参数（Bush）。
//   FrondLike        —— 仅把“主轴 + 短子叶”结构的连通簇合并（Grass，v4 判定）。
//   ConnectedCluster —— Fern：每片叶一个风单元（v7）。core 近似重合才合并；小叶卡片
//                       按最近 core + 径向方位归属；同片叶共享锚点/方向/沿轴权重场，
//                       不同叶片独立运动，避免整株合一后的共享运动形变。
enum class EPlantWindFrondMergeMode : uint8 {
  None,
  FrondLike,
  ConnectedCluster,
};

// SpeedTree frond 规则（SpeedTreeCommon.ush：Frond Wind / DirectionalBranchWindFrondStyle /
// LeafTumble）：一个 frond（叶轴 + 附着其上的子叶）共用同一个风力锚点、同一弯曲方向与
// 同一条沿轴权重场；叶片的相位由该锚点决定。合成风据此把空间相接、且呈“主轴 + 短子叶”
// 结构的连通片合并为一个叶片簇（frond），让叶轴与子叶在交界处取到同一套参数，从根上消除
// 分离；长度相近、仅根部相触的独立叶片（草叶簇）不合并，保留各自锚点与相位，动感不变。
struct FPlantWindFrondParameters {
  TArray<int32> FrondOfIsland;
  TArray<FVector3f> BasePosition;
  TArray<FVector3f> TipPosition;
  TArray<FVector3f> Direction;
  TArray<float> MaxProjection;
  int32 ConsolidatedFrondCount = 0;
  float JoinDistance = 0.0f;

  int32 Num() const { return BasePosition.Num(); }
};

// Fern 结构化风推导（F1-F4 + 回退判据）
struct FPlantWindFernStructure {
  TArray<FVector3f> Axis;
  TArray<float> Length;
  TArray<FVector3f> Base;
  TArray<FVector3f> Tip;
  TArray<float> Width;
  TArray<float> Planarity;
  TArray<float> MaxProjection;
  TArray<uint8> Rachis;
  TArray<int32> ClusterOfIsland;
  TArray<FVector3f> ClusterBase;
  TArray<FVector3f> ClusterDirection;
  TArray<FVector3f> ClusterWindPosition;
  TArray<float> ClusterMaxProjection;
  TSet<uint64> StructuralIslandPairs;
  TSet<uint64> StructuralClusterPairs;
  TArray<AssetHivePlantWind::FPlantWindStructuralSeam> StructuralSeams;
  float JoinDistance = 0.0f;
  float ContactDistance = 0.0f;
  float AttachDistance = 0.0f;
  float SpanLimit = 0.0f;
  int32 RachisCount = 0;
  int32 ClusterCount = 0;
  int32 CandidatePairCount = 0;
  int32 MergedPairCount = 0;
  // 邻近合并（贴合且同向的连通片并入同一风单元）统计，见 ComputePlantWindFernStructure 内注释。
  int32 ProximityMerges = 0;
  int32 ProximityRejectedBySpan = 0;
  int32 ProximityRejectedByRachis = 0;
  int32 RejectedBySpan = 0;
  int32 RejectedByRachisCapacity = 0;
  int32 RejectedParallelRachis = 0;
  int32 LargestClusterIslands = 0;
  float LargestClusterShare = 0.0f;
  bool bFallbackPerIsland = false;
};

template <typename TGetPosition>
static bool ComputePlantWindFernStructure(int32 VertexCount,
                                          TGetPosition &&GetPosition,
                                          const FPlantWindIslandParameters &Islands,
                                          FPlantWindFernStructure &Out) {
  Out = FPlantWindFernStructure();
  const int32 IslandCount = Islands.Num();
  if (VertexCount <= 0 || IslandCount <= 0 || Islands.IslandOfVertex.Num() < VertexCount) {
    return false;
  }
  const FVector3f MeshSize = Islands.MeshMax - Islands.MeshMin;
  const float Diagonal = MeshSize.Size();
  if (Diagonal <= KINDA_SMALL_NUMBER) {
    return false;
  }
  // F1 参数：按资产对角自适应（原型终版 0.005 / 0.010 / 0.5 / 0.02，单位 cm）。
  Out.JoinDistance = FMath::Clamp(Diagonal * 0.005f, 0.15f, 1.5f);
  Out.ContactDistance = FMath::Clamp(FMath::Max(Out.JoinDistance, Diagonal * 0.010f), 0.2f, 1.2f);
  Out.AttachDistance = FMath::Clamp(Diagonal * 0.020f, 0.3f, 3.0f);
  Out.SpanLimit = Diagonal * 0.5f;

  TArray<TArray<int32>> IslandVertices;
  IslandVertices.SetNum(IslandCount);
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
    if (IslandVertices.IsValidIndex(IslandIndex)) {
      IslandVertices[IslandIndex].Add(VertexIndex);
    }
  }
  // ORIENT-FIX: 资产 XY 质心，供近水平片判断“朝外”方向。
  double AssetCenterX = 0.0;
  double AssetCenterY = 0.0;
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const FVector3f Position = GetPosition(VertexIndex);
    AssetCenterX += Position.X;
    AssetCenterY += Position.Y;
  }
  AssetCenterX /= static_cast<double>(VertexCount);
  AssetCenterY /= static_cast<double>(VertexCount);

  Out.Axis.SetNum(IslandCount);
  Out.Length.Init(0.0f, IslandCount);
  Out.Base.SetNum(IslandCount);
  Out.Tip.SetNum(IslandCount);
  Out.Width.Init(0.0f, IslandCount);
  Out.Planarity.Init(0.0f, IslandCount);
  Out.MaxProjection.Init(0.0f, IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    const TArray<int32> &Vertices = IslandVertices[IslandIndex];
    const int32 Count = Vertices.Num();
    const FVector3f FallbackDirection =
        Islands.Direction.IsValidIndex(IslandIndex) ? Islands.Direction[IslandIndex]
                                                    : FVector3f(0.0f, 0.0f, 1.0f);
    const FVector3f FallbackBase = Islands.BasePosition.IsValidIndex(IslandIndex)
                                       ? Islands.BasePosition[IslandIndex]
                                       : FVector3f::ZeroVector;
    if (Count <= 0) {
      Out.Axis[IslandIndex] = FallbackDirection;
      Out.Base[IslandIndex] = FallbackBase;
      Out.Tip[IslandIndex] = FallbackBase;
      continue;
    }
    FVector3d Centroid = FVector3d::ZeroVector;
    for (const int32 VertexIndex : Vertices) {
      Centroid += FVector3d(GetPosition(VertexIndex));
    }
    Centroid = Centroid / static_cast<double>(Count);
    double XX = 0.0, YY = 0.0, ZZ = 0.0, XY = 0.0, XZ = 0.0, YZ = 0.0;
    for (const int32 VertexIndex : Vertices) {
      const FVector3d Delta = FVector3d(GetPosition(VertexIndex)) - Centroid;
      XX += Delta.X * Delta.X;
      YY += Delta.Y * Delta.Y;
      ZZ += Delta.Z * Delta.Z;
      XY += Delta.X * Delta.Y;
      XZ += Delta.X * Delta.Z;
      YZ += Delta.Y * Delta.Z;
    }
    XX /= Count; YY /= Count; ZZ /= Count; XY /= Count; XZ /= Count; YZ /= Count;
    // 幂迭代：三个坐标轴种子各跑一次，取 Rayleigh 商最大者。
    // 单一 (1,0,0) 种子在轴对齐网格上可能恰好落在非主特征方向并保持不动。
    double AxisX = 1.0, AxisY = 0.0, AxisZ = 0.0;
    double BestEigen = -1.0;
    for (int32 SeedIndex = 0; SeedIndex < 3; ++SeedIndex) {
      double SeedX = SeedIndex == 0 ? 1.0 : 0.0;
      double SeedY = SeedIndex == 1 ? 1.0 : 0.0;
      double SeedZ = SeedIndex == 2 ? 1.0 : 0.0;
      for (int32 Iteration = 0; Iteration < 32; ++Iteration) {
        const double NextX = XX * SeedX + XY * SeedY + XZ * SeedZ;
        const double NextY = XY * SeedX + YY * SeedY + YZ * SeedZ;
        const double NextZ = XZ * SeedX + YZ * SeedY + ZZ * SeedZ;
        const double AxisLength = FMath::Sqrt(NextX * NextX + NextY * NextY + NextZ * NextZ);
        if (AxisLength <= 1e-12) {
          break;
        }
        SeedX = NextX / AxisLength;
        SeedY = NextY / AxisLength;
        SeedZ = NextZ / AxisLength;
      }
      const double Eigen =
          XX * SeedX * SeedX + YY * SeedY * SeedY + ZZ * SeedZ * SeedZ +
          2.0 * (XY * SeedX * SeedY + XZ * SeedX * SeedZ + YZ * SeedY * SeedZ);
      if (Eigen > BestEigen) {
        BestEigen = Eigen;
        AxisX = SeedX;
        AxisY = SeedY;
        AxisZ = SeedZ;
      }
    }
    const FVector3f PcaAxis(static_cast<float>(AxisX), static_cast<float>(AxisY), static_cast<float>(AxisZ));
    float LowProjection = TNumericLimits<float>::Max();
    float HighProjection = -TNumericLimits<float>::Max();
    for (const int32 VertexIndex : Vertices) {
      const FVector3d Delta = FVector3d(GetPosition(VertexIndex)) - Centroid;
      const float Projection = static_cast<float>(Delta.X * AxisX + Delta.Y * AxisY + Delta.Z * AxisZ);
      LowProjection = FMath::Min(LowProjection, Projection);
      HighProjection = FMath::Max(HighProjection, Projection);
    }
    const float IslandLength = FMath::Max(HighProjection - LowProjection, 1e-6f);
    double LateralSquared = 0.0;
    FVector3d LowSum = FVector3d::ZeroVector;
    FVector3d HighSum = FVector3d::ZeroVector;
    int32 LowCount = 0;
    int32 HighCount = 0;
    for (const int32 VertexIndex : Vertices) {
      const FVector3f Position = GetPosition(VertexIndex);
      const FVector3d Delta = FVector3d(Position) - Centroid;
      const double Projection = Delta.X * AxisX + Delta.Y * AxisY + Delta.Z * AxisZ;
      const double LateralX = Delta.X - AxisX * Projection;
      const double LateralY = Delta.Y - AxisY * Projection;
      const double LateralZ = Delta.Z - AxisZ * Projection;
      LateralSquared += LateralX * LateralX + LateralY * LateralY + LateralZ * LateralZ;
      if (Projection <= LowProjection + IslandLength * 0.25f) {
        LowSum += FVector3d(Position);
        LowCount += 1;
      }
      if (Projection >= HighProjection - IslandLength * 0.25f) {
        HighSum += FVector3d(Position);
        HighCount += 1;
      }
    }
    LateralSquared /= static_cast<double>(Count);
    FVector3f BandBase = LowCount > 0 ? FVector3f(LowSum / LowCount) : FVector3f(Centroid);
    FVector3f BandTip = HighCount > 0 ? FVector3f(HighSum / HighCount) : FVector3f(Centroid);
    FVector3f Axis = BandTip - BandBase;
    Axis = Axis.SizeSquared() > 1e-12f ? Axis.GetSafeNormal() : PcaAxis;
    // ORIENT-FIX 方向归一：基部=低端（+Z 为上）；近水平片取远离资产中轴的一端为叶尖。
    // 否则 PCA 轴符号不定会让 F4 权重沿“顶->基”增长，整个簇发硬。
    //
    // 双判据（2026-10-10 补充）：原口径 `|Axis.Z| >= 0.2` 时**只看 Z 符号**，径向测试被跳过。
    // 实测两类资产会在这条上失效——拱形羽叶回弯后锚点落在植株外侧、末端指向中心：
    //   Lady_Fern_h9ecnwd_03 的卡 9（|Z| = 0.206）与 h9ecnwd_12 的卡 1（|Z| = 0.850），
    //   锚点半径 26.3 / 10.8cm 而末端半径只有 3.0 / 6.6cm。
    // 此时权重斜坡沿「外侧 -> 中心」增长，视觉上是「叶尖不动、末端动」，与预期完全相反。
    // 因此补一次径向一致性测试：只要末端离植株中轴明显更近（小于基部半径的 1/1.25），
    // 就认定方向反了。系数取 1.25 而非 1.0，是为了把基部/末端 25% 波段质心的正常抖动排除在外，
    // 避免把垂直生长的叶片误翻。
    const double BaseRadius = FMath::Sqrt((BandBase.X - AssetCenterX) * (BandBase.X - AssetCenterX) +
                                          (BandBase.Y - AssetCenterY) * (BandBase.Y - AssetCenterY));
    const double TipRadius = FMath::Sqrt((BandTip.X - AssetCenterX) * (BandTip.X - AssetCenterX) +
                                         (BandTip.Y - AssetCenterY) * (BandTip.Y - AssetCenterY));
    bool bFlipAxis = false;
    if (FMath::Abs(Axis.Z) >= 0.2f) {
      bFlipAxis = Axis.Z < 0.0f;
    } else {
      bFlipAxis = TipRadius < BaseRadius - 1e-6;
    }
    if (!bFlipAxis && TipRadius > 0.05 && BaseRadius > TipRadius * 1.25) {
      bFlipAxis = true;
    }
    if (bFlipAxis) {
      Axis = -Axis;
      const FVector3f SwapPosition = BandBase;
      BandBase = BandTip;
      BandTip = SwapPosition;
    }
    Out.Axis[IslandIndex] = Axis;
    Out.Length[IslandIndex] = IslandLength;
    Out.Base[IslandIndex] = BandBase;
    Out.Tip[IslandIndex] = BandTip;
    Out.Width[IslandIndex] = static_cast<float>(FMath::Sqrt(FMath::Max(LateralSquared, 0.0)));
    Out.Planarity[IslandIndex] = static_cast<float>(
        LateralSquared / FMath::Max(static_cast<double>(IslandLength) * IslandLength, 1e-9));
    float MaxProjection = 0.0f;
    for (const int32 VertexIndex : Vertices) {
      MaxProjection = FMath::Max(MaxProjection, FVector3f::DotProduct(GetPosition(VertexIndex) - BandBase, Axis));
    }
    Out.MaxProjection[IslandIndex] = MaxProjection;
  }

  // F2 类型：长且细长 -> rachis（叶轴/主枝）；其余 leaflet（叶片/小叶）。
  TArray<float> SortedLengths = Out.Length;
  SortedLengths.Sort();
  const int32 SortedCount = SortedLengths.Num();
  const float LengthP90 =
      SortedCount > 0
          ? SortedLengths[FMath::Min(SortedCount - 1, FMath::FloorToInt(SortedCount * 0.90f))]
          : 0.0f;
  Out.Rachis.Init(0, IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    const float Slenderness = Out.Length[IslandIndex] / FMath::Max(Out.Width[IslandIndex], 1e-6f);
    const bool bLongEnough =
        Out.Length[IslandIndex] >= Diagonal * 0.20f || Out.Length[IslandIndex] >= LengthP90;
    const bool bSlender = Slenderness >= 3.0f;
    const bool bSmallBlade = Out.Length[IslandIndex] < Diagonal * 0.12f;
    if (bLongEnough && bSlender && !bSmallBlade) {
      Out.Rachis[IslandIndex] = 1;
      Out.RachisCount += 1;
    }
  }

  // F3 候选边（一）：贴合 / 基部附着（空间哈希，cell = Rattach）。
  struct FContactCandidate {
    float MinDistance = TNumericLimits<float>::Max();
    int32 VertexA = INDEX_NONE;
    int32 VertexB = INDEX_NONE;
    bool bAttach = false;
    float AttachDistance = TNumericLimits<float>::Max();
    int32 AttachVertexA = INDEX_NONE;
    int32 AttachVertexB = INDEX_NONE;
  };
  struct FAxialCandidate {
    float Distance = TNumericLimits<float>::Max();
    int32 VertexA = INDEX_NONE;
    int32 VertexB = INDEX_NONE;
  };
  TArray<uint8> BaseVertex;
  BaseVertex.Init(0, VertexCount);
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
    if (!Out.Base.IsValidIndex(IslandIndex)) {
      continue;
    }
    const float Reach = FMath::Max(Out.Length[IslandIndex] * 0.35f, 0.3f);
    BaseVertex[VertexIndex] =
        FVector3f::DistSquared(GetPosition(VertexIndex), Out.Base[IslandIndex]) <= Reach * Reach ? 1 : 0;
  }
  TMap<uint64, FContactCandidate> ContactPairs;
  const float Cell = FMath::Max(Out.AttachDistance, 1e-3f);
  const float AttachSquared = Out.AttachDistance * Out.AttachDistance;
  TMap<FIntVector, TArray<int32>> VertexGrid;
  VertexGrid.Reserve(VertexCount);
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const FVector3f Position = GetPosition(VertexIndex);
    const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
    const FIntVector Key(FMath::FloorToInt(Position.X / Cell),
                         FMath::FloorToInt(Position.Y / Cell),
                         FMath::FloorToInt(Position.Z / Cell));
    for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
      for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
        for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
          const TArray<int32> *Bucket = VertexGrid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
          if (!Bucket) {
            continue;
          }
          for (const int32 OtherVertex : *Bucket) {
            const int32 OtherIsland = Islands.IslandOfVertex[OtherVertex];
            if (OtherIsland == IslandIndex) {
              continue;
            }
            const float DistanceSquared = FVector3f::DistSquared(Position, GetPosition(OtherVertex));
            if (DistanceSquared > AttachSquared) {
              continue;
            }
            const bool bVertexFirst = IslandIndex < OtherIsland;
            FContactCandidate &Record = ContactPairs.FindOrAdd(
                AssetHivePlantWind::PlantWindUnitPairKey(IslandIndex, OtherIsland));
            const float Distance = FMath::Sqrt(DistanceSquared);
            if (Distance < Record.MinDistance) {
              Record.MinDistance = Distance;
              Record.VertexA = bVertexFirst ? VertexIndex : OtherVertex;
              Record.VertexB = bVertexFirst ? OtherVertex : VertexIndex;
            }
            if (BaseVertex[VertexIndex] != 0 || BaseVertex[OtherVertex] != 0) {
              if (Distance < Record.AttachDistance) {
                Record.AttachDistance = Distance;
                Record.AttachVertexA = bVertexFirst ? VertexIndex : OtherVertex;
                Record.AttachVertexB = bVertexFirst ? OtherVertex : VertexIndex;
              }
              Record.bAttach = true;
            }
          }
        }
      }
    }
    VertexGrid.FindOrAdd(Key).Add(VertexIndex);
  }

  // F3 候选边（二）：轴向外延（叶轴主轴两端延长线采样命中，补足“小叶基部插在叶轴上”的接缝）。
  TMap<uint64, FAxialCandidate> AxialPairs;
  const float AxialSampleStep = FMath::Max(Out.JoinDistance, 0.5f);
  const float AxialHitTolerance = AxialSampleStep * 1.6f;
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    const FVector3f Direction = Out.Axis[IslandIndex];
    if (Direction.IsNearlyZero()) {
      continue;
    }
    const float IslandLength = FMath::Max(Out.Length[IslandIndex], 1e-6f);
    for (int32 Side = 0; Side < 2; ++Side) {
      const float DirectionSign = Side == 0 ? -1.0f : 1.0f;
      const FVector3f Origin = Side == 0 ? Out.Base[IslandIndex] : Out.Tip[IslandIndex];
      const float Reach = Side == 0 ? IslandLength * 0.6f : IslandLength * 0.9f;
      const int32 Steps = FMath::Max(2, FMath::FloorToInt(Reach / AxialSampleStep));
      for (int32 Step = 1; Step <= Steps; ++Step) {
        const float StepDistance = Reach * static_cast<float>(Step) / static_cast<float>(Steps);
        const FVector3f Sample = Origin + Direction * (StepDistance * DirectionSign);
        const FIntVector SampleKey(FMath::FloorToInt(Sample.X / Cell),
                                   FMath::FloorToInt(Sample.Y / Cell),
                                   FMath::FloorToInt(Sample.Z / Cell));
        int32 NearestHitVertex = INDEX_NONE;
        float NearestHitSquared = TNumericLimits<float>::Max();
        for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
          for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
            for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
              const TArray<int32> *Bucket = VertexGrid.Find(SampleKey + FIntVector(OffsetX, OffsetY, OffsetZ));
              if (!Bucket) {
                continue;
              }
              for (const int32 OtherVertex : *Bucket) {
                const int32 OtherIsland = Islands.IslandOfVertex[OtherVertex];
                if (OtherIsland == IslandIndex) {
                  continue;
                }
                if (AssetHivePlantWind::PlantWindAngleDegrees(Direction, Out.Axis[OtherIsland]) > 50.0f) {
                  continue;
                }
                const float DistanceSquared = FVector3f::DistSquared(Sample, GetPosition(OtherVertex));
                if (DistanceSquared <= AxialHitTolerance * AxialHitTolerance &&
                    DistanceSquared < NearestHitSquared) {
                  NearestHitSquared = DistanceSquared;
                  NearestHitVertex = OtherVertex;
                }
              }
            }
          }
        }
        if (NearestHitVertex == INDEX_NONE) {
          continue;
        }
        const uint64 PairKey = AssetHivePlantWind::PlantWindUnitPairKey(
            IslandIndex, Islands.IslandOfVertex[NearestHitVertex]);
        const float SampleDistance = FMath::Sqrt(NearestHitSquared);
        const FAxialCandidate *Existing = AxialPairs.Find(PairKey);
        if (Existing != nullptr && Existing->Distance <= SampleDistance) {
          continue;
        }
        int32 NearestAxisVertex = INDEX_NONE;
        float NearestAxisSquared = TNumericLimits<float>::Max();
        for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
          for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
            for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
              const TArray<int32> *Bucket = VertexGrid.Find(SampleKey + FIntVector(OffsetX, OffsetY, OffsetZ));
              if (!Bucket) {
                continue;
              }
              for (const int32 AxisVertex : *Bucket) {
                if (Islands.IslandOfVertex[AxisVertex] != IslandIndex) {
                  continue;
                }
                const float DistanceSquared = FVector3f::DistSquared(Sample, GetPosition(AxisVertex));
                if (DistanceSquared < NearestAxisSquared) {
                  NearestAxisSquared = DistanceSquared;
                  NearestAxisVertex = AxisVertex;
                }
              }
            }
          }
        }
        const bool bIslandFirst = IslandIndex < Islands.IslandOfVertex[NearestHitVertex];
        const int32 AxisVertex = NearestAxisVertex != INDEX_NONE ? NearestAxisVertex : NearestHitVertex;
        FAxialCandidate &Record = AxialPairs.FindOrAdd(PairKey);
        Record.Distance = SampleDistance;
        Record.VertexA = bIslandFirst ? AxisVertex : NearestHitVertex;
        Record.VertexB = bIslandFirst ? NearestHitVertex : AxisVertex;
      }
    }
  }

  // F3 归组：Kruskal（结构过滤 + 每簇 rachis 上限 2 + 簇跨度上限）。
  struct FMergeEdge {
    int32 Kind = 2;  // 0 = 轴向外延，1 = 基部附着，2 = 贴合
    float Distance = 0.0f;
    int32 IslandA = INDEX_NONE;
    int32 IslandB = INDEX_NONE;
    int32 VertexA = INDEX_NONE;
    int32 VertexB = INDEX_NONE;
  };
  TArray<FMergeEdge> Edges;
  Out.CandidatePairCount = ContactPairs.Num();
  TSet<uint64> FallbackStructuralPairs;
  for (const TPair<uint64, FAxialCandidate> &Pair : AxialPairs) {
    const int32 IslandA = static_cast<int32>(Pair.Key >> 32);
    const int32 IslandB = static_cast<int32>(Pair.Key & 0xFFFFFFFFull);
    FallbackStructuralPairs.Add(Pair.Key);
    if (Out.Rachis[IslandA] == 0 && Out.Rachis[IslandB] == 0) {
      continue;
    }
    if (AssetHivePlantWind::PlantWindAngleDegrees(Out.Axis[IslandA], Out.Axis[IslandB]) > 50.0f) {
      continue;
    }
    FMergeEdge Edge;
    Edge.Kind = 0;
    Edge.Distance = Pair.Value.Distance;
    Edge.IslandA = IslandA;
    Edge.IslandB = IslandB;
    Edge.VertexA = Pair.Value.VertexA;
    Edge.VertexB = Pair.Value.VertexB;
    Edges.Add(Edge);
  }
  for (const TPair<uint64, FContactCandidate> &Pair : ContactPairs) {
    const FContactCandidate &Record = Pair.Value;
    const int32 IslandA = static_cast<int32>(Pair.Key >> 32);
    const int32 IslandB = static_cast<int32>(Pair.Key & 0xFFFFFFFFull);
    const bool bBothRachis = Out.Rachis[IslandA] != 0 && Out.Rachis[IslandB] != 0;
    if (bBothRachis) {
      continue;  // 纯贴合平行双轴不并
    }
    if (Record.bAttach) {
      FallbackStructuralPairs.Add(Pair.Key);
    }
    const bool bOneRachis = Out.Rachis[IslandA] != 0 || Out.Rachis[IslandB] != 0;
    if (Record.MinDistance <= Out.JoinDistance) {
      const float AngleLimit = bOneRachis ? 65.0f : 35.0f;
      if (AssetHivePlantWind::PlantWindAngleDegrees(Out.Axis[IslandA], Out.Axis[IslandB]) <= AngleLimit ||
          Record.bAttach) {
        FMergeEdge Edge;
        Edge.Kind = 2;
        Edge.Distance = Record.MinDistance;
        Edge.IslandA = IslandA;
        Edge.IslandB = IslandB;
        Edge.VertexA = Record.VertexA;
        Edge.VertexB = Record.VertexB;
        Edges.Add(Edge);
      }
    }
    if (Record.bAttach && Record.AttachVertexA != INDEX_NONE &&
        Record.AttachDistance <= Out.AttachDistance) {
      FMergeEdge Edge;
      Edge.Kind = 1;
      Edge.Distance = FMath::Min(Record.AttachDistance, Record.MinDistance);
      Edge.IslandA = IslandA;
      Edge.IslandB = IslandB;
      Edge.VertexA = Record.AttachVertexA;
      Edge.VertexB = Record.AttachVertexB;
      Edges.Add(Edge);
    }
  }
  Edges.Sort([](const FMergeEdge &A, const FMergeEdge &B) {
    if (A.Kind != B.Kind) {
      return A.Kind < B.Kind;
    }
    if (A.Distance != B.Distance) {
      return A.Distance < B.Distance;
    }
    if (A.IslandA != B.IslandA) {
      return A.IslandA < B.IslandA;
    }
    return A.IslandB < B.IslandB;
  });

  TArray<int32> Parents;
  Parents.SetNumUninitialized(IslandCount);
  TArray<int32> RootRachisCount;
  RootRachisCount.SetNumUninitialized(IslandCount);
  TArray<FVector3f> RootBoundsMin;
  RootBoundsMin.SetNumUninitialized(IslandCount);
  TArray<FVector3f> RootBoundsMax;
  RootBoundsMax.SetNumUninitialized(IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    Parents[IslandIndex] = IslandIndex;
    RootRachisCount[IslandIndex] = Out.Rachis[IslandIndex] != 0 ? 1 : 0;
    FVector3f BoundsMin(TNumericLimits<float>::Max());
    FVector3f BoundsMax(-TNumericLimits<float>::Max());
    for (const int32 VertexIndex : IslandVertices[IslandIndex]) {
      const FVector3f Position = GetPosition(VertexIndex);
      for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex) {
        BoundsMin[AxisIndex] = FMath::Min(BoundsMin[AxisIndex], Position[AxisIndex]);
        BoundsMax[AxisIndex] = FMath::Max(BoundsMax[AxisIndex], Position[AxisIndex]);
      }
    }
    RootBoundsMin[IslandIndex] = BoundsMin;
    RootBoundsMax[IslandIndex] = BoundsMax;
  }
  for (const FMergeEdge &Edge : Edges) {
    int32 RootA = Edge.IslandA;
    while (Parents[RootA] != RootA) {
      Parents[RootA] = Parents[Parents[RootA]];
      RootA = Parents[RootA];
    }
    int32 RootB = Edge.IslandB;
    while (Parents[RootB] != RootB) {
      Parents[RootB] = Parents[Parents[RootB]];
      RootB = Parents[RootB];
    }
    if (RootA == RootB) {
      continue;
    }
    bool bAccepted = false;
    if (RootRachisCount[RootA] + RootRachisCount[RootB] > 2) {
      Out.RejectedByRachisCapacity += 1;
    } else if (RootRachisCount[RootA] == 1 && RootRachisCount[RootB] == 1 && Edge.Kind == 2) {
      Out.RejectedParallelRachis += 1;
    } else {
      FVector3f MergedMin;
      FVector3f MergedMax;
      for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex) {
        MergedMin[AxisIndex] = FMath::Min(RootBoundsMin[RootA][AxisIndex], RootBoundsMin[RootB][AxisIndex]);
        MergedMax[AxisIndex] = FMath::Max(RootBoundsMax[RootA][AxisIndex], RootBoundsMax[RootB][AxisIndex]);
      }
      if ((MergedMax - MergedMin).Size() > Out.SpanLimit) {
        Out.RejectedBySpan += 1;
      } else {
        Parents[RootB] = RootA;
        RootRachisCount[RootA] += RootRachisCount[RootB];
        RootBoundsMin[RootA] = MergedMin;
        RootBoundsMax[RootA] = MergedMax;
        Out.MergedPairCount += 1;
        bAccepted = true;
      }
    }
    Out.StructuralIslandPairs.Add(AssetHivePlantWind::PlantWindUnitPairKey(Edge.IslandA, Edge.IslandB));
    if (!bAccepted && Edge.VertexA != INDEX_NONE && Edge.VertexB != INDEX_NONE &&
        Edge.VertexA != Edge.VertexB) {
      AssetHivePlantWind::FPlantWindStructuralSeam Seam;
      Seam.VertexA = Edge.VertexA;
      Seam.VertexB = Edge.VertexB;
      Seam.BlendWeight = Edge.Kind == 2 ? 0.30f : (Edge.Kind == 1 ? 0.35f : 0.25f);
      Out.StructuralSeams.Add(Seam);
    }
  }
  // 结构顶点对：attach / axial 关系本身也直接对插（低权重区，叶尖不参与）。
  for (const TPair<uint64, FContactCandidate> &Pair : ContactPairs) {
    const FContactCandidate &Record = Pair.Value;
    if (!Record.bAttach || Record.AttachVertexA == INDEX_NONE || Record.AttachVertexB == INDEX_NONE) {
      continue;
    }
    if (Record.AttachDistance > FMath::Max(Out.AttachDistance, Out.ContactDistance)) {
      continue;
    }
    AssetHivePlantWind::FPlantWindStructuralSeam Seam;
    Seam.VertexA = Record.AttachVertexA;
    Seam.VertexB = Record.AttachVertexB;
    Seam.BlendWeight = 0.35f;
    Out.StructuralSeams.Add(Seam);
  }
  for (const TPair<uint64, FAxialCandidate> &Pair : AxialPairs) {
    if (Pair.Value.VertexA == INDEX_NONE || Pair.Value.VertexB == INDEX_NONE ||
        Pair.Value.VertexA == Pair.Value.VertexB) {
      continue;
    }
    AssetHivePlantWind::FPlantWindStructuralSeam Seam;
    Seam.VertexA = Pair.Value.VertexA;
    Seam.VertexB = Pair.Value.VertexB;
    Seam.BlendWeight = 0.25f;
    Out.StructuralSeams.Add(Seam);
  }
  auto FindRoot = [&Parents](int32 Index) {
    while (Parents[Index] != Index) {
      Parents[Index] = Parents[Parents[Index]];
      Index = Parents[Index];
    }
    return Index;
  };

  // 邻近合并（2026-10-10）：贴合且方向相近的连通片必须共用同一个风单元。
  //
  // 症状：Lady_Fern_h9ecnwd_14 的卡 0（9216 顶点）与卡 1（1332 顶点）最近顶点只差 0.02cm
  //   ——本质是同一片叶子——但锚点模长 7.28cm vs 1.95cm。F3 的合并判据（rachis<=2、跨度上限、
  //   平行双轴拒绝）没有把它们并起来，于是两张卡各持一条独立梯度：被切开的位置整体平移 5.3cm
  //   量级，视觉上就是"茎秆与叶片分离"。Lady_Fern_h9ecnwd_03 的卡 12/13/14 同理（各 3129 顶点，
  //   互相只差 0.01~0.17cm，锚点却相距 3.6cm）。
  //
  // 两道护栏（离线实测，缺一不可）：
  //   1) 合并距离取 0.5cm：只并"真正重合"的片。取 2.0cm 会一路传递合并，实测本地
  //      Megascans 蕨 Var4 从 14 张卡并到 1 张，整株退化成刚性整体运动，比现状更糟。
  //      0.5cm 也低于 SyntheticWindFernWideGap 用例里的 8mm 间隙，该用例断言的
  //      "8mm 根部间隙并入同一风单元"由 F3 负责，不被本规则干扰。
  //   2) 并集跨度不得超过"两片各自沿自身轴跨度之和"的 1.6 倍。不能拿并集直接和固定阈值比：
  //      换锚轴后，原本沿自身轴 16cm 的片在新轴上会被量成 29cm——那是"换尺子"造成的假超标，
  //      会把本该粘合的两段羽叶挡在门外（bvxkwlf_13 实测就被这条挡住）。
  // 角度阈值 70°：羽叶是拱形，同一片叶被切成两段时段间本来就有明显弯折。
  //   bvxkwlf_13 实测两轴夹角 46.371°，45° 阈值恰好把它挡在门外，导致主干与羽叶分离
  //   （跨卡接触面 50626 对顶点相对错动 1.26cm）。放宽到 70° 后该资产并成 1 张卡、错动归零；
  //   全家族 01/03/11/13/14/15 六个变体实测跨卡位移差全部为 0.000cm。
  // rachis 容量上限放开（64）：0.5cm 的贴合距离本身已排除级联，再加严会误伤
  // SyntheticWindFernWideGap 这类"三片都应并入同一风单元"的合法用例。
  constexpr float ProximityMergeDistance = 0.5f;
  constexpr float ProximityMergeAngleDegrees = 70.0f;
  constexpr int32 ProximityMergeMaxRachis = 64;
  constexpr float ProximityMergeSpanRatio = 1.6f;
  {
    struct FProximityCandidate {
      int32 IslandA = INDEX_NONE;
      int32 IslandB = INDEX_NONE;
      float ProximityDistanceSquared = 0.0f;
    };
    TArray<FProximityCandidate> Candidates;
    TSet<uint64> SeenPairs;
    TMap<FIntVector, TArray<int32>> ProximityGrid;
    ProximityGrid.Reserve(VertexCount);
    const float ProximityCell = ProximityMergeDistance;
    const float ProximitySquared = ProximityMergeDistance * ProximityMergeDistance;
    const float CosMergeAngle = FMath::Cos(FMath::DegreesToRadians(ProximityMergeAngleDegrees));
    for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
      const FVector3f Position = GetPosition(VertexIndex);
      const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
      const FIntVector Key(FMath::FloorToInt(Position.X / ProximityCell),
                           FMath::FloorToInt(Position.Y / ProximityCell),
                           FMath::FloorToInt(Position.Z / ProximityCell));
      for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
        for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
          for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
            const TArray<int32> *Bucket =
                ProximityGrid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
            if (!Bucket) {
              continue;
            }
            for (const int32 OtherVertex : *Bucket) {
              const int32 OtherIsland = Islands.IslandOfVertex[OtherVertex];
              if (OtherIsland == IslandIndex) {
                continue;
              }
              if (FVector3f::DotProduct(Out.Axis[IslandIndex], Out.Axis[OtherIsland]) <
                  CosMergeAngle) {
                continue;
              }
              const int32 ProximityLowIsland = FMath::Min(IslandIndex, OtherIsland);
              const int32 ProximityHighIsland = FMath::Max(IslandIndex, OtherIsland);
              const uint64 ProximityPairKey = AssetHivePlantWind::PlantWindUnitPairKey(
                  ProximityLowIsland, ProximityHighIsland);
              if (SeenPairs.Contains(ProximityPairKey)) {
                continue;
              }
              const float ProximityDistanceSquared =
                  FVector3f::DistSquared(Position, GetPosition(OtherVertex));
              if (ProximityDistanceSquared > ProximitySquared) {
                continue;
              }
              SeenPairs.Add(ProximityPairKey);
              FProximityCandidate NewCandidate;
              NewCandidate.IslandA = ProximityLowIsland;
              NewCandidate.IslandB = ProximityHighIsland;
              NewCandidate.ProximityDistanceSquared = ProximityDistanceSquared;
              Candidates.Add(NewCandidate);
            }
          }
        }
      }
      ProximityGrid.FindOrAdd(Key).Add(VertexIndex);
    }
    Candidates.Sort([](const FProximityCandidate &Left, const FProximityCandidate &Right) {
      return Left.ProximityDistanceSquared < Right.ProximityDistanceSquared;
    });

    auto IslandProjectionRange = [&](int32 IslandIndex, int32 AnchorIsland,
                                     float &OutLow, float &OutHigh) {
      const FVector3f &Anchor = Out.Base[AnchorIsland];
      const FVector3f &Axis = Out.Axis[AnchorIsland];
      OutLow = TNumericLimits<float>::Max();
      OutHigh = -TNumericLimits<float>::Max();
      for (const int32 VertexIndex : IslandVertices[IslandIndex]) {
        const float Projection = FVector3f::DotProduct(GetPosition(VertexIndex) - Anchor, Axis);
        OutLow = FMath::Min(OutLow, Projection);
        OutHigh = FMath::Max(OutHigh, Projection);
      }
    };

    TArray<int32> ProximityRootAnchor;
    TArray<int32> ProximityRootRachisCount;
    ProximityRootAnchor.Init(INDEX_NONE, IslandCount);
    ProximityRootRachisCount.Init(0, IslandCount);
    for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
      ProximityRootAnchor[IslandIndex] = IslandIndex;
      ProximityRootRachisCount[IslandIndex] = Out.Rachis[IslandIndex] != 0 ? 1 : 0;
    }
    for (const FProximityCandidate &ProximityCandidate : Candidates) {
      const int32 ProximityRootA = FindRoot(ProximityCandidate.IslandA);
      const int32 ProximityRootB = FindRoot(ProximityCandidate.IslandB);
      if (ProximityRootA == ProximityRootB) {
        continue;
      }
      if (ProximityRootRachisCount[ProximityRootA] + ProximityRootRachisCount[ProximityRootB] > ProximityMergeMaxRachis) {
        Out.ProximityRejectedByRachis += 1;
        continue;
      }
      const int32 AnchorA = ProximityRootAnchor[ProximityRootA];
      const int32 AnchorB = ProximityRootAnchor[ProximityRootB];
      if (!Out.Length.IsValidIndex(AnchorA) || !Out.Length.IsValidIndex(AnchorB)) {
        continue;
      }
      const int32 NewAnchor =
          Out.Length[AnchorA] >= Out.Length[AnchorB] ? AnchorA : AnchorB;
      // 并集在新锚轴上的跨度，与"两片各自沿自身轴的跨度之和"比较，消除换尺子效应。
      float OwnLowA = 0.0f, OwnHighA = 0.0f, OwnLowB = 0.0f, OwnHighB = 0.0f;
      IslandProjectionRange(AnchorA, AnchorA, OwnLowA, OwnHighA);
      IslandProjectionRange(AnchorB, AnchorB, OwnLowB, OwnHighB);
      const float OwnSpanSum =
          FMath::Max((OwnHighA - OwnLowA) + (OwnHighB - OwnLowB), 1e-6f);
      float ProximitySpanLow = TNumericLimits<float>::Max();
      float ProximitySpanHigh = -TNumericLimits<float>::Max();
      for (const int32 IslandIndex : {AnchorA, AnchorB}) {
        float Low = 0.0f;
        float High = 0.0f;
        IslandProjectionRange(IslandIndex, NewAnchor, Low, High);
        ProximitySpanLow = FMath::Min(ProximitySpanLow, Low);
        ProximitySpanHigh = FMath::Max(ProximitySpanHigh, High);
      }
      if (ProximitySpanHigh - ProximitySpanLow > OwnSpanSum * ProximityMergeSpanRatio) {
        Out.ProximityRejectedBySpan += 1;
        continue;
      }
      Parents[ProximityRootB] = ProximityRootA;
      ProximityRootAnchor[ProximityRootA] = NewAnchor;
      ProximityRootRachisCount[ProximityRootA] += ProximityRootRachisCount[ProximityRootB];
      Out.ProximityMerges += 1;
    }
  }

  // F4 簇参数：方向 = 簇内最长 rachis 轴（无 rachis 取最长片轴）；锚点 = rachis 基
  //（无 rachis 取最低 25% 顶点质心）；权重 = 沿簇轴投影 0..1。
  TArray<TArray<int32>> ClusterMembers;
  {
    TArray<int32> ClusterByRoot;
    ClusterByRoot.Init(INDEX_NONE, IslandCount);
    for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
      const int32 Root = FindRoot(IslandIndex);
      if (!ClusterByRoot.IsValidIndex(Root)) {
        continue;
      }
      if (ClusterByRoot[Root] == INDEX_NONE) {
        ClusterByRoot[Root] = ClusterMembers.AddDefaulted();
      }
      ClusterMembers[ClusterByRoot[Root]].Add(IslandIndex);
    }
  }
  const int32 ClusterCount = ClusterMembers.Num();
  Out.ClusterOfIsland.Init(INDEX_NONE, IslandCount);
  Out.ClusterBase.SetNum(ClusterCount);
  Out.ClusterDirection.SetNum(ClusterCount);
  Out.ClusterWindPosition.SetNum(ClusterCount);
  Out.ClusterMaxProjection.Init(0.0f, ClusterCount);
  for (int32 ClusterIndex = 0; ClusterIndex < ClusterCount; ++ClusterIndex) {
    const TArray<int32> &Members = ClusterMembers[ClusterIndex];
    int32 RachisIsland = INDEX_NONE;
    int32 LongestIsland = Members.Num() > 0 ? Members[0] : INDEX_NONE;
    for (const int32 IslandIndex : Members) {
      Out.ClusterOfIsland[IslandIndex] = ClusterIndex;
      if (Out.Rachis[IslandIndex] != 0 &&
          (RachisIsland == INDEX_NONE || Out.Length[IslandIndex] > Out.Length[RachisIsland])) {
        RachisIsland = IslandIndex;
      }
      if (LongestIsland != INDEX_NONE && Out.Length[IslandIndex] > Out.Length[LongestIsland]) {
        LongestIsland = IslandIndex;
      }
    }
    if (LongestIsland == INDEX_NONE) {
      continue;
    }
    const int32 AnchorIsland = RachisIsland != INDEX_NONE ? RachisIsland : LongestIsland;
    FVector3f Anchor = Out.Base[AnchorIsland];
    FVector3f Basis = Out.Axis[AnchorIsland];
    if (RachisIsland == INDEX_NONE) {
      float MinZ = TNumericLimits<float>::Max();
      float MaxZ = -TNumericLimits<float>::Max();
      for (const int32 IslandIndex : Members) {
        for (const int32 VertexIndex : IslandVertices[IslandIndex]) {
          const float Z = GetPosition(VertexIndex).Z;
          MinZ = FMath::Min(MinZ, Z);
          MaxZ = FMath::Max(MaxZ, Z);
        }
      }
      const float HeightRange = FMath::Max(MaxZ - MinZ, 1e-6f);
      FVector3d LowSum = FVector3d::ZeroVector;
      int32 LowCount = 0;
      for (const int32 IslandIndex : Members) {
        for (const int32 VertexIndex : IslandVertices[IslandIndex]) {
          const FVector3f Position = GetPosition(VertexIndex);
          if (Position.Z <= MinZ + HeightRange * 0.25f) {
            LowSum += FVector3d(Position);
            LowCount += 1;
          }
        }
      }
      Anchor = LowCount > 0 ? FVector3f(LowSum / LowCount) : Out.Base[LongestIsland];
      Basis = Out.Axis[LongestIsland];
    }
    Out.ClusterBase[ClusterIndex] = Anchor;
    Out.ClusterDirection[ClusterIndex] = Basis;
    Out.ClusterWindPosition[ClusterIndex] = Islands.TipPosition.IsValidIndex(AnchorIsland)
                                                 ? Islands.TipPosition[AnchorIsland]
                                                 : Out.Tip[AnchorIsland];
    float ClusterMaxProjection = 0.0f;
    for (const int32 IslandIndex : Members) {
      for (const int32 VertexIndex : IslandVertices[IslandIndex]) {
        ClusterMaxProjection = FMath::Max(
            ClusterMaxProjection,
            FVector3f::DotProduct(GetPosition(VertexIndex) - Anchor, Basis));
      }
    }
    Out.ClusterMaxProjection[ClusterIndex] = FMath::Max(ClusterMaxProjection, 1e-6f);
  }
  Out.ClusterCount = ClusterCount;
  for (const TArray<int32> &Members : ClusterMembers) {
    Out.LargestClusterIslands = FMath::Max(Out.LargestClusterIslands, Members.Num());
  }
  Out.LargestClusterShare = IslandCount > 0
                                ? static_cast<float>(Out.LargestClusterIslands) / static_cast<float>(IslandCount)
                                : 0.0f;

  // 回退判据：结构归组几乎不成立且几乎全是细长 rachis -> 逐片独立相位 + 基部接缝焊接。
  //
  // 邻近合并必须计入分子（2026-10-10）：Lady_Fern_h9ecnwd_14 的 3 个连通片全是 rachis
  // （RachisShare = 1.0）而 F3 合并对数为 0，原式必然触发回退——**这张资产从来没走过结构归组**，
  // 叶轴与子叶各自成风单元、锚点相距 5.3cm，"茎秆叶片分离"就是这么来的。邻近合并既然把
  // "贴合且同向"的片并了起来，就说明这个资产的结构归组是有效的，不应再回退。
  const float EffectiveMergeRatio =
      Out.CandidatePairCount > 0
          ? static_cast<float>(Out.MergedPairCount + Out.ProximityMerges) /
                static_cast<float>(Out.CandidatePairCount)
          : 0.0f;
  const float RachisShare = static_cast<float>(Out.RachisCount) / static_cast<float>(IslandCount);
  Out.bFallbackPerIsland = EffectiveMergeRatio < 0.15f && RachisShare > 0.70f;
  if (Out.bFallbackPerIsland) {
    Out.StructuralIslandPairs = FallbackStructuralPairs;
    Out.ClusterOfIsland.SetNum(IslandCount);
    Out.ClusterBase.SetNum(IslandCount);
    Out.ClusterDirection.SetNum(IslandCount);
    Out.ClusterWindPosition.SetNum(IslandCount);
    Out.ClusterMaxProjection.SetNum(IslandCount);
    for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
      Out.ClusterOfIsland[IslandIndex] = IslandIndex;
      Out.ClusterBase[IslandIndex] = Out.Base[IslandIndex];
      Out.ClusterDirection[IslandIndex] = Out.Axis[IslandIndex];
      Out.ClusterWindPosition[IslandIndex] =
          Islands.TipPosition.IsValidIndex(IslandIndex) ? Islands.TipPosition[IslandIndex]
                                                        : Out.Tip[IslandIndex];
      Out.ClusterMaxProjection[IslandIndex] = FMath::Max(Out.MaxProjection[IslandIndex], 1e-6f);
    }
    Out.ClusterCount = IslandCount;
  }
  for (const uint64 PairKey : Out.StructuralIslandPairs) {
    const int32 IslandA = static_cast<int32>(PairKey >> 32);
    const int32 IslandB = static_cast<int32>(PairKey & 0xFFFFFFFFull);
    if (!Out.ClusterOfIsland.IsValidIndex(IslandA) || !Out.ClusterOfIsland.IsValidIndex(IslandB)) {
      continue;
    }
    const int32 ClusterA = Out.ClusterOfIsland[IslandA];
    const int32 ClusterB = Out.ClusterOfIsland[IslandB];
    if (ClusterA == ClusterB) {
      continue;
    }
    Out.StructuralClusterPairs.Add(AssetHivePlantWind::PlantWindUnitPairKey(ClusterA, ClusterB));
  }
  return true;
}

static bool ComputePlantWindFronds(FMeshDescription &Mesh,
                                   const FPlantWindIslandParameters &Islands,
                                   EPlantWindFrondMergeMode MergeMode,
                                   FPlantWindFrondParameters &Out,
                                   FPlantWindFernStructure *OutFernStructure = nullptr) {
  Out = FPlantWindFrondParameters();
  const int32 IslandCount = Islands.Num();
  const int32 VertexCount = Mesh.Vertices().Num();
  if (IslandCount <= 0 || VertexCount <= 0 ||
      Islands.IslandOfVertex.Num() < VertexCount) {
    return false;
  }
  FStaticMeshAttributes Attributes(Mesh);
  Attributes.Register(true);
  TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();

  // 合并模式：
  //   ConnectedCluster（Fern）—— F1–F6 结构化推导：逐片 PCA 结构轴 + rachis/leaflet 类型 +
  //     结构邻接归组（贴合/基部附着/轴向外延）+ 簇参数（方向/锚点/沿轴权重）+ F5 结构接缝焊接
  //     + 每簇哈希相位。旧版（v6/v7）的“core + satellite 径向方位”启发式已被 F1–F6 取代：
  //     旧版按 1–2cm 接触全量合并会把冠部相接的蕨类整株并成一个风单元（“整株蒲扇式倒伏”），
  //     v7 的 core 判定又依赖径向方位，无法处理多枝叶朝外延伸类资产（蕨类/小灌木/丛状半球草本）。
  //   FrondLike（Grass）—— 主籽 + 短子叶的连通簇合并（下面 Grass 路径）。
  //   None（Bush）—— 逐片记录，branch2 走 UV3。
  if (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster) {
    // Fern 结构化风推导（2026-10-09 定稿 F1–F6；离线原型 fern-rule5.cjs，24 个 Megascans
    // 植被样本全量回归）：逐片 PCA 结构轴 -> rachis/leaflet 类型 -> 结构邻接归组（贴合 / 基部
    // 附着 / 轴向外延，硬约束 = 每簇 <=2 条 rachis 且纯平行贴合双轴不并 + 簇跨度上限）->
    // 簇参数（方向 = 簇内最长 rachis 轴，锚点 = rachis 基，权重 = 沿簇轴投影）-> F5 结构接缝
    // 焊接（BlendPlantWindSeams）-> 每簇哈希相位（方向抖动 + ripple）。
    // 不透明确切（_OPAQUE）网格可能有数万个碎片，per-frond 推导过重；该路径由 CardReference
    // 覆盖，找不到参考时回退到逐片参数。
    constexpr int32 MaxFernIslandsForPerFrond = 2048;
    if (IslandCount > MaxFernIslandsForPerFrond) {
      return false;
    }
    FPlantWindFernStructure Structure;
    if (!ComputePlantWindFernStructure(
            VertexCount,
            [&Positions](int32 VertexIndex) {
              return Positions[FVertexID(VertexIndex)];
            },
            Islands, Structure)) {
      return false;
    }
    if (Structure.ClusterCount <= 0) {
      return false;
    }
    if (OutFernStructure != nullptr) {
      *OutFernStructure = Structure;
    }
    Out.FrondOfIsland.Init(INDEX_NONE, IslandCount);
    Out.BasePosition.SetNum(Structure.ClusterCount);
    Out.TipPosition.SetNum(Structure.ClusterCount);
    Out.Direction.SetNum(Structure.ClusterCount);
    Out.MaxProjection.Init(0.0f, Structure.ClusterCount);
    for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
      const int32 ClusterIndex =
          Structure.ClusterOfIsland.IsValidIndex(IslandIndex)
              ? Structure.ClusterOfIsland[IslandIndex]
              : INDEX_NONE;
      Out.FrondOfIsland[IslandIndex] = ClusterIndex;
      if (!Structure.ClusterBase.IsValidIndex(ClusterIndex)) {
        continue;
      }
      Out.BasePosition[ClusterIndex] = Structure.ClusterBase[ClusterIndex];
      Out.TipPosition[ClusterIndex] =
          Structure.ClusterWindPosition.IsValidIndex(ClusterIndex)
              ? Structure.ClusterWindPosition[ClusterIndex]
              : Structure.ClusterBase[ClusterIndex];
      Out.Direction[ClusterIndex] = Structure.ClusterDirection[ClusterIndex];
      Out.MaxProjection[ClusterIndex] =
          FMath::Max(Structure.ClusterMaxProjection[ClusterIndex], 1e-6f);
    }
    Out.ConsolidatedFrondCount = Structure.ClusterCount;
    Out.JoinDistance = Structure.JoinDistance;
    return Out.Num() > 0;
  }

  // ---- Grass / FrondLike 路径（Fern 已在上面走 F1–F6 结构归组）----
  // 连通片按跨片近邻合并：顶点距离 <= JoinDistance 视为同一结构（叶轴-叶柄交界）。
  TArray<int32> Parents;
  Parents.SetNumUninitialized(IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    Parents[IslandIndex] = IslandIndex;
  }
  auto FindRoot = [&Parents](int32 Index) {
    while (Parents[Index] != Index) {
      Parents[Index] = Parents[Parents[Index]];
      Index = Parents[Index];
    }
    return Index;
  };
  const FVector3f MeshSize = Islands.MeshMax - Islands.MeshMin;
  const float MeshDiagonal = MeshSize.Size();
  // 合并距离（2026-10-07 v6）：Grass frond 判定保持 v4 的小距离；Fern 的叶轴-羽片
  // 根部间隙实测 6–12mm，必须放宽到 ≥1.0cm 才能全量覆盖（离线回归：1.0cm 时所有
  // 测试蕨类的 ≤10mm 跨单元顶点对归零），上限 2.0cm 防止无关结构过度并入。
  const float JoinDistance =
      MergeMode == EPlantWindFrondMergeMode::ConnectedCluster
          ? FMath::Clamp(MeshDiagonal * 0.014f, 1.0f, 2.0f)
          : FMath::Clamp(MeshDiagonal * 0.004f, 0.1f, 0.5f);
  Out.JoinDistance = JoinDistance;
  {
    const float JoinDistanceSquared = JoinDistance * JoinDistance;
    const float GridCell = FMath::Max(JoinDistance, 1e-3f);
    TMap<FIntVector, TArray<int32>> VertexGrid;
    VertexGrid.Reserve(VertexCount);
    for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
      const FVector3f Position = Positions[FVertexID(VertexIndex)];
      const FIntVector Key(
          FMath::FloorToInt(Position.X / GridCell),
          FMath::FloorToInt(Position.Y / GridCell),
          FMath::FloorToInt(Position.Z / GridCell));
      const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
      for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
        for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
          for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
            const TArray<int32> *OtherVertices =
                VertexGrid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
            if (!OtherVertices) {
              continue;
            }
            for (const int32 OtherVertex : *OtherVertices) {
              const int32 OtherIsland = Islands.IslandOfVertex[OtherVertex];
              if (OtherIsland == IslandIndex ||
                  FVector3f::DistSquared(Position,
                                         Positions[FVertexID(OtherVertex)]) >
                      JoinDistanceSquared) {
                continue;
              }
              const int32 RootA = FindRoot(IslandIndex);
              const int32 RootB = FindRoot(OtherIsland);
              if (RootA != RootB) {
                Parents[RootB] = RootA;
              }
            }
          }
        }
      }
      VertexGrid.FindOrAdd(Key).Add(VertexIndex);
    }
  }

  // 2) 汇总每簇成员与包围盒（用于“叶片簇”结构判定）。
  TArray<int32> ClusterIndexByRoot;
  ClusterIndexByRoot.Init(INDEX_NONE, IslandCount);
  TArray<TArray<int32>> ClusterMembers;
  TArray<int32> ClusterOfIsland;
  ClusterOfIsland.Init(INDEX_NONE, IslandCount);
  for (int32 IslandIndex = 0; IslandIndex < IslandCount; ++IslandIndex) {
    const int32 Root = FindRoot(IslandIndex);
    int32 ClusterIndex = ClusterIndexByRoot[Root];
    if (ClusterIndex == INDEX_NONE) {
      ClusterIndex = ClusterMembers.AddDefaulted();
      ClusterIndexByRoot[Root] = ClusterIndex;
    }
    ClusterMembers[ClusterIndex].Add(IslandIndex);
    ClusterOfIsland[IslandIndex] = ClusterIndex;
  }
  TArray<FVector3f> ClusterMin;
  TArray<FVector3f> ClusterMax;
  ClusterMin.Init(FVector3f(TNumericLimits<float>::Max()), ClusterMembers.Num());
  ClusterMax.Init(FVector3f(-TNumericLimits<float>::Max()), ClusterMembers.Num());
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
    const int32 ClusterIndex =
        ClusterOfIsland.IsValidIndex(IslandIndex) ? ClusterOfIsland[IslandIndex]
                                                  : INDEX_NONE;
    if (!ClusterMin.IsValidIndex(ClusterIndex)) {
      continue;
    }
    const FVector3f Position = Positions[FVertexID(VertexIndex)];
    for (int32 Axis = 0; Axis < 3; ++Axis) {
      ClusterMin[ClusterIndex][Axis] =
          FMath::Min(ClusterMin[ClusterIndex][Axis], Position[Axis]);
      ClusterMax[ClusterIndex][Axis] =
          FMath::Max(ClusterMax[ClusterIndex][Axis], Position[Axis]);
    }
  }

  // 3) 结构判定 + 建立风单元：主轴长度至少为参考片长的 2 倍（>=3 片取中位数、2 片取较短者），
  //    且簇跨度不超过主轴长度的 1.8 倍（排除把整株/整丛连成一簇的过度合并）。
  Out.FrondOfIsland.Init(INDEX_NONE, IslandCount);
  auto AddFrond = [&Out](const FVector3f &BasePosition,
                         const FVector3f &WindPosition,
                         const FVector3f &Direction) {
    const int32 FrondIndex = Out.BasePosition.Add(BasePosition);
    Out.TipPosition.Add(WindPosition);
    Out.Direction.Add(Direction);
    Out.MaxProjection.Add(0.0f);
    return FrondIndex;
  };
  for (int32 ClusterIndex = 0; ClusterIndex < ClusterMembers.Num(); ++ClusterIndex) {
    const TArray<int32> &Members = ClusterMembers[ClusterIndex];
    int32 LeadIsland = Members[0];
    for (const int32 IslandIndex : Members) {
      if (Islands.MaxProjection[IslandIndex] >
          Islands.MaxProjection[LeadIsland]) {
        LeadIsland = IslandIndex;
      }
    }
    // Fern：接触连通簇无条件合并；Grass：按“主轴 + 短子叶”结构判定；其余不合并。
    bool bFrondLike = MergeMode == EPlantWindFrondMergeMode::ConnectedCluster;
    if (!bFrondLike && Members.Num() >= 2) {
      TArray<float> Lengths;
      Lengths.Reserve(Members.Num());
      for (const int32 IslandIndex : Members) {
        Lengths.Add(Islands.MaxProjection[IslandIndex]);
      }
      Lengths.Sort();
      const float MaxLength = Lengths.Last();
      const float ReferenceLength =
          Members.Num() >= 3 ? Lengths[Lengths.Num() / 2] : Lengths[0];
      const float ClusterSpan =
          (ClusterMax[ClusterIndex] - ClusterMin[ClusterIndex]).Size();
      bFrondLike =
          MaxLength >= 2.0f * FMath::Max(ReferenceLength, KINDA_SMALL_NUMBER) &&
          ClusterSpan <= MaxLength * 1.8f;
    }
    if (bFrondLike) {
      const int32 FrondIndex = AddFrond(
          Islands.BasePosition[LeadIsland],
          Islands.TipPosition.IsValidIndex(LeadIsland)
              ? Islands.TipPosition[LeadIsland]
              : Islands.BasePosition[LeadIsland],
          Islands.Direction[LeadIsland]);
      for (const int32 IslandIndex : Members) {
        Out.FrondOfIsland[IslandIndex] = FrondIndex;
      }
      Out.ConsolidatedFrondCount += 1;
    } else {
      for (const int32 IslandIndex : Members) {
        Out.FrondOfIsland[IslandIndex] = AddFrond(
            Islands.BasePosition[IslandIndex],
            Islands.TipPosition.IsValidIndex(IslandIndex)
                ? Islands.TipPosition[IslandIndex]
                : Islands.BasePosition[IslandIndex],
            Islands.Direction[IslandIndex]);
      }
    }
  }

  // 4) 每个风单元的沿轴最大投影（权重 0..1 的归一化尺度）。
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    const int32 IslandIndex = Islands.IslandOfVertex[VertexIndex];
    const int32 FrondIndex =
        Out.FrondOfIsland.IsValidIndex(IslandIndex) ? Out.FrondOfIsland[IslandIndex]
                                                    : INDEX_NONE;
    if (!Out.BasePosition.IsValidIndex(FrondIndex)) {
      continue;
    }
    const float Projection = FVector3f::DotProduct(
        Positions[FVertexID(VertexIndex)] - Out.BasePosition[FrondIndex],
        Out.Direction[FrondIndex]);
    Out.MaxProjection[FrondIndex] =
        FMath::Max(Out.MaxProjection[FrondIndex], Projection);
  }
  return Out.Num() > 0;
}

// 裁切（_OPAQUE）植被导出：同目录的未裁切 FBX 就是原始卡片来源
// （SM_x_01_OPAQUE.fbx -> SM_x_01.fbx）。找不到时返回空串，调用方回退到碎片级推导。
static FString ResolveOpaquePlantWindReferenceFile(const FString &CutSourceFile) {
  if (CutSourceFile.IsEmpty()) {
    return FString();
  }
  const FString Stem = FPaths::GetBaseFilename(CutSourceFile);
  if (!Stem.ToUpper().EndsWith(TEXT("OPAQUE"))) {
    return FString();
  }
  FString Trimmed = Stem.LeftChop(6);  // 去掉 OPAQUE
  while (!Trimmed.IsEmpty() &&
         (Trimmed.EndsWith(TEXT("_")) || Trimmed.EndsWith(TEXT("-")) ||
          Trimmed.EndsWith(TEXT(".")))) {
    Trimmed.LeftChopInline(1);
  }
  if (Trimmed.IsEmpty()) {
    return FString();
  }
  FString Extension = FPaths::GetExtension(CutSourceFile, /*bIncludeDot=*/true);
  if (Extension.IsEmpty()) {
    Extension = TEXT(".fbx");
  }
  const FString Candidate =
      FPaths::GetPath(CutSourceFile) / (Trimmed + Extension);
  return FPaths::FileExists(Candidate) ? Candidate : FString();
}

// 合成风参考：从未裁切的原始卡片网格推导卡片参数与最近点查询结构。
// 只有裁切（_OPAQUE）网格需要它——Masked 网格自身的连通片就是卡片。
struct FPlantWindCardReference {
  TArray<AssetHivePlantWind::FPlantWindCard> Cards;
  AssetHivePlantWind::FPlantWindCardSurface Surface;
  // SpeedTree branch1 权重按整簇高度归一化（ST9 实测）。不透明裁切网格只剩上部叶片，
  // 自身包围盒会丢掉根部高度，必须沿用来源（未裁切）网格的 MeshMin/MeshMax。
  FVector3f MeshMin = FVector3f::ZeroVector;
  FVector3f MeshMax = FVector3f::ZeroVector;
  // Fern 结构簇之间的结构邻接（卡片索引对）。不透明裁切网格的风属性取自来源结构簇（卡片），
  // 因此裁切网格的 F5 焊接也必须以「卡片」为风单元，并用这套来源结构对判定无条件焊接接缝；
  // 否则裁切网格自身的 F1–F6 结构与来源属性错位，茎秆-子叶等铰接处在高权重区漏焊（branch1 拉扯）。
  TSet<uint64> StructuralCardPairs;

  bool IsValid() const { return Cards.Num() > 0 && Surface.IsValid(); }

  bool HasBounds() const {
    return MeshMax.Z - MeshMin.Z > KINDA_SMALL_NUMBER;
  }

  void Reset() {
    Cards.Reset();
    StructuralCardPairs.Reset();
    Surface = AssetHivePlantWind::FPlantWindCardSurface();
    MeshMin = FVector3f::ZeroVector;
    MeshMax = FVector3f::ZeroVector;
  }
};

// 裁切网格与原始卡片必须处于同一局部空间与缩放：包围盒对角线出现数量级差异时放弃参考
// （单位/缩放不一致会让最近点匹配选到错误的卡片）。
static bool IsPlantWindCardReferenceScaleCompatible(
    const UStaticMesh &TargetMesh,
    const FPlantWindCardReference &Reference) {
  if (!Reference.IsValid()) {
    return false;
  }
  const FBox TargetBounds = TargetMesh.GetBoundingBox();
  const float TargetDiagonal = static_cast<float>(TargetBounds.GetSize().Size());
  const float ReferenceDiagonal = Reference.Surface.GetDiagonal();
  if (TargetDiagonal <= KINDA_SMALL_NUMBER ||
      ReferenceDiagonal <= KINDA_SMALL_NUMBER) {
    return false;
  }
  const float Ratio = ReferenceDiagonal / TargetDiagonal;
  return Ratio >= 0.4f && Ratio <= 2.5f;
}

// v8?2026-10-07???? FBX ????????
// Blender / three.js ????????? FBX?_OPAQUE?? GlobalSettings ??
// ???? FBX ???FrontAxisSign / CoordAxisSign ????UE ???????
// ???????????? 180???? UV0 ??? 100% ??????????
// ??????????????????????????? / ???
// ????UV0 ?????????? 0?/90?/180?/270? ????????????
// ????????????? > 0.5cm??????????< 0.5cm ? < 50%?
// ??????????????? UV ???????
static FVector3f RotatePlantWindYaw(const FVector3f &Value, float Degrees) {
  const float Radians = FMath::DegreesToRadians(Degrees);
  const float CosValue = FMath::Cos(Radians);
  const float SinValue = FMath::Sin(Radians);
  return FVector3f(CosValue * Value.X - SinValue * Value.Y,
                   SinValue * Value.X + CosValue * Value.Y, Value.Z);
}

static float NormalizePlantWindCardReferenceYaw(
    FMeshDescription &CutMesh, FPlantWindCardReference &Reference) {
  if (!Reference.IsValid() || Reference.Surface.Positions.Num() == 0) {
    return 0.0f;
  }
  FStaticMeshAttributes Attributes(CutMesh);
  Attributes.Register(true);
  TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
  if (UVs.GetNumChannels() < 1) {
    return 0.0f;
  }
  const int32 VertexInstanceCount = CutMesh.VertexInstances().Num();
  if (VertexInstanceCount == 0) {
    return 0.0f;
  }
  constexpr int32 SampleTarget = 3000;
  const int32 SampleStride = FMath::Max(1, VertexInstanceCount / SampleTarget);
  constexpr int32 YawCount = 4;
  const float YawDegrees[YawCount] = {0.0f, 90.0f, 180.0f, 270.0f};
  double DistanceSum[YawCount] = {0.0, 0.0, 0.0, 0.0};
  int32 DistanceSamples[YawCount] = {0, 0, 0, 0};
  for (int32 InstanceIndex = 0; InstanceIndex < VertexInstanceCount;
       InstanceIndex += SampleStride) {
    const FVertexInstanceID InstanceID(InstanceIndex);
    const FVertexID VertexID = CutMesh.GetVertexInstanceVertex(InstanceID);
    const FVector3f Position = Positions[VertexID];
    const FVector2f UV = UVs.Get(InstanceID, 0);
    for (int32 YawIndex = 0; YawIndex < YawCount; ++YawIndex) {
      // ????? -? ????????? +??????????????????
      const FVector3f Query = RotatePlantWindYaw(Position, -YawDegrees[YawIndex]);
      float Distance = 0.0f;
      if (Reference.Surface.FindCardByUV(UV, Query, Distance) != INDEX_NONE) {
        DistanceSum[YawIndex] += Distance;
        DistanceSamples[YawIndex] += 1;
      }
    }
  }
  for (int32 YawIndex = 0; YawIndex < YawCount; ++YawIndex) {
    if (DistanceSamples[YawIndex] <= 0) {
      return 0.0f;
    }
  }
  int32 BestYawIndex = 0;
  double BestMean = DistanceSum[0] / DistanceSamples[0];
  const double IdentityMean = BestMean;
  for (int32 YawIndex = 1; YawIndex < YawCount; ++YawIndex) {
    const double Mean = DistanceSum[YawIndex] / DistanceSamples[YawIndex];
    if (Mean < BestMean) {
      BestMean = Mean;
      BestYawIndex = YawIndex;
    }
  }
  if (BestYawIndex == 0) {
    if (IdentityMean > 0.5) {
      // ????????????????????????????
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive import: ????? FBX ????? UV0 ?????? %.3f cm?????????????????"),
             static_cast<float>(IdentityMean));
    }
    return 0.0f;
  }
  if (IdentityMean <= 0.5 || BestMean >= 0.5 || BestMean >= IdentityMean * 0.5) {
    return 0.0f;
  }
  const float AppliedYaw = YawDegrees[BestYawIndex];
  for (FVector3f &Position : Reference.Surface.Positions) {
    Position = RotatePlantWindYaw(Position, AppliedYaw);
  }
  for (AssetHivePlantWind::FPlantWindCard &Card : Reference.Cards) {
    Card.BasePosition = RotatePlantWindYaw(Card.BasePosition, AppliedYaw);
    Card.Direction = RotatePlantWindYaw(Card.Direction, AppliedYaw);
  }
  Reference.Surface.BuildGrid();
  return AppliedYaw;
}

static bool BuildPlantWindCardReference(const UStaticMesh *ReferenceMesh,
                                        EPlantWindFrondMergeMode MergeMode,
                                        FPlantWindCardReference &Out) {
  Out.Cards.Reset();
  Out.Surface.Positions.Reset();
  Out.Surface.TriangleIndices.Reset();
  Out.Surface.CardOfTriangle.Reset();
  Out.Surface.TriangleGrid.Reset();
  Out.Surface.CoarseTriangleGrid.Reset();
  Out.Surface.TriangleUVs.Reset();
  Out.Surface.UVTriangleGrid.Reset();
  Out.StructuralCardPairs.Reset();
  if (!ReferenceMesh || ReferenceMesh->GetNumSourceModels() == 0) {
    return false;
  }
  FMeshDescription *Mesh = ReferenceMesh->GetMeshDescription(0);
  if (!Mesh) {
    return false;
  }
  FPlantWindIslandParameters IslandParameters;
  if (!ComputePlantWindIslands(*Mesh, IslandParameters)) {
    return false;
  }
  Out.MeshMin = IslandParameters.MeshMin;
  Out.MeshMax = IslandParameters.MeshMax;
  // Grass（frond 判定）/ Fern（接触连通簇）：卡片按 SpeedTree frond 规则构建——叶轴 + 其子叶
  // 合并成同一张卡（共享锚点/方向/沿轴权重场）。裁切碎片经 UV0 反查到卡后即取同一套参数。
  FPlantWindFrondParameters FrondParameters;
  // Fern（ConnectedCluster）用同一套 F1–F6 结构簇参数构建参考卡片，保证裁切网格映射回卡片后
  // 的锚点/方向/权重场与源网格一致。
  FPlantWindFernStructure FernStructure;
  const bool bHasFronds =
      MergeMode != EPlantWindFrondMergeMode::None &&
      ComputePlantWindFronds(*Mesh, IslandParameters, MergeMode, FrondParameters,
                             &FernStructure) &&
      FrondParameters.Num() > 0;
  FStaticMeshAttributes Attributes(*Mesh);
  Attributes.Register(true);
  TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
  const bool bHasUV0 = UVs.GetNumChannels() > 0;
  const int32 VertexCount = Mesh->Vertices().Num();
  const int32 CardCount = bHasFronds ? FrondParameters.Num() : IslandParameters.Num();
  Out.Cards.SetNum(CardCount);
  for (int32 CardIndex = 0; CardIndex < CardCount; ++CardIndex) {
    AssetHivePlantWind::FPlantWindCard &Card = Out.Cards[CardIndex];
    if (bHasFronds) {
      Card.BasePosition = FrondParameters.BasePosition[CardIndex];
      Card.WindPosition =
          FrondParameters.TipPosition.IsValidIndex(CardIndex)
              ? FrondParameters.TipPosition[CardIndex]
              : FrondParameters.BasePosition[CardIndex];
      Card.Direction = FrondParameters.Direction[CardIndex];
      Card.MaxProjection = FrondParameters.MaxProjection[CardIndex];
    } else {
      Card.BasePosition = IslandParameters.BasePosition[CardIndex];
      Card.WindPosition =
          IslandParameters.TipPosition.IsValidIndex(CardIndex)
              ? IslandParameters.TipPosition[CardIndex]
              : IslandParameters.BasePosition[CardIndex];
      Card.Direction = IslandParameters.Direction[CardIndex];
      Card.MaxProjection = IslandParameters.MaxProjection[CardIndex];
    }
  }
  // 来源结构簇（= 卡片）之间的结构邻接：裁切网格按卡片作 wind unit 时复用这套「必焊」集合。
  Out.StructuralCardPairs.Reset();
  if (bHasFronds) {
    Out.StructuralCardPairs.Reserve(FernStructure.StructuralClusterPairs.Num());
    for (const uint64 PairKey : FernStructure.StructuralClusterPairs) {
      Out.StructuralCardPairs.Add(PairKey);
    }
  }
  Out.Surface.Positions.SetNumUninitialized(VertexCount);
  for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
    Out.Surface.Positions[VertexIndex] = Positions[FVertexID(VertexIndex)];
  }
  Out.Surface.TriangleIndices.Reserve(Mesh->Triangles().Num() * 3);
  Out.Surface.CardOfTriangle.Reserve(Mesh->Triangles().Num());
  for (const FTriangleID TriangleID : Mesh->Triangles().GetElementIDs()) {
    const TArrayView<const FVertexID> Corners =
        Mesh->GetTriangleVertices(TriangleID);
    if (Corners.Num() < 3) {
      continue;
    }
    for (int32 Corner = 0; Corner < 3; ++Corner) {
      Out.Surface.TriangleIndices.Add(
          static_cast<uint32>(Corners[Corner].GetValue()));
    }
    const int32 CornerIsland =
        IslandParameters.IslandOfVertex[Corners[0].GetValue()];
    Out.Surface.CardOfTriangle.Add(
        bHasFronds && FrondParameters.FrondOfIsland.IsValidIndex(CornerIsland)
            ? FrondParameters.FrondOfIsland[CornerIsland]
            : CornerIsland);
    if (bHasUV0) {
      const TArrayView<const FVertexInstanceID> CornersInstances =
          Mesh->GetTriangleVertexInstances(TriangleID);
      if (CornersInstances.Num() >= 3) {
        for (int32 Corner = 0; Corner < 3; ++Corner) {
          Out.Surface.TriangleUVs.Add(UVs.Get(CornersInstances[Corner], 0));
        }
      }
    }
  }
  if (Out.Surface.TriangleUVs.Num() != Out.Surface.CardOfTriangle.Num() * 3) {
    Out.Surface.TriangleUVs.Reset();
  }
  if (Out.Surface.TriangleIndices.Num() < 3) {
    Out.Cards.Reset();
    return false;
  }
  Out.Surface.BuildGrid();
  Out.Surface.BuildUVGrid();
  return true;
}
// 卡片尺度权重的分母下限（cm）。
//
// 权重 = 顶点在卡片轴上的投影 / MaxProjection，后者是「卡片沿自身轴向的跨度」。当一张卡片本身
// 只有 0.6~0.9cm 长（Megascans 的小叶片被裁切成短卡片时很常见），权重斜坡会在相邻顶点之间
// 走出 0.2~0.3 的落差——这是真实的连续梯度，紧贴的碎片之间不会裂开（位置重合顶点的位移差
// 实测 ≤0.0004cm），但整片叶子会在极短距离内被弯折，视觉上是「局部被拧住」。
// 给分母设 2cm 下限后权重梯度被限制在 1/2 每 cm，实测 Desert_Cotton_l9uj50a_07 跨接缝
// 最大位移差 0.52cm -> 0.19cm、超过 0.1 的边 113 -> 0，代价是 30% 的短叶片摆幅降到约 55%
// （这些叶片本身只有厘米级，观感影响小）。调大到 5cm 可进一步压到 0.08cm，但 66% 的卡片都会被影响。
// 下限只作用于卡片尺度权重（Fern/Bush）；Grass 走整簇高度归一化，不受影响。
constexpr float MinPlantWindWeightSpan = 2.0f;

// 合成 SpeedTree 风：写 branch1（UV1/UV2）与 branch2（UV3）数据。
// Grass（FrondLike）权重按 SpeedTree 原生推导：branch1 权重 = 顶点在整簇高度上的归一化位置
//   （ST9 实测 ThickGrassTall 的 weight 与 Z 完全线性，残差 ±0.01；单叶/横向草 weight 恒 0）。
// Fern/Bush 权重仍以“原始卡片”为尺度：Masked 网格自身的连通片就是卡片；不透明裁切（_OPAQUE）网格的碎片
//   远小于卡片，必须用 CardReference（未裁切 FBX 推导的卡片参数）映射回卡片再取权重，否则碎片内
//   权重瞬间走完 0..1，整片被拉成条状。没有参考时回退到按自身碎片推导（旧行为）。
//   裁切网格的卡片归属优先用 UV0 反查 CardReference（裁切网格完整继承源 UV0），UV 缺失或未命中
//   时回退 3D 最近三角形；两种路径都会再经下面的“近距一致性”分组统一到同一张卡。
// UV3 是 16383 进制编码，必须 32bit UV 精度，因此同时把该 LOD 的 UV 精度设为全精度。
static bool ApplySyntheticSpeedTreeWind(UStaticMesh *StaticMesh, bool bWriteBranch2,
                                        EPlantWindFrondMergeMode MergeMode,
                                        const FPlantWindCardReference *CardReference,
                                        FString &OutSummary) {
  OutSummary.Reset();
  if (!StaticMesh || StaticMesh->GetNumSourceModels() == 0) {
    return false;
  }

  struct FPlantWindIsland {
    FVector3f BasePosition = FVector3f::ZeroVector;
    // UV1.x（PackedPosition）编码用的位置：SpeedTree 实测为叶片顶部，不是基部。
    FVector3f WindPosition = FVector3f::ZeroVector;
    FVector3f Direction = FVector3f(0.0f, 0.0f, 1.0f);
    float PackedPosition = 0.0f;
    float PackedDirection = 0.0f;
    // UV2.g：SpeedTree 的 ripple 权重（逐风单元/逐叶）。
    float RippleWeight = 0.0f;
    int32 PositionCode = 0;
    int32 DirectionCode = 0;
    float MaxProjection = 1.0f;
  };

  const bool bUseCardReference = CardReference && CardReference->IsValid();
  const int32 RequiredChannels = bWriteBranch2 ? 4 : 3;
  int32 ProcessedLods = 0;
  int32 TotalIslands = 0;
  int32 TotalIslandsBeforeWeld = 0;
  int32 TotalWeldMergedVertices = 0;
  int32 TotalVertices = 0;
  int32 TotalContactBlendedVertices = 0;
  int32 TotalCardMappedVertices = 0;
  int32 TotalCardFallbackVertices = 0;
  int32 TotalCardMappedIslands = 0;
  int32 TotalCardFallbackIslands = 0;
  int32 RawCardHitVertices = 0;
  int32 TotalUVMigratedVertices = 0;
  float MaxCardMatchDistance = 0.0f;
  TSet<int32> UsedCards;
  int32 TotalConsistencyGroups = 0;
  int32 TotalConsistencyMergedIslands = 0;
  int32 TotalConsistencyMaxGroupIslands = 0;
  // 因组规模上限被放弃的合并次数（日志用）。
  int32 TotalConsistencyCappedUnions = 0;
  float TotalNearCardDistance = 0.0f;
  int32 TotalConsolidatedFronds = 0;
  int32 TotalFrondMergedIslands = 0;
  float TotalJoinDistanceMax = 0.0f;
  // Fern（F1–F6）统计：簇数 / rachis / 结构边 / 回退 LOD / 接缝焊接顶点。
  int32 TotalFernClusters = 0;
  int32 TotalFernRachis = 0;
  int32 TotalFernCandidatePairs = 0;
  int32 TotalFernMergedPairs = 0;
  int32 TotalFernRejectedSpan = 0;
  int32 TotalFernRejectedRachis = 0;
  int32 TotalFernFallbackLods = 0;
  int32 TotalFernStructuralSeams = 0;
  int32 TotalSeamWeldedVertices = 0;

  for (int32 LodIndex = 0; LodIndex < StaticMesh->GetNumSourceModels(); ++LodIndex) {
    FMeshDescription *Mesh = StaticMesh->GetMeshDescription(LodIndex);
    if (!Mesh || Mesh->Triangles().Num() == 0) {
      continue;
    }
    FStaticMeshAttributes Attributes(*Mesh);
    Attributes.Register(true);
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
    const int32 VertexCount = Mesh->Vertices().Num();
    if (VertexCount == 0) {
      continue;
    }
    FPlantWindIslandParameters IslandParameters;
    if (!ComputePlantWindIslands(*Mesh, IslandParameters)) {
      continue;
    }
    // Grass / Fern 按 SpeedTree frond 规则共享风单元；Bush（branch2->UV3）保留逐片记录。
    FPlantWindFrondParameters FrondParameters;
    // F1–F6 结构（Fern 专用）：簇参数 + 结构接缝集合，供下面的 F5 接缝焊接使用。
    FPlantWindFernStructure FernStructure;
    const bool bUseFronds =
        MergeMode != EPlantWindFrondMergeMode::None &&
        ComputePlantWindFronds(*Mesh, IslandParameters, MergeMode, FrondParameters,
                               &FernStructure) &&
        FrondParameters.Num() > 0;

    const FVector3f MeshSize = IslandParameters.MeshMax - IslandParameters.MeshMin;
    // Grass 高度归一化（SpeedTree 原生口径）：裁切网格继承源卡片空间坐标，但自身只保留
    // 上部叶片，直接用自身 bbox 会把整片压到接近 1（僵硬）；有来源参考时优先用来源包围盒。
    const FVector3f WindBoundsMin =
        (bUseCardReference && CardReference->HasBounds())
            ? CardReference->MeshMin
            : IslandParameters.MeshMin;
    const FVector3f WindBoundsMax =
        (bUseCardReference && CardReference->HasBounds())
            ? CardReference->MeshMax
            : IslandParameters.MeshMax;
    const float WindBoundsHeight =
        FMath::Max(WindBoundsMax.Z - WindBoundsMin.Z, KINDA_SMALL_NUMBER);
    const FVector3f SafeMeshSize(FMath::Max(MeshSize.X, KINDA_SMALL_NUMBER),
                                 FMath::Max(MeshSize.Y, KINDA_SMALL_NUMBER),
                                 FMath::Max(MeshSize.Z, KINDA_SMALL_NUMBER));
    // F6 每簇哈希相位：Fern / Grass 都要把各风单元的摆动方向错开（否则整株同步摆动）；
    // Bush 的 branch2 保留原方向编码。Fern 的相位锚点 = 结构簇基（rachis 基），同簇内叶轴与
    // 小叶共享同一相位，簇与簇之间相位独立 -> 叶间相对运动。
    const bool bJitterWindPhase =
        MergeMode == EPlantWindFrondMergeMode::FrondLike ||
        MergeMode == EPlantWindFrondMergeMode::ConnectedCluster;
    // UV2.g（RippleWeight）只对 frond 类（Grass/Fern）合成；Bush 的 branch2 走 UV3，保持 0。
    const bool bWriteRippleWeight =
        MergeMode == EPlantWindFrondMergeMode::FrondLike ||
        MergeMode == EPlantWindFrondMergeMode::ConnectedCluster;
    auto PackWindSource = [&IslandParameters, &SafeMeshSize, bWriteBranch2,
                           bJitterWindPhase,
                           bWriteRippleWeight](FPlantWindIsland &Source,
                                               const FVector3f &BasePosition,
                                               const FVector3f &WindPosition,
                                               const FVector3f &Direction,
                                               const FVector3f &PhaseAnchor,
                                               float MaxProjection) {
      Source.BasePosition = BasePosition;
      Source.WindPosition = WindPosition;
      Source.Direction = Direction;
      Source.MaxProjection = MaxProjection;
      const FVector3f Normalized(
          FMath::Clamp(
              (BasePosition.X - IslandParameters.MeshMin.X) / SafeMeshSize.X, 0.0f,
              1.0f),
          FMath::Clamp(
              (BasePosition.Y - IslandParameters.MeshMin.Y) / SafeMeshSize.Y, 0.0f,
              1.0f),
          FMath::Clamp(
              (BasePosition.Z - IslandParameters.MeshMin.Z) / SafeMeshSize.Z, 0.0f,
              1.0f));
      // UV1.x：SpeedTree 用「叶片顶部位置」编码（实测对齐），与基部的 Anchors 分开。
      const FVector3f NormalizedWind(
          FMath::Clamp(
              (WindPosition.X - IslandParameters.MeshMin.X) / SafeMeshSize.X, 0.0f,
              1.0f),
          FMath::Clamp(
              (WindPosition.Y - IslandParameters.MeshMin.Y) / SafeMeshSize.Y, 0.0f,
              1.0f),
          FMath::Clamp(
              (WindPosition.Z - IslandParameters.MeshMin.Z) / SafeMeshSize.Z, 0.0f,
              1.0f));
      Source.PackedPosition = AssetHivePlantWind::PackPosition(NormalizedWind);
      // UV1.y：SpeedTree 逐叶编码"生长方向"（同时决定振荡相位）。相位锚点 Grass=单叶、
      // Fern/Bush=叶片簇，保证同簇相邻片（叶轴/子叶）不掉相位差、单叶之间又能错开。
      const FVector3f WindDirection =
          bJitterWindPhase
              ? AssetHivePlantWind::JitterGrassWindDirection(PhaseAnchor, Direction)
              : Direction;
      Source.PackedDirection =
          AssetHivePlantWind::PackDirection(WindDirection, /*bQuantized=*/false);
      // UV2.g：ripple 权重（每片叶幅度不同 -> 打破"叶片粘住"）。
      Source.RippleWeight =
          bWriteRippleWeight
              ? AssetHivePlantWind::PackGrassRippleWeight(PhaseAnchor)
              : 0.0f;
      Source.PositionCode = AssetHivePlantWind::ToUV3Code(
          Source.PackedPosition, AssetHivePlantWind::PositionMin,
          AssetHivePlantWind::PositionRange, AssetHivePlantWind::UV3CodeMax);
      if (bWriteBranch2) {
        // UV3 只能落在 10bit 档位：单独做一次量化最佳拟合。
        Source.DirectionCode = AssetHivePlantWind::ToUV3Code(
            AssetHivePlantWind::PackDirection(Direction, /*bQuantized=*/true),
            AssetHivePlantWind::DirectionMin,
            AssetHivePlantWind::DirectionRange, AssetHivePlantWind::UV3CodeMax);
      }
    };

    TArray<FPlantWindIsland> Islands;
    Islands.SetNum(IslandParameters.Num());
    for (int32 IslandIndex = 0; IslandIndex < IslandParameters.Num(); ++IslandIndex) {
      const int32 FrondIndex =
          bUseFronds && FrondParameters.FrondOfIsland.IsValidIndex(IslandIndex)
              ? FrondParameters.FrondOfIsland[IslandIndex]
              : INDEX_NONE;
      // 相位锚点：Grass（FrondLike）按单叶锚点 -> 同簇叶片相位/ripple 各不相同；
      // Fern（ConnectedCluster）按叶片簇锚点 -> 叶轴与子叶共享相位，避免接缝分离。
      const bool bPerIslandPhase =
          MergeMode == EPlantWindFrondMergeMode::FrondLike &&
          IslandParameters.BasePosition.IsValidIndex(IslandIndex);
      const FVector3f PhaseAnchor =
          IslandParameters.BasePosition.IsValidIndex(IslandIndex)
              ? IslandParameters.BasePosition[IslandIndex]
              : Islands[IslandIndex].BasePosition;
      if (FrondParameters.BasePosition.IsValidIndex(FrondIndex)) {
        PackWindSource(
            Islands[IslandIndex], FrondParameters.BasePosition[FrondIndex],
            FrondParameters.TipPosition.IsValidIndex(FrondIndex)
                ? FrondParameters.TipPosition[FrondIndex]
                : FrondParameters.BasePosition[FrondIndex],
            FrondParameters.Direction[FrondIndex],
            bPerIslandPhase ? PhaseAnchor : FrondParameters.BasePosition[FrondIndex],
            FrondParameters.MaxProjection[FrondIndex]);
      } else {
        PackWindSource(
            Islands[IslandIndex], IslandParameters.BasePosition[IslandIndex],
            IslandParameters.TipPosition.IsValidIndex(IslandIndex)
                ? IslandParameters.TipPosition[IslandIndex]
                : IslandParameters.BasePosition[IslandIndex],
            IslandParameters.Direction[IslandIndex], PhaseAnchor,
            IslandParameters.MaxProjection[IslandIndex]);
      }
    }

    // 裁切网格：把顶点映射回原始卡片（最近三角形，距离≈0），权重改用卡片尺度。
    // 连通片表决：同一片（裁切碎片）内所有顶点统一使用同一个卡片。逐顶点写数据时个别顶点
    // 未命中会回退碎片级参数，同一片内出现两套锚点/方向/权重 -> WPO 顶点撕裂。
    TArray<int32> CardIndexByVertex;
    TArray<float> CardDistanceByVertex;
    TArray<int32> IslandCardIndex;
    TArray<int32> IslandResolvedCardIndex;
    TArray<FPlantWindIsland> PackedCards;
    // 本网格卡片映射回退的碎片数（卡片模式刚性判定用；0 = 全部碎片命中卡片）。
    int32 MeshCardFallbackIslands = 0;
    if (bUseCardReference) {
      PackedCards.SetNum(CardReference->Cards.Num());
      for (int32 CardIndex = 0; CardIndex < CardReference->Cards.Num(); ++CardIndex) {
        const AssetHivePlantWind::FPlantWindCard &Card =
            CardReference->Cards[CardIndex];
        PackWindSource(PackedCards[CardIndex], Card.BasePosition, Card.WindPosition,
                       Card.Direction, Card.BasePosition, Card.MaxProjection);
      }
      CardIndexByVertex.SetNumUninitialized(VertexCount);
      CardDistanceByVertex.SetNumUninitialized(VertexCount);
      // UV0 迁移（opaque 分组策略，2026-10-07）：裁切网格由源卡片裁出，UV0 与源网格一致
      // （实测命中 100%）。先用 UV0 反查卡片、在候选内用 3D 位置消歧，贴合双层卡片不会串层；
      // UV 缺失或未命中再回退 3D 最近三角形。两者随后统一走下面的一致性分组。
      const bool bHasCutUV0 = UVs.GetNumChannels() > 0;
      TArray<FVector2f> UV0ByVertex;
      TArray<bool> HasUV0ByVertex;
      if (bHasCutUV0) {
        UV0ByVertex.Init(FVector2f::ZeroVector, VertexCount);
        HasUV0ByVertex.Init(false, VertexCount);
        for (const FVertexInstanceID VertexInstanceID :
             Mesh->VertexInstances().GetElementIDs()) {
          const int32 InstanceVertexIndex =
              Mesh->GetVertexInstanceVertex(VertexInstanceID).GetValue();
          if (InstanceVertexIndex < 0 || InstanceVertexIndex >= VertexCount ||
              HasUV0ByVertex[InstanceVertexIndex]) {
            continue;
          }
          UV0ByVertex[InstanceVertexIndex] = UVs.Get(VertexInstanceID, 0);
          HasUV0ByVertex[InstanceVertexIndex] = true;
        }
      }
      for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
        const FVertexID VertexID(VertexIndex);
        float Distance = 0.0f;
        int32 CardIndex = INDEX_NONE;
        if (bHasCutUV0 && HasUV0ByVertex[VertexIndex]) {
          CardIndex = CardReference->Surface.FindCardByUV(
              UV0ByVertex[VertexIndex], Positions[VertexID], Distance);
          if (CardIndex != INDEX_NONE) {
            TotalUVMigratedVertices += 1;
          }
        }
        if (CardIndex == INDEX_NONE) {
          const int32 Triangle =
              CardReference->Surface.FindNearestTriangle(Positions[VertexID],
                                                         Distance);
          if (Triangle != INDEX_NONE) {
            CardIndex = CardReference->Surface.CardOfTriangle[Triangle];
          }
        }
        CardIndexByVertex[VertexIndex] =
            PackedCards.IsValidIndex(CardIndex) ? CardIndex : INDEX_NONE;
        CardDistanceByVertex[VertexIndex] = Distance;
        if (CardIndexByVertex[VertexIndex] != INDEX_NONE) {
          RawCardHitVertices += 1;
          MaxCardMatchDistance = FMath::Max(MaxCardMatchDistance, Distance);
        }
      }

      struct FPlantWindCardVote {
        int32 Count = 0;
        double DistanceSum = 0.0;
      };
      auto ChooseBestCard = [](const TMap<int32, FPlantWindCardVote> &InVotes) {
        int32 BestCard = INDEX_NONE;
        int32 BestCount = 0;
        double BestDistanceSum = TNumericLimits<double>::Max();
        for (const TPair<int32, FPlantWindCardVote> &Vote : InVotes) {
          if (BestCard == INDEX_NONE || Vote.Value.Count > BestCount ||
              (Vote.Value.Count == BestCount &&
               (Vote.Value.DistanceSum < BestDistanceSum ||
                (Vote.Value.DistanceSum == BestDistanceSum &&
                 Vote.Key < BestCard)))) {
            BestCard = Vote.Key;
            BestCount = Vote.Value.Count;
            BestDistanceSum = Vote.Value.DistanceSum;
          }
        }
        return BestCard;
      };
      // 兜底：碎片级「最近卡片」多数表决（顶点级计票，保留原有行为）。
      TArray<TMap<int32, FPlantWindCardVote>> IslandVotes;
      IslandVotes.SetNum(Islands.Num());
      for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
        const int32 CardIndex = CardIndexByVertex[VertexIndex];
        if (CardIndex == INDEX_NONE) {
          continue;
        }
        const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
        FPlantWindCardVote &Vote = IslandVotes[IslandIndex].FindOrAdd(CardIndex);
        Vote.Count += 1;
        Vote.DistanceSum += CardDistanceByVertex[VertexIndex];
      }
      IslandCardIndex.Init(INDEX_NONE, Islands.Num());
      for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex) {
        IslandCardIndex[IslandIndex] = ChooseBestCard(IslandVotes[IslandIndex]);
      }

      // 近距卡片集合：裁切顶点理论上落在原始卡片表面（距离≈0）。贴合/叠放的多张卡片会同时
      // 进入同一顶点的近距集合——这是“同一表面被不同卡片切开”撕裂（相邻顶点两套参数）的判别依据。
      const float NearCardDistance =
          FMath::Clamp(CardReference->Surface.GetDiagonal() * 0.0025f, 0.1f, 0.5f);
      TotalNearCardDistance = FMath::Max(TotalNearCardDistance, NearCardDistance);
      TArray<int32> NearCardFlat;
      TArray<float> NearCardDistanceFlat;
      TArray<int32> NearCardOffset;
      NearCardOffset.SetNumUninitialized(VertexCount + 1);
      NearCardFlat.Reserve(VertexCount * 2);
      NearCardDistanceFlat.Reserve(VertexCount * 2);
      {
        TMap<int32, float> NearScratch;
        for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
          NearCardOffset[VertexIndex] = NearCardFlat.Num();
          CardReference->Surface.CollectNearCards(
              Positions[FVertexID(VertexIndex)], NearCardDistance, NearScratch);
          int32 Added = 0;
          for (const TPair<int32, float> &Pair : NearScratch) {
            if (!PackedCards.IsValidIndex(Pair.Key)) {
              continue;
            }
            NearCardFlat.Add(Pair.Key);
            NearCardDistanceFlat.Add(FMath::Sqrt(Pair.Value));
            Added += 1;
          }
          if (Added == 0 && CardIndexByVertex[VertexIndex] != INDEX_NONE) {
            NearCardFlat.Add(CardIndexByVertex[VertexIndex]);
            NearCardDistanceFlat.Add(CardDistanceByVertex[VertexIndex]);
          }
        }
        NearCardOffset[VertexCount] = NearCardFlat.Num();
      }

      // 碎片候选卡片：≥90% 顶点近距的卡片（放松到 50%），仍为空时退回最近卡片表决结果。
      TArray<int32> IslandVertexCounts;
      IslandVertexCounts.Init(0, Islands.Num());
      for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
        IslandVertexCounts[IslandParameters.IslandOfVertex[VertexIndex]] += 1;
      }
      TArray<TSet<int32>> IslandCandidateCards;
      IslandCandidateCards.SetNum(Islands.Num());
      {
        TMap<uint64, int32> NearCountByIslandCard;
        NearCountByIslandCard.Reserve(NearCardFlat.Num());
        for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
          const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
          for (int32 NearIndex = NearCardOffset[VertexIndex];
               NearIndex < NearCardOffset[VertexIndex + 1]; ++NearIndex) {
            const uint64 Key =
                (static_cast<uint64>(static_cast<uint32>(IslandIndex)) << 32) |
                static_cast<uint32>(NearCardFlat[NearIndex]);
            NearCountByIslandCard.FindOrAdd(Key) += 1;
          }
        }
        for (const TPair<uint64, int32> &Pair : NearCountByIslandCard) {
          const int32 IslandIndex = static_cast<int32>(Pair.Key >> 32);
          const int32 CardIndex = static_cast<int32>(Pair.Key & 0xFFFFFFFFull);
          if (!IslandCandidateCards.IsValidIndex(IslandIndex) ||
              !PackedCards.IsValidIndex(CardIndex) ||
              IslandVertexCounts[IslandIndex] <= 0) {
            continue;
          }
          if (Pair.Value * 10 >= IslandVertexCounts[IslandIndex] * 9) {
            IslandCandidateCards[IslandIndex].Add(CardIndex);
          }
        }
        for (const TPair<uint64, int32> &Pair : NearCountByIslandCard) {
          const int32 IslandIndex = static_cast<int32>(Pair.Key >> 32);
          const int32 CardIndex = static_cast<int32>(Pair.Key & 0xFFFFFFFFull);
          if (!IslandCandidateCards.IsValidIndex(IslandIndex) ||
              IslandCandidateCards[IslandIndex].Num() > 0 ||
              !PackedCards.IsValidIndex(CardIndex) ||
              IslandVertexCounts[IslandIndex] <= 0) {
            continue;
          }
          if (Pair.Value * 2 >= IslandVertexCounts[IslandIndex]) {
            IslandCandidateCards[IslandIndex].Add(CardIndex);
          }
        }
        for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex) {
          if (IslandCandidateCards[IslandIndex].Num() == 0 &&
              IslandCardIndex.IsValidIndex(IslandIndex) &&
              PackedCards.IsValidIndex(IslandCardIndex[IslandIndex])) {
            IslandCandidateCards[IslandIndex].Add(IslandCardIndex[IslandIndex]);
          }
        }
      }

      // 邻接统一：空间相接（顶点距离 < NeighborDistance）且共享候选卡片的碎片并入同一组。
      // 贴合的双层叶片/相邻碎片被统一到同一张卡，消除相邻顶点两套锚点/方向/权重造成的撕裂。
      // 组规模上限（2026-10-09 二轮修正）：邻接统一是为了把「同一张叶片被切成多片」的局部碎片
      // 归到同一张卡；但并查集是传递的，密集裁切网格里几十个碎片连环相接后会把整株并成一个组，
      // 组内投票又让整组落到同一张卡——Desert_Cotton_05 实测一个组吃到 7120 个碎片（占全网格约 70%），
      // 整株绕同一锚点弯曲，视觉上就是「蠕虫」。这里对组规模设硬上限：超过上限的合并直接放弃，
      // 统一只保留局部（同叶片尺度）范围。
      constexpr int32 MaxConsistencyGroupIslands = 32;
      TArray<int32> GroupParents;
      GroupParents.SetNumUninitialized(Islands.Num());
      for (int32 IslandIndex = 0; IslandIndex < GroupParents.Num(); ++IslandIndex) {
        GroupParents[IslandIndex] = IslandIndex;
      }
      TArray<int32> GroupMemberCounts;
      GroupMemberCounts.Init(1, Islands.Num());
      auto FindGroupRoot = [&GroupParents](int32 Index) {
        while (GroupParents[Index] != Index) {
          GroupParents[Index] = GroupParents[GroupParents[Index]];
          Index = GroupParents[Index];
        }
        return Index;
      };
      const float NeighborDistance = FMath::Clamp(NearCardDistance * 2.7f, 0.3f, 0.6f);
      {
        TMap<FIntVector, TArray<int32>> VertexGrid;
        VertexGrid.Reserve(VertexCount);
        const float GridCell = FMath::Max(NeighborDistance, 1e-3f);
        const float NeighborDistanceSquared = NeighborDistance * NeighborDistance;
        for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
          const FVector3f Position = Positions[FVertexID(VertexIndex)];
          const FIntVector Key(
              FMath::FloorToInt(Position.X / GridCell),
              FMath::FloorToInt(Position.Y / GridCell),
              FMath::FloorToInt(Position.Z / GridCell));
          const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
          const TSet<int32> &CandidateCards = IslandCandidateCards[IslandIndex];
          for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
            for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
              for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
                const TArray<int32> *OtherVertices =
                    VertexGrid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
                if (!OtherVertices) {
                  continue;
                }
                for (const int32 OtherVertex : *OtherVertices) {
                  const int32 OtherIsland =
                      IslandParameters.IslandOfVertex[OtherVertex];
                  if (OtherIsland == IslandIndex) {
                    continue;
                  }
                  if (FVector3f::DistSquared(
                          Position, Positions[FVertexID(OtherVertex)]) >
                      NeighborDistanceSquared) {
                    continue;
                  }
                  const TSet<int32> &OtherCandidateCards =
                      IslandCandidateCards[OtherIsland];
                  const bool bCandidateCardsOrdered =
                      CandidateCards.Num() <= OtherCandidateCards.Num();
                  const TSet<int32> &SmallSet =
                      bCandidateCardsOrdered ? CandidateCards : OtherCandidateCards;
                  const TSet<int32> &LargeSet =
                      bCandidateCardsOrdered ? OtherCandidateCards : CandidateCards;
                  bool bSharedCard = false;
                  for (const int32 CardIndex : SmallSet) {
                    if (LargeSet.Contains(CardIndex)) {
                      bSharedCard = true;
                      break;
                    }
                  }
                  if (bSharedCard) {
                    const int32 RootA = FindGroupRoot(IslandIndex);
                    const int32 RootB = FindGroupRoot(OtherIsland);
                    if (RootA != RootB) {
                      const int32 MergedSize =
                          GroupMemberCounts[RootA] + GroupMemberCounts[RootB];
                      if (MergedSize <= MaxConsistencyGroupIslands) {
                        GroupParents[RootB] = RootA;
                        GroupMemberCounts[RootA] = MergedSize;
                      } else {
                        TotalConsistencyCappedUnions += 1;
                      }
                    }
                  }
                }
              }
            }
          }
          VertexGrid.FindOrAdd(Key).Add(VertexIndex);
        }
      }

      // 组内统一卡片：组内顶点对近距卡片计票（越贴合票数越高），平票取距离和最小者。
      IslandResolvedCardIndex = IslandCardIndex;
      {
        TMap<int32, TMap<int32, FPlantWindCardVote>> GroupVotes;
        for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
          const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
          TMap<int32, FPlantWindCardVote> &Votes =
              GroupVotes.FindOrAdd(FindGroupRoot(IslandIndex));
          for (int32 NearIndex = NearCardOffset[VertexIndex];
               NearIndex < NearCardOffset[VertexIndex + 1]; ++NearIndex) {
            FPlantWindCardVote &Vote = Votes.FindOrAdd(NearCardFlat[NearIndex]);
            Vote.Count += 1;
            Vote.DistanceSum += NearCardDistanceFlat[NearIndex];
          }
        }
        for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex) {
          const TMap<int32, FPlantWindCardVote> *Votes =
              GroupVotes.Find(FindGroupRoot(IslandIndex));
          if (!Votes) {
            continue;
          }
          const int32 BestCard = ChooseBestCard(*Votes);
          if (PackedCards.IsValidIndex(BestCard)) {
            IslandResolvedCardIndex[IslandIndex] = BestCard;
          }
        }
      }
      // 一致性统计（日志）：合并组数、被统一到邻片卡片的碎片数。
      {
        TSet<int32> Roots;
        Roots.Reserve(Islands.Num());
        TMap<int32, int32> GroupSizes;
        for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex) {
          const int32 Root = FindGroupRoot(IslandIndex);
          Roots.Add(Root);
          GroupSizes.FindOrAdd(Root) += 1;
          if (IslandResolvedCardIndex[IslandIndex] != IslandCardIndex[IslandIndex]) {
            TotalConsistencyMergedIslands += 1;
          }
        }
        TotalConsistencyGroups += Roots.Num();
        for (const TPair<int32, int32> &Pair : GroupSizes) {
          TotalConsistencyMaxGroupIslands =
              FMath::Max(TotalConsistencyMaxGroupIslands, Pair.Value);
        }
      }
      for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
        const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
        if (IslandResolvedCardIndex.IsValidIndex(IslandIndex) &&
            IslandResolvedCardIndex[IslandIndex] != INDEX_NONE) {
          TotalCardMappedVertices += 1;
        } else {
          TotalCardFallbackVertices += 1;
        }
      }
      for (int32 IslandIndex = 0; IslandIndex < Islands.Num(); ++IslandIndex) {
        if (IslandResolvedCardIndex.IsValidIndex(IslandIndex) &&
            IslandResolvedCardIndex[IslandIndex] != INDEX_NONE) {
          TotalCardMappedIslands += 1;
          UsedCards.Add(IslandResolvedCardIndex[IslandIndex]);
        } else {
          TotalCardFallbackIslands += 1;
          MeshCardFallbackIslands += 1;
        }
      }
    }
    // ---- F5 结构接缝焊接（仅 Fern 预设）----
    // 裁切碎片、双层面片、叶片-茎秆等「空间贴合但未焊接」的位置，两侧风属性必须一致，否则
    // WPO 位移在接缝处跳变 -> 顶点撕裂。F5 口径（与离线原型 fern-rule5.cjs 的 weldSeams 一致）：
    //   1) 两遍扫描 + 就地更新：跨风单元、贴合半径内的顶点做加权平均；普通接触仅在两侧原始权重
    //      都 <= 0.7 时参与（叶尖保护），结构接缝不受该门控。
    //   2) 结构顶点对（合并边 / 被容量判据拒绝的边 / attach / axial）在低权重区（<= 0.6）直接对插。
    //   3) UV1.x 顶端位置仍按贴合半径平滑（保持既有行为），避免位置编码在接缝处跳变。
    // 仅对 Fern（ConnectedCluster）启用；Grass（FrondLike）保持逐片相位原行为不变。
    // F5 需要 F1–F6 结构（簇 + 结构接缝集合）；不透明确切（_OPAQUE）等超大网格在
    // ComputePlantWindFronds 里被刻意跳过（>2048 连通片），此时回退到旧版贴合带平均，
    // 保证裁切网格的接缝平滑不退化。
    const bool bFernSeamBlend =
        MergeMode == EPlantWindFrondMergeMode::ConnectedCluster &&
        FernStructure.ContactDistance > 0.0f;
    const bool bLegacyContactBlend =
        MergeMode == EPlantWindFrondMergeMode::ConnectedCluster && !bFernSeamBlend;
    const bool bContactBlendEnabled = bFernSeamBlend || bLegacyContactBlend;
    const float ContactBlendRadius =
        bContactBlendEnabled
            ? FMath::Clamp(
                  (bUseCardReference ? CardReference->Surface.GetDiagonal()
                                     : FVector3f::Distance(IslandParameters.MeshMin,
                                                           IslandParameters.MeshMax)) *
                      0.003f,
                  0.05f, 0.5f)
            : 0.0f;
    // 卡片参考模式下的「刚性卡片场」判定（2026-10-09）：
    //   卡片映射（UV0 反查 + 一致性分组）已把每个碎片固定到唯一卡片的锚点/方向/沿轴权重场，
    //   同一张卡内部天然连续；此时再跑跨卡片平均（UV1.x 顶端位置平滑 / F5 普通接触焊接）只会把
    //   邻片参数灌进刚性叶片刻，制造同一片叶内部的跳变：表现为茎秆-子叶分离、茎秆中段扭曲
    //   （Desert_Cotton_02/03/06/08、Lady_Fern 各族均可复现）。
    //   因此卡片模式下：跳过 UV1.x 位置平滑，F5 只焊来源结构接缝对；任何碎片未命中卡片（回退）
    //   时退回旧逻辑，保证裁切网格接缝不退化。
    const bool bCardRigidField = bUseCardReference && MeshCardFallbackIslands == 0;
    TArray<FVector3f> ContactDirection;
    TArray<FVector3f> ContactTip;
    TArray<float> ContactWeight;
    TArray<float> ContactRipple;
    TArray<uint8> ContactBlended;
    TArray<uint8> SeamWelded;
    int32 ContactBlendedVertices = 0;
    int32 SeamWeldedVertices = 0;
    // 卡片参考模式（不透明裁切网格）下 F5 以卡片为风单元，结构对集合同步切换到来源结构。
    bool bCardUnitMode = false;
    {
      TArray<FVector3f> VertexPositions;
      TArray<int32> VertexWindUnit;
      VertexPositions.SetNumUninitialized(VertexCount);
      VertexWindUnit.SetNumUninitialized(VertexCount);
      // 全部初始化：无 Source 的顶点会走 INDEX_NONE 分支，不参与焊接，但平滑扫描仍会读到。
      ContactDirection.Init(FVector3f::ZeroVector, VertexCount);
      ContactTip.Init(FVector3f::ZeroVector, VertexCount);
      ContactWeight.Init(0.0f, VertexCount);
      ContactRipple.Init(0.0f, VertexCount);
      ContactBlended.Init(0, VertexCount);
      SeamWelded.Init(0, VertexCount);
      for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
        VertexPositions[VertexIndex] = Positions[FVertexID(VertexIndex)];
        const int32 IslandIndex = IslandParameters.IslandOfVertex[VertexIndex];
        const int32 CardIndex =
            IslandResolvedCardIndex.IsValidIndex(IslandIndex)
                ? IslandResolvedCardIndex[IslandIndex]
                : INDEX_NONE;
        const FPlantWindIsland *Source =
            (CardIndex != INDEX_NONE && PackedCards.IsValidIndex(CardIndex))
                ? &PackedCards[CardIndex]
                : (Islands.IsValidIndex(IslandIndex) ? &Islands[IslandIndex]
                                                     : nullptr);
        if (!Source) {
          VertexWindUnit[VertexIndex] = INDEX_NONE;
          continue;
        }
        // F5 的风单元必须与「风属性来源」同口径：
        //   - 卡片参考模式（不透明裁切网格）：属性取自来源结构簇=卡片 -> 风单元也用卡片索引；
        //     裁切网格自身的 F1–F6 结构只用于几何统计，若拿它当风单元，结构接缝对不上属性跳变位置，
        //     茎秆-子叶等铰接处在高权重区漏焊 -> branch1 拉扯。
        //   - 无参考：风单元 = Fern 结构簇（逐片回退时 = 连通片），与 StructuralClusterPairs 同口径。
        const int32 ClusterIndex =
            FernStructure.ClusterOfIsland.IsValidIndex(IslandIndex)
                ? FernStructure.ClusterOfIsland[IslandIndex]
                : INDEX_NONE;
        const bool bUseCardUnit =
            bUseCardReference && CardIndex != INDEX_NONE &&
            PackedCards.IsValidIndex(CardIndex);
        if (bUseCardUnit) {
          bCardUnitMode = true;
        }
        VertexWindUnit[VertexIndex] =
            bUseCardUnit
                ? CardIndex
                : (ClusterIndex != INDEX_NONE
                       ? ClusterIndex
                       : (CardIndex != INDEX_NONE ? CardIndex
                                                  : Islands.Num() + IslandIndex));
        ContactDirection[VertexIndex] =
            AssetHivePlantWind::DecodeDirection(Source->PackedDirection);
        ContactTip[VertexIndex] = Source->WindPosition;
        const float Projection = FVector3f::DotProduct(
            VertexPositions[VertexIndex] - Source->BasePosition, Source->Direction);
        const float WeightSpan =
            FMath::Max(Source->MaxProjection, MinPlantWindWeightSpan);
        const float FrondWeight =
            FMath::Clamp(Projection / WeightSpan, 0.0f, 1.0f);
        ContactWeight[VertexIndex] = FrondWeight;
        ContactRipple[VertexIndex] = Source->RippleWeight * FrondWeight;
      }
      // (1) UV1.x 顶端位置平滑（保持既有行为）
      if (ContactBlendRadius > 0.0f && !bCardRigidField) {
        TMap<FIntVector, TArray<int32>> ContactGrid;
        ContactGrid.Reserve(VertexCount);
        const float Cell = FMath::Max(ContactBlendRadius, 1e-3f);
        const float RadiusSquared = ContactBlendRadius * ContactBlendRadius;
        for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex) {
          const FVector3f Position = VertexPositions[VertexIndex];
          const FIntVector Key(FMath::FloorToInt(Position.X / Cell),
                               FMath::FloorToInt(Position.Y / Cell),
                               FMath::FloorToInt(Position.Z / Cell));
          float SumWeight = 1.0f;
          FVector3f SumTip = ContactTip[VertexIndex];
          FVector3f SumDirection = ContactDirection[VertexIndex];
          float SumRipple = ContactRipple[VertexIndex];
          float SumUnitWeight = ContactWeight[VertexIndex];
          int32 NeighborCount = 0;
          for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX) {
            for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY) {
              for (int32 OffsetZ = -1; OffsetZ <= 1; ++OffsetZ) {
                const TArray<int32> *Bucket =
                    ContactGrid.Find(Key + FIntVector(OffsetX, OffsetY, OffsetZ));
                if (!Bucket) {
                  continue;
                }
                for (const int32 OtherVertex : *Bucket) {
                  if (VertexWindUnit[OtherVertex] == VertexWindUnit[VertexIndex]) {
                    continue;
                  }
                  const float DistanceSquared = FVector3f::DistSquared(
                      Position, VertexPositions[OtherVertex]);
                  if (DistanceSquared > RadiusSquared) {
                    continue;
                  }
                  const float NeighborWeight =
                      1.0f - FMath::Sqrt(DistanceSquared) / ContactBlendRadius;
                  SumWeight += NeighborWeight;
                  SumTip += ContactTip[OtherVertex] * NeighborWeight;
                  if (bLegacyContactBlend) {
                    // 旧路径（无 F5 结构）：方向 / ripple / 权重一并平均。
                    FVector3f OtherDirection = ContactDirection[OtherVertex];
                    if (FVector3f::DotProduct(OtherDirection,
                                              ContactDirection[VertexIndex]) < 0.0f) {
                      OtherDirection = -OtherDirection;
                    }
                    SumDirection += OtherDirection * NeighborWeight;
                    SumRipple += ContactRipple[OtherVertex] * NeighborWeight;
                    SumUnitWeight += ContactWeight[OtherVertex] * NeighborWeight;
                  }
                  NeighborCount += 1;
                }
              }
            }
          }
          if (NeighborCount > 0) {
            ContactTip[VertexIndex] = SumTip / SumWeight;
            if (bLegacyContactBlend) {
              const FVector3f AveragedDirection = SumDirection.GetSafeNormal();
              if (!AveragedDirection.IsNearlyZero()) {
                ContactDirection[VertexIndex] = AveragedDirection;
              }
              ContactRipple[VertexIndex] = SumRipple / SumWeight;
              ContactWeight[VertexIndex] = SumUnitWeight / SumWeight;
            }
            ContactBlended[VertexIndex] = 1;
            ContactBlendedVertices += 1;
          }
          ContactGrid.FindOrAdd(Key).Add(VertexIndex);
        }
      }
      // (2) 接缝处理：
      //   - 卡片刚性场（碎片全命中卡片）：不做任何接缝平均。卡片映射已经保证每个碎片取到唯一
      //     一张卡片的连续场（方向逐卡常量、权重为沿轴投影比例），跨卡相邻碎片的方向本身也一致
      //     （实测跨接缝位移差 0.002~0.024）。此处若再做窄带跨卡平均，会在等化带边缘造出
      //     0.35~0.4 的权重跳变与 20°~40° 的方向跳变，把位移差放大到 0.44~0.93 —— 这正是
      //     "蕨类茎秆与叶片分离、Cotton 整株蠕虫状"的来源，详见 EqualizePlantWindCardSeams 注释。
      //   - 非刚性：跑既有 F5（结构接缝无条件，普通接触仅低权重区）。
      if (bCardRigidField) {
        SeamWeldedVertices = 0;
        UE_LOG(LogTemp, Warning,
               TEXT("AssetHive import: %s 合成风卡片模式：碎片全命中卡片，跳过跨卡接缝平均（方向/权重/ripple 全部取卡片原值）"),
               *StaticMesh->GetName());
      } else if (bFernSeamBlend) {
        const TSet<uint64> &StructuralPairs =
            (bCardUnitMode && CardReference != nullptr)
                ? CardReference->StructuralCardPairs
                : FernStructure.StructuralClusterPairs;
        SeamWeldedVertices = AssetHivePlantWind::BlendPlantWindSeams(
            VertexCount, VertexPositions, VertexWindUnit, /*ClusterOfUnit=*/nullptr,
            StructuralPairs, FernStructure.StructuralSeams,
            FernStructure.ContactDistance, ContactDirection, ContactWeight,
            ContactRipple, SeamWelded,
            /*NonStructuralTipWeightLimit=*/0.7f, /*bBlendExplicitSeams=*/true);
      }
    }
    if (UVs.GetNumChannels() < RequiredChannels) {
      UVs.SetNumChannels(RequiredChannels);
    }
    for (const FVertexInstanceID VertexInstanceID :
         Mesh->VertexInstances().GetElementIDs()) {
      const FVertexID VertexID = Mesh->GetVertexInstanceVertex(VertexInstanceID);
      const int32 VertexIndex = VertexID.GetValue();
      const int32 VertexIslandIndex =
          IslandParameters.IslandOfVertex.IsValidIndex(VertexIndex)
              ? IslandParameters.IslandOfVertex[VertexIndex]
              : INDEX_NONE;
      const int32 VertexCardIndex =
          IslandResolvedCardIndex.IsValidIndex(VertexIslandIndex)
              ? IslandResolvedCardIndex[VertexIslandIndex]
              : INDEX_NONE;
      const FPlantWindIsland *Source = nullptr;
      if (VertexCardIndex != INDEX_NONE &&
          PackedCards.IsValidIndex(VertexCardIndex)) {
        Source = &PackedCards[VertexCardIndex];
      } else if (Islands.IsValidIndex(VertexIslandIndex)) {
        Source = &Islands[VertexIslandIndex];
      }
      if (!Source) {
        continue;
      }
      const float Projection = FVector3f::DotProduct(
          Positions[VertexID] - Source->BasePosition, Source->Direction);
      const float WeightSpan =
          FMath::Max(Source->MaxProjection, MinPlantWindWeightSpan);
      const float FrondWeight =
          FMath::Clamp(Projection / WeightSpan, 0.0f, 1.0f);
      // 权重口径分三档，按物种形态约束选择（均为离线实测标定）：
      //
      // Grass（FrondLike）：整株高度场 + 幂曲线整形。
      //   高度场保证同一高度同相位；但线性高度权重会让整根叶被"均匀剪切"，
      //   中段被甩出去、看起来像从中段折弯。叶片本身近基部刚、叶尖柔，
      //   因此对权重取二次幂，把摆动集中到叶尖。des6xw0_01 实测卡内权重跨度
      //   0.484 -> 0.248，跨卡错动 0.424cm -> 0.047cm。
      //
      // Fern（ConnectedCluster）：整株高度场与卡片沿轴梯度各半。
      //   蕨类羽叶是从叶柄伸出后下垂的拱形，其"拱顶"往往是全株最高处，
      //   而着生点与下垂叶尖都处在低位。纯高度场会让拱顶权重最高、两端都低，
      //   风大时中段被甩出去、两端被按住，形成半月形拉伸
      //   （bvxkwlf_13 实测中段突出度 +0.244）。掺入一半卡片沿轴梯度后
      //   降到 -0.019（单调），半月消失；同时保留整株高度成分，
      //   避免退回"每张卡一条独立梯度"导致的相邻叶片不同步。
      //
      // Bush（None）：卡片尺度权重，其叶片不构成上述拱形。
      const bool bUseHeightWeight =
          MergeMode == EPlantWindFrondMergeMode::FrondLike ||
          MergeMode == EPlantWindFrondMergeMode::ConnectedCluster;
      float Weight =
          bUseHeightWeight
              ? FMath::Clamp(
                    (Positions[VertexID].Z - WindBoundsMin.Z) / WindBoundsHeight,
                    0.0f, 1.0f)
              : FrondWeight;
      if (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster) {
        Weight = FMath::Clamp(0.5f * Weight + 0.5f * FrondWeight, 0.0f, 1.0f);
      } else if (MergeMode == EPlantWindFrondMergeMode::FrondLike) {
        Weight = FMath::Clamp(Weight * Weight, 0.0f, 1.0f);
      }
      float PackedPosition = Source->PackedPosition;
      float PackedDirection = Source->PackedDirection;
      float BlendedRippleWeight =
          bWriteRippleWeight
              ? FMath::Clamp(Source->RippleWeight * FrondWeight, 0.0f, 1.0f)
              : 0.0f;
      const bool bContactBlended =
          ContactBlended.IsValidIndex(VertexIndex) &&
          ContactBlended[VertexIndex] != 0;
      const bool bSeamWelded =
          SeamWelded.IsValidIndex(VertexIndex) && SeamWelded[VertexIndex] != 0;
      const bool bAttributeBlended =
          bSeamWelded || (bLegacyContactBlend && bContactBlended);
      if (bContactBlended) {
        // UV1.x：接缝两侧顶端位置平滑（保持既有行为）。
        const FVector3f NormalizedTip(
            FMath::Clamp((ContactTip[VertexIndex].X - IslandParameters.MeshMin.X) /
                             SafeMeshSize.X,
                         0.0f, 1.0f),
            FMath::Clamp((ContactTip[VertexIndex].Y - IslandParameters.MeshMin.Y) /
                             SafeMeshSize.Y,
                         0.0f, 1.0f),
            FMath::Clamp((ContactTip[VertexIndex].Z - IslandParameters.MeshMin.Z) /
                             SafeMeshSize.Z,
                         0.0f, 1.0f));
        PackedPosition = AssetHivePlantWind::PackPosition(NormalizedTip);
      }
      if (bAttributeBlended) {
        // F5（或旧版贴合带平均）：方向 / 沿轴权重 / ripple 与相邻风单元对齐
        PackedDirection = AssetHivePlantWind::PackDirectionNear(
            ContactDirection[VertexIndex], Source->PackedDirection);
        if (!bUseHeightWeight) {
          // 高度场口径下权重由世界 Z 唯一决定，不能被碎片级沿轴权重覆盖，否则同一株上
          // 仍会退化成"每张卡一条独立梯度"（这正是 Fern 走高度场要消除的现象）。
          Weight = FMath::Clamp(ContactWeight[VertexIndex], 0.0f, 1.0f);
        }
        if (bWriteRippleWeight) {
          BlendedRippleWeight =
              FMath::Clamp(ContactRipple[VertexIndex], 0.0f, 1.0f);
        }
      }
      UVs.Set(VertexInstanceID, 1, FVector2f(PackedPosition, PackedDirection));
      // UV2 = (Branch1Weight, RippleWeight)：与 SpeedTree 的 UV2 布局一致。
      // RippleWeight（UV2.g）必须带“沿叶轴比例”：母材质的 ripple 是**整片位移**项
      // （MF_SpeedTreeWind_cUSTOM：Position + rippleDir * UV2.g），整片等权会把叶基一起推走，
      // 整丛散成浮空碎片（2026-10-08 反馈）。改成 基部 0 / 顶端=每片叶幅度，位移即变为弯曲。
      UVs.Set(VertexInstanceID, 2, FVector2f(Weight, BlendedRippleWeight));
      if (bWriteBranch2) {
        const int32 WeightCode = FMath::Clamp(
            FMath::RoundToInt(Weight * AssetHivePlantWind::UV3WeightCodeMax), 0,
            AssetHivePlantWind::UV3WeightCodeMax);
        const int32 PositionCode =
            (bContactBlended || bSeamWelded)
                ? AssetHivePlantWind::ToUV3Code(
                      PackedPosition, AssetHivePlantWind::PositionMin,
                      AssetHivePlantWind::PositionRange,
                      AssetHivePlantWind::UV3CodeMax)
                : Source->PositionCode;
        const int32 DirectionCode =
            (bContactBlended || bSeamWelded)
                ? AssetHivePlantWind::ToUV3Code(
                      PackedDirection, AssetHivePlantWind::DirectionMin,
                      AssetHivePlantWind::DirectionRange,
                      AssetHivePlantWind::UV3CodeMax)
                : Source->DirectionCode;
        UVs.Set(VertexInstanceID, 3,
                AssetHivePlantWind::EncodeUV3(PositionCode, DirectionCode,
                                              WeightCode));
      }
    }

    StaticMesh->GetSourceModel(LodIndex).BuildSettings.bUseFullPrecisionUVs = true;
    StaticMesh->CommitMeshDescription(LodIndex);
    ProcessedLods += 1;
    TotalIslands += Islands.Num();
    TotalIslandsBeforeWeld += IslandParameters.IslandsBeforeWeld;
    TotalWeldMergedVertices += IslandParameters.WeldMergedVertices;
    TotalVertices += VertexCount;
    TotalContactBlendedVertices += ContactBlendedVertices;
    TotalSeamWeldedVertices += SeamWeldedVertices;
    if (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster &&
        FernStructure.ClusterCount > 0) {
      TotalFernClusters += FernStructure.ClusterCount;
      TotalFernRachis += FernStructure.RachisCount;
      TotalFernCandidatePairs += FernStructure.CandidatePairCount;
      TotalFernMergedPairs += FernStructure.MergedPairCount;
      TotalFernRejectedSpan += FernStructure.RejectedBySpan;
      TotalFernRejectedRachis += FernStructure.RejectedByRachisCapacity;
      TotalFernStructuralSeams += FernStructure.StructuralSeams.Num();
      if (FernStructure.bFallbackPerIsland) {
        TotalFernFallbackLods += 1;
      }
    }
    if (bUseFronds) {
      TotalConsolidatedFronds += FrondParameters.ConsolidatedFrondCount;
      TotalFrondMergedIslands +=
          FMath::Max(0, IslandParameters.Num() - FrondParameters.Num());
      TotalJoinDistanceMax =
          FMath::Max(TotalJoinDistanceMax, FrondParameters.JoinDistance);
    }
  }

  if (ProcessedLods == 0) {
    return false;
  }
  // Grass 的 branch1 权重已改为 SpeedTree 整簇高度归一化，文案与 Fern/Bush 的卡片尺度区分开。
  const FString CardSummary =
      bUseCardReference
          ? (MergeMode == EPlantWindFrondMergeMode::FrondLike
                 ? FString::Printf(
                       TEXT("；权重按 SpeedTree 整簇高度推导，UV0 迁移 %d 个顶点"),
                       TotalUVMigratedVertices)
                 : FString::Printf(
                       TEXT("；权重按原始卡片推导，UV0 迁移 %d 个顶点"),
                       TotalUVMigratedVertices))
          : FString();
  // UE5 的 FString::Printf 使用 TCheckedFormatString，格式串必须是编译期字面量，
  // 不能是三元表达式——Fern / Grass 两种文案分开调用。
  FString FrondSummary;
  if (TotalConsolidatedFronds > 0) {
    if (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster) {
      FrondSummary = FString::Printf(
          TEXT("；Fern 结构簇 %d 个（连通片 %d 个，rachis %d 条；候选边 %d，合并 %d，拒(跨度) %d，拒(rachis>2) %d，逐片回退 LOD %d 个；同簇共享锚点/方向/沿轴权重，簇间独立相位）"),
          TotalFernClusters, TotalIslands, TotalFernRachis, TotalFernCandidatePairs,
          TotalFernMergedPairs, TotalFernRejectedSpan, TotalFernRejectedRachis,
          TotalFernFallbackLods);
    } else {
      FrondSummary = FString::Printf(
          TEXT("；Grass frond 叶片簇 %d 个（%d 个连通片并入，锚点/方向共享，权重取整簇高度，叶片相位独立）"),
          TotalConsolidatedFronds, TotalFrondMergedIslands);
    }
  }
  const TCHAR *const WindStyleLabel =
      bWriteBranch2 ? TEXT("branch1 + branch2->UV3")
                    : (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster
                           ? TEXT("fern / branch1")
                           : TEXT("branch1"));
  OutSummary = FString::Printf(
      TEXT("%s: 合成 SpeedTree 风 (%s) LOD %d 个 / 连通片 %d 个（位置焊接前 %d，合并重复顶点 %d 个）/ 顶点 %d 个%s%s"),
      *StaticMesh->GetName(), WindStyleLabel, ProcessedLods,
      TotalIslands, TotalIslandsBeforeWeld, TotalWeldMergedVertices, TotalVertices,
      *CardSummary, *FrondSummary);
  UE_LOG(LogTemp, Display, TEXT("AssetHive import: %s"), *OutSummary);
  if (TotalConsolidatedFronds > 0) {
    if (MergeMode == EPlantWindFrondMergeMode::ConnectedCluster) {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive import: %s 合成风 Fern 分组：%d 个叶片风单元（%d 个连通片归入叶片；core 近重合阈值 %.2fcm，同片叶共享锚点/方向/沿轴权重，叶片间独立运动）"),
             *StaticMesh->GetName(), TotalConsolidatedFronds, TotalFrondMergedIslands,
             TotalJoinDistanceMax);
    } else {
      UE_LOG(LogTemp, Warning,
             TEXT("AssetHive import: %s 合成风 frond 分组：%d 个叶片簇（%d 个连通片并入；叶轴与子叶共享锚点/方向/沿轴权重）"),
             *StaticMesh->GetName(), TotalConsolidatedFronds, TotalFrondMergedIslands);
    }
  }
  if (bUseCardReference) {
    // Display 级别在项目日志中被过滤，这里用 Warning 保证排查可见（每个网格一行）。
    UE_LOG(LogTemp, Warning,
           TEXT("AssetHive import: %s 合成风卡片映射（UV0 迁移 %d 个顶点）：连通片命中 %d/%d（回退 %d 个），顶点命中 %d/%d（原始命中 %d 个，使用卡片 %d 张，最近点最大距离 %.3f cm）"),
           *StaticMesh->GetName(), TotalUVMigratedVertices,
           TotalCardMappedIslands,
           TotalCardMappedIslands + TotalCardFallbackIslands,
           TotalCardFallbackIslands, TotalCardMappedVertices,
           TotalCardMappedVertices + TotalCardFallbackVertices, RawCardHitVertices,
           UsedCards.Num(), MaxCardMatchDistance);
    UE_LOG(LogTemp, Warning,
           TEXT("AssetHive import: %s 合成风一致性：近距匹配阈值 %.3f cm，合并为 %d 组（最大 %d 个碎片），跨片统一卡片 %d 个碎片；组规模上限 32，放弃合并 %d 次"),
        *StaticMesh->GetName(), TotalNearCardDistance, TotalConsistencyGroups,
        TotalConsistencyMaxGroupIslands, TotalConsistencyMergedIslands,
        TotalConsistencyCappedUnions);
  }
  // 逐角拆分的源网格（例如不透明裁切导出的 FBX）会在焊接前后出现数量级差异，明确告警，
  // 便于后续排查“每个三角形一片”造成的合成风顶点撕裂。
  if (TotalContactBlendedVertices > 0 || TotalSeamWeldedVertices > 0) {
    UE_LOG(LogTemp, Warning,
           TEXT("AssetHive import: %s 合成风(Fern)接缝处理：F5 焊接 %d 个顶点（结构顶点对 %d 个），UV1.x 顶端位置平滑 %d 个顶点"),
           *StaticMesh->GetName(), TotalSeamWeldedVertices, TotalFernStructuralSeams,
           TotalContactBlendedVertices);
  }
  if (TotalIslandsBeforeWeld > TotalIslands * 4) {
    UE_LOG(LogTemp, Warning,
           TEXT("AssetHive import: %s 源网格顶点逐角拆分（重复顶点 %d 个），已按位置焊接后写入合成风数据：连通片 %d -> %d"),
           *StaticMesh->GetName(), TotalWeldMergedVertices, TotalIslandsBeforeWeld,
           TotalIslands);
  }
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
      // 临时凸包碰撞参数（2026-10-09 定稿）：Hull Count 64、Max Hull Verts 32、
      // Hull Precision 1000000（UE 上限）。精度过低会抹掉模型轮廓细节。
      constexpr int32 TemporaryHullCount = 64;
      constexpr int32 TemporaryHullMaxVerts = 32;
      constexpr int32 TemporaryHullPrecision = 1000000;
      Subsystem->SetConvexDecompositionCollisions(
          StaticMesh, TemporaryHullCount, TemporaryHullMaxVerts,
          TemporaryHullPrecision);
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

// A masked/LOD fallback variant still needs the original Masked/Atlas
// material set. When every imported model variant is a cut OPAQUE variant,
// that set is redundant and must not be created.
static bool HasMaskedPlantModelVariant(
    const TMap<UStaticMesh *, FString> &VariantKeyByMesh,
    const TSet<FString> &OpaqueModelVariantKeys) {
  for (const TPair<UStaticMesh *, FString> &VariantPair : VariantKeyByMesh) {
    const bool bOpaqueVariant =
        IsOpaqueModelVariantKey(VariantPair.Value) ||
        OpaqueModelVariantKeys.Contains(
            NormalizeModelVariantKey(VariantPair.Value));
    if (!bOpaqueVariant) {
      return true;
    }
  }
  return false;
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
  // 命名跟随静态网格体：SM_ 前缀换成 FT_（SM_Env_Grass_x_01_OPAQUE -> FT_Env_Grass_x_01_OPAQUE）。
  FString FoliageStem = StaticMesh->GetName();
  if (FoliageStem.StartsWith(TEXT("SM_"), ESearchCase::CaseSensitive)) {
    FoliageStem.RightChopInline(3);
  } else if (FoliageStem.IsEmpty()) {
    FoliageStem = AssetName;
  }
  const FString FoliageAssetName =
      FString::Printf(TEXT("FT_%s"), *FoliageStem);
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
  // Display 在项目日志里会被过滤，用 Warning 保证导入后能看到 FT 的落点（每个网格一行）。
  UE_LOG(LogTemp, Warning, TEXT("AssetHive import: FoliageType %s <- %s"),
         *FoliageType->GetPathName(), *StaticMesh->GetName());
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
                            bool bOpaque = false,
                            EPlantSyntheticWind SyntheticWind =
                                EPlantSyntheticWind::None) {
  const bool bBillboard = MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase);
  // 合成 SpeedTree 风（导入时选择 Grass/Bush/Fern）：改用带 ST 风的母材质；OPAQUE 裁切
  // 变体同样使用 ST 风母材质（Blend=Opaque 覆盖保持不变），仅 billboard 不参与。
  const EPlantSyntheticWind WindStyle =
      bBillboard ? EPlantSyntheticWind::None : SyntheticWind;
  const bool bSyntheticWind = WindStyle != EPlantSyntheticWind::None;
  const bool bSyntheticWindBranch2 =
      WindStyle == EPlantSyntheticWind::Branch1Branch2UV3;
  FString ParentPath;
  UMaterialInterface *ParentMaterial = nullptr;
  if (bSyntheticWind) {
    if (WindStyle == EPlantSyntheticWind::Fern) {
      // Fern 预设：每片叶一个风单元 + 最多 branch1（UV1/UV2）；父材质可由
      // Project Settings → AssetHive → Plant Material 的 Fern 项覆盖。
      ParentPath = UAssetHiveSettings::GetPlantFernSTParentMaterialPath(bUseVT);
      ParentMaterial = UAssetHiveSettings::GetPlantFernSTParentMaterial(bUseVT);
    } else {
      ParentPath = bSyntheticWindBranch2
                       ? UAssetHiveSettings::GetPlantBushSTParentMaterialPath(bUseVT)
                       : UAssetHiveSettings::GetPlantGrassSTParentMaterialPath(bUseVT);
      ParentMaterial =
          bSyntheticWindBranch2
              ? UAssetHiveSettings::GetPlantBushSTParentMaterial(bUseVT)
              : UAssetHiveSettings::GetPlantGrassSTParentMaterial(bUseVT);
    }
  } else if (bOpaque) {
    ParentPath = UAssetHiveSettings::GetPlantOpaqueParentMaterialPath(bUseVT);
    ParentMaterial = UAssetHiveSettings::GetPlantOpaqueParentMaterial(bUseVT);
  } else {
    ParentPath = UAssetHiveSettings::GetPlantParentMaterialPath(bBillboard);
    ParentMaterial =
        UAssetHiveSettings::GetPlantParentMaterial(bBillboard, bUseVT);
  }
  if (!ParentMaterial) {
    GAssetHiveImportFailed = true;
    UE_LOG(LogTemp, Error, TEXT("AssetHive: missing Plant %s parent material: %s"),
           bSyntheticWind
               ? TEXT("SpeedTree Wind")
               : (bOpaque ? TEXT("Opaque")
                          : (bBillboard ? TEXT("Billboard") : TEXT("Atlas"))),
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

  if (bSyntheticWind) {
    // 合成 SpeedTree 风参数（Grass/Bush/Fern）：branch1 恒定从 UV1/UV2 读；
    // branch2 由 UV3 重映射读取（Use Remapped UV3）。母材质里 xMax/yMax/wMax 承载的是
    // Range（范围）而不是最大值，与 ApplySyntheticSpeedTreeWind 的编码范围一一对应。
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(AssetHivePlantWind::SwitchBranch1Enable)), true);
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(AssetHivePlantWind::SwitchBranch2Enable)),
        bSyntheticWindBranch2);
    // Grass / Bush / Fern 预设统一关闭 ripple：Desert_Cotton 等 Fern 资产开启 ripple 会在
    // 结构接缝上引入额外位移，产生撕裂；UV2.g 仍烘焙，便于日后仅改开关恢复。
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(AssetHivePlantWind::SwitchRippleEnable)),
        false);
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(AssetHivePlantWind::SwitchHasBranch2Data)),
        bSyntheticWindBranch2);
    MaterialInstance->SetStaticSwitchParameterValueEditorOnly(
        FMaterialParameterInfo(FName(AssetHivePlantWind::SwitchUseRemappedUV3)),
        bSyntheticWindBranch2);
    // 基础风参数（用户 2026-10-09 指定 Grass 预设默认值）+ UV 解码范围。
    const TPair<const TCHAR *, float> WindScalars[] = {
        {AssetHivePlantWind::ScalarPositionMin, AssetHivePlantWind::PositionMin},
        {AssetHivePlantWind::ScalarPositionRange, AssetHivePlantWind::PositionRange},
        {AssetHivePlantWind::ScalarDirectionMin, AssetHivePlantWind::DirectionMin},
        {AssetHivePlantWind::ScalarDirectionRange, AssetHivePlantWind::DirectionRange},
        {AssetHivePlantWind::ScalarWeightMin, AssetHivePlantWind::WeightMin},
        {AssetHivePlantWind::ScalarWeightRange, AssetHivePlantWind::WeightRange},
        {AssetHivePlantWind::ScalarBranch1Bend, AssetHivePlantWind::DefaultBranch1Bend},
        {AssetHivePlantWind::ScalarBranch1Oscillation, AssetHivePlantWind::DefaultBranch1Oscillation},
        {AssetHivePlantWind::ScalarBranch1StretchLimit, AssetHivePlantWind::DefaultBranch1StretchLimit},
        {AssetHivePlantWind::ScalarSharedBend, AssetHivePlantWind::DefaultSharedBend},
        {AssetHivePlantWind::ScalarSharedOscillation, AssetHivePlantWind::DefaultSharedOscillation}};
    for (const TPair<const TCHAR *, float> &WindScalar : WindScalars) {
      MaterialInstance->SetScalarParameterValueEditorOnly(
          FMaterialParameterInfo(FName(WindScalar.Key)), WindScalar.Value);
    }
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
  // 2026-10-08：植被（3dplant）的 FoliageType 改为导入时自动创建（每个静态网格体
  // 一个 FT_ 资产，放在资产文件夹根）。软件侧不再下发 createFoliage 字段，
  // 只有在 job 里显式写入 false 时才关闭，方便排查/回退。
  bool bCreateFoliageDefault = true;
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
      // 合成 SpeedTree 风由导入时选择（grass / bush / fern / 不选），随 job 传入：未选择时
      // 不写风动 UV、也不切换 ST 风母材质；选择后写入的即导出到引擎的那个网格
      // （勾选不透明剪切并导出裁切版本时，裁切资产同样处理）。
      FString SyntheticWindMode;
      AssetObject->TryGetStringField(TEXT("plantSyntheticWind"), SyntheticWindMode);
      SyntheticWindMode.TrimStartAndEndInline();
      SyntheticWindMode = SyntheticWindMode.ToLower();
      if (SyntheticWindMode == TEXT("grass")) {
        PlantProfile.SyntheticWind = EPlantSyntheticWind::Branch1;
      } else if (SyntheticWindMode == TEXT("bush")) {
        PlantProfile.SyntheticWind = EPlantSyntheticWind::Branch1Branch2UV3;
      } else if (SyntheticWindMode == TEXT("fern")) {
        PlantProfile.SyntheticWind = EPlantSyntheticWind::Fern;
      } else {
        PlantProfile.SyntheticWind = EPlantSyntheticWind::None;
      }
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
    // 植被（3dplant）：材质实例 / 纹理 / 静态网格体分目录存放，FoliageType 留在资产文件夹根。
    const bool bUsesFoliageFolders = AssetType == TEXT("3dplant");
    const FString TextureFolder =
        (bUsesMaterialFolders || bUsesFoliageFolders) ? AssetFolder / TEXT("Tex")
                                                       : AssetFolder;
    const FString MaterialFolder =
        (bUsesMaterialFolders || bUsesFoliageFolders) ? AssetFolder / TEXT("MI")
                                                      : AssetFolder;
    const FString MeshFolder =
        bUsesFoliageFolders ? AssetFolder / TEXT("Meshes") : AssetFolder;
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
              AssetToolsModule, BaseFile, MeshFolder, BaseMeshName,
              PlantProfile.bNanite);
          if (!BaseMesh) {
            continue;
          }

          // LOD 版本导出的 Masked 植被：把该变体自己的 LOD 链合并成 custom LOD，
          // 合并后才配置材质，整个 LOD 栈只重建一次。OPAQUE 版本不带任何 LOD。
          if (const TArray<FPlantModelLodEntry> *VariantLods =
                  PlantLodsByVariant.Find(VariantKey)) {
            ImportPlantCustomLods(AssetToolsModule, BaseMesh, *VariantLods,
                                  MeshFolder, BaseMeshName);
          }

          // Nanite and materials are configured together after texture import.
          ImportedMeshes.Add(BaseMesh);
          VariantKeyByMesh.Add(BaseMesh, VariantKey);
          SourceFileByMesh.Add(BaseMesh, BaseFile);

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
          Task->DestinationPath = MeshFolder;
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
              // 植被（3dplant，含 st9）逐个网格创建对应 FoliageType（FT_ + 网格名去掉 SM_）。
              if (bCreateFoliageForAsset && AssetType == TEXT("3dplant")) {
                CreateFoliageTypeAsset(AssetFolder, ModelAssetName, StaticMesh);
              }
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
    const bool bHasMaskedModelVariant =
        AssetType == TEXT("3dplant") &&
        HasMaskedPlantModelVariant(VariantKeyByMesh, OpaqueModelVariantKeys);
    const bool bAllModelVariantsOpaque =
        AssetType == TEXT("3dplant") && VariantKeyByMesh.Num() > 0 &&
        !bHasMaskedModelVariant;
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
              TextureFolder, FString::Printf(TEXT("T_%s_D"), *TextureStem),
              ImportPlantSource(TEXT("albedo")), EPlantTextureKind::Diffuse,
              PlantProfile, bUseVT);

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 24, 0, 99)),
              FString::Printf(TEXT("导出 Normal 贴图: %s"), *AssetName), false);
          UTexture2D *NormalTexture = CreatePlantTextureAsset(
              TextureFolder, FString::Printf(TEXT("T_%s_N"), *TextureStem),
              ImportPlantSource(TEXT("normal")), EPlantTextureKind::Normal,
              PlantProfile, bUseVT);

          UTexture2D *ORMTexture = nullptr;
          if (SourceTextureBySlot.Contains(TEXT("orm"))) {
            ORMTexture = CreatePlantTextureAsset(
                TextureFolder, FString::Printf(TEXT("T_%s_ORM"), *TextureStem),
                ImportPlantSource(TEXT("orm")), EPlantTextureKind::ORM,
                PlantProfile, bUseVT);
          } else {
            UTexture2D *AOSource = ImportPlantSource(TEXT("ao"));
            UTexture2D *RoughnessSource = ImportPlantSource(TEXT("roughness"));
            UTexture2D *MetallicSource = ImportPlantSource(TEXT("metalness"));
            if (AOSource || RoughnessSource || MetallicSource) {
              ORMTexture = CreatePackedORMTexture(
                  TextureFolder, FString::Printf(TEXT("T_%s_ORM"), *TextureStem),
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
                TextureFolder, FString::Printf(TEXT("T_%s_O"), *TextureStem),
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
                TextureFolder, FString::Printf(TEXT("T_%s_SSC"), *TextureStem),
                FImageUtils::ImportFileAsTexture2D(SubsurfaceSourcePath),
                EPlantTextureKind::SubsurfaceColor, PlantProfile, bUseVT);
          }

          SetStageProgress(
              static_cast<float>(FMath::Clamp(AssetBaseProgress + 26, 0, 99)),
              FString::Printf(TEXT("创建植被材质实例: %s"), *AssetName), false);
          // 合成 SpeedTree 风（导入时选择 Grass/Bush）：atlas 与 OPAQUE 裁切角色改用
          // 带 ST 风的母材质（Grass→MI_Env_Grass_ST，Bush→MI_Env_Bush_ST；OPAQUE
          // 保持 Blend=Opaque 覆盖），billboard 保持原样。
          const EPlantSyntheticWind PlantWindStyle =
              MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase)
                  ? EPlantSyntheticWind::None
                  : PlantProfile.SyntheticWind;
          if (bHasMaskedModelVariant) {
            MaterialInstance = CreatePlantMaterialInstance(
                MaterialFolder, PlantMaterialStem, DiffuseTexture, NormalTexture,
                ORMTexture, OpacityMaskTexture, SubsurfaceTexture, MaterialRole,
                bUseVT, /*bOpaque=*/false, PlantWindStyle);
          }
          // md §5.2：OPAQUE 裁切变体不创建 billboard 材质实例。
          if (bHasOpaqueModelVariant &&
              !MaterialRole.Equals(TEXT("billboard"), ESearchCase::IgnoreCase)) {
            // 勾选不透明剪切 + 合成风时，裁切变体的材质同样使用 ST 风母材质
            // （CreatePlantMaterialInstance 内部保持 Blend=Opaque 覆盖）。
            OpaqueMaterialInstance = CreatePlantMaterialInstance(
                MaterialFolder, PlantMaterialStem, DiffuseTexture, NormalTexture,
                ORMTexture, nullptr, SubsurfaceTexture, MaterialRole, bUseVT,
                /*bOpaque=*/true, PlantWindStyle);
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
      // 合成 SpeedTree 风（导入时选择 grass/bush）：写 branch1（UV1/UV2）；Bush 额外
      // 把 branch2 重映射到 UV3。不透明裁切（_OPAQUE）网格的碎片远小于原始卡片，权重必须用
      // 同目录未裁切 FBX 的卡片参数推导——否则碎片内权重瞬间走完 0..1，整株被拉成条状。
      if (AssetType == TEXT("3dplant") &&
          PlantProfile.SyntheticWind != EPlantSyntheticWind::None) {
        const bool bWindBranch2 =
            PlantProfile.SyntheticWind == EPlantSyntheticWind::Branch1Branch2UV3;
        // Fern（蕨类）：每片叶一个风单元（近重合 core 合并 + 小叶归属）；Grass：frond 判定合并；Bush：不合并。
        const EPlantWindFrondMergeMode WindMergeMode =
            PlantProfile.SyntheticWind == EPlantSyntheticWind::Fern
                ? EPlantWindFrondMergeMode::ConnectedCluster
                : (bWindBranch2 ? EPlantWindFrondMergeMode::None
                                : EPlantWindFrondMergeMode::FrondLike);
        const FString WindMeshVariantKey = VariantKeyByMesh.FindRef(StaticMesh);
        const bool bOpaqueWindMesh =
            IsOpaqueModelVariantKey(WindMeshVariantKey) ||
            OpaqueModelVariantKeys.Contains(
                NormalizeModelVariantKey(WindMeshVariantKey));
        // 裁切网格的卡片参考一律按「单叶」粒度建立（Grass 在非裁切网格上仍按 frond 合并）：
        // UV1.y 相位与 UV2.g ripple 都按叶片锚点散列，卡片粒度 = 单叶时 _OPAQUE 与 masked
        // 两个变体的风动才一致；权重仍由 ApplySyntheticSpeedTreeWind 的 MergeMode 决定。
        const EPlantWindFrondMergeMode WindCardMergeMode =
            WindMergeMode == EPlantWindFrondMergeMode::FrondLike
                ? EPlantWindFrondMergeMode::None
                : WindMergeMode;
        FPlantWindCardReference WindCardReference;
        UStaticMesh *TempWindReferenceMesh = nullptr;
        if (bOpaqueWindMesh) {
          const FString WindSourceFile = SourceFileByMesh.FindRef(StaticMesh);
          const FString ReferenceFile =
              ResolveOpaquePlantWindReferenceFile(WindSourceFile);
          if (!ReferenceFile.IsEmpty()) {
            TempWindReferenceMesh = ImportStaticMeshAsset(
                AssetToolsModule, ReferenceFile, AssetFolder,
                FString::Printf(TEXT("TEMP_WINDCARDS_%s"), *StaticMesh->GetName()),
                /*bBuildNanite=*/false);
            if (!TempWindReferenceMesh) {
              UE_LOG(LogTemp, Warning,
                     TEXT("AssetHive import: %s 原始 FBX %s 导入失败，回退碎片级合成风"),
                     *StaticMesh->GetName(), *ReferenceFile);
            } else if (!BuildPlantWindCardReference(
                           TempWindReferenceMesh, WindCardMergeMode,
                           WindCardReference)) {
              UE_LOG(LogTemp, Warning,
                     TEXT("AssetHive import: %s 无法从原始 FBX %s 推导卡片参数，回退碎片级合成风"),
                     *StaticMesh->GetName(), *ReferenceFile);
            } else if (!IsPlantWindCardReferenceScaleCompatible(*StaticMesh,
                                                               WindCardReference)) {
              UE_LOG(LogTemp, Warning,
                     TEXT("AssetHive import: %s 原始 FBX %s 与裁切网格尺度不一致（包围盒对角线 %.2f / %.2f cm），回退碎片级合成风"),
                     *StaticMesh->GetName(), *ReferenceFile,
                     WindCardReference.Surface.GetDiagonal(),
                     static_cast<float>(
                         StaticMesh->GetBoundingBox().GetSize().Size()));
              WindCardReference.Reset();
            }
          } else {
            UE_LOG(LogTemp, Warning,
                   TEXT("AssetHive import: %s 未找到同目录未裁切 FBX（%s），回退碎片级合成风"),
                   *StaticMesh->GetName(),
                   *FPaths::GetCleanFilename(WindSourceFile));
          }
        }
        if (WindCardReference.IsValid()) {
          if (FMeshDescription *CutMeshDescription =
                  StaticMesh->GetMeshDescription(0)) {
            const float AppliedReferenceYaw =
                NormalizePlantWindCardReferenceYaw(*CutMeshDescription,
                                                   WindCardReference);
            if (!FMath::IsNearlyZero(AppliedReferenceYaw)) {
              UE_LOG(LogTemp, Warning,
                     TEXT("AssetHive import: %s ????? FBX ????????? %.0f?????????????? UV0 ??????????"),
                     *StaticMesh->GetName(), AppliedReferenceYaw);
            }
          }
        }
        FString WindSummary;
        if (!ApplySyntheticSpeedTreeWind(
                StaticMesh, bWindBranch2, WindMergeMode,
                WindCardReference.IsValid() ? &WindCardReference : nullptr,
                WindSummary)) {
          UE_LOG(LogTemp, Warning,
                 TEXT("AssetHive import: %s 合成 SpeedTree 风数据写入失败（无可用 LOD）"),
                 *StaticMesh->GetName());
        }
        if (TempWindReferenceMesh) {
          ObjectTools::DeleteSingleObject(TempWindReferenceMesh,
                                          /*bPerformReferenceCheck=*/false);
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
          OpaqueMaterialInstances.Num() > 0 &&
          (bOpaqueMesh || MaterialInstances.Num() == 0);
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
  TArray<FString> MegascansPlantTags;
  MegascansPlantTags.Add(TEXT("Megascans"));
  const FString MegascansPlantStem =
      BuildPlantObjectStem(MegascansPlantTags, TEXT("abc123"));
  TestEqual(TEXT("Megascans plant stem uses Foliage segment"),
            MegascansPlantStem, FString(TEXT("Env_Foliage_abc123")));
  TestEqual(TEXT("Megascans plant mesh uses Foliage segment"),
            FString::Printf(TEXT("SM_%s_%s"), *MegascansPlantStem, TEXT("01")),
            FString(TEXT("SM_Env_Foliage_abc123_01")));

  TMap<UStaticMesh *, FString> OpaqueOnlyPlantVariants;
  OpaqueOnlyPlantVariants.Add(nullptr, TEXT("01"));
  TSet<FString> OpaquePlantVariantKeys;
  OpaquePlantVariantKeys.Add(TEXT("01"));
  TestFalse(TEXT("Opaque-only plant skips masked material set"),
            HasMaskedPlantModelVariant(OpaqueOnlyPlantVariants,
                                       OpaquePlantVariantKeys));

  TMap<UStaticMesh *, FString> OpaqueSuffixPlantVariants;
  OpaqueSuffixPlantVariants.Add(nullptr, TEXT("01_OPAQUE"));
  TSet<FString> NoOpaquePlantVariantKeys;
  TestFalse(TEXT("Opaque suffix plant skips masked material set"),
            HasMaskedPlantModelVariant(OpaqueSuffixPlantVariants,
                                       NoOpaquePlantVariantKeys));

  TMap<UStaticMesh *, FString> MaskedFallbackPlantVariants;
  MaskedFallbackPlantVariants.Add(nullptr, TEXT("01"));
  TestTrue(TEXT("Masked fallback plant keeps masked material set"),
           HasMaskedPlantModelVariant(MaskedFallbackPlantVariants,
                                      NoOpaquePlantVariantKeys));

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

  // 合成 SpeedTree 风（Megascans 3D Plants，2026-10-07）：仅 Megascans 生效，
  // Grass=branch1、Bush=branch1+branch2→UV3、Fern=接触连通簇合并（最多 branch1），
  // 其余资产类型保持不写风数据。
  TestTrue(TEXT("Megascans grass uses branch1 synthetic wind"),
           ResolvePlantAssetProfile({FString(TEXT("Megascans")), FString(TEXT("Grass"))})
                   .SyntheticWind == EPlantSyntheticWind::Branch1);
  TestTrue(TEXT("Megascans bush uses branch1 + branch2 UV3 wind"),
           ResolvePlantAssetProfile({FString(TEXT("Megascans")), FString(TEXT("Bush"))})
                   .SyntheticWind == EPlantSyntheticWind::Branch1Branch2UV3);
  TestTrue(TEXT("Non-Megascans grass keeps no synthetic wind"),
           GrassProfile.SyntheticWind == EPlantSyntheticWind::None);
  TestTrue(TEXT("Non-Megascans bush keeps no synthetic wind"),
           BushProfile.SyntheticWind == EPlantSyntheticWind::None);
  TestTrue(TEXT("Megascans tree keeps no synthetic wind"),
           ResolvePlantAssetProfile({FString(TEXT("Megascans")), FString(TEXT("Tree"))})
                   .SyntheticWind == EPlantSyntheticWind::None);

  {
    // UV3 编码必须能被材质侧 HLSL 解码函数还原（10bit 位置 / 10bit 方向 / 8bit 权重）。
    const float PackedPosition =
        AssetHivePlantWind::PackPosition(FVector3f(0.4f, 0.7f, 1.0f));
    const float PackedDirection =
        AssetHivePlantWind::PackDirection(FVector3f(0.0f, 0.0f, 1.0f), true);
    const FVector2f Encoded = AssetHivePlantWind::EncodeUV3(
        AssetHivePlantWind::ToUV3Code(PackedPosition,
                                      AssetHivePlantWind::PositionMin,
                                      AssetHivePlantWind::PositionRange,
                                      AssetHivePlantWind::UV3CodeMax),
        AssetHivePlantWind::ToUV3Code(PackedDirection,
                                      AssetHivePlantWind::DirectionMin,
                                      AssetHivePlantWind::DirectionRange,
                                      AssetHivePlantWind::UV3CodeMax),
        AssetHivePlantWind::UV3WeightCodeMax);
    const int32 Low = FMath::RoundToInt(Encoded.X * 16383.0f);
    const int32 High = FMath::RoundToInt(Encoded.Y * 16383.0f);
    const float DecodedPosition =
        AssetHivePlantWind::PositionMin +
        (static_cast<float>(High / 16) / 1023.0f) * AssetHivePlantWind::PositionRange;
    const float DecodedDirection =
        AssetHivePlantWind::DirectionMin +
        (static_cast<float>((High % 16) * 64 + Low / 256) / 1023.0f) *
            AssetHivePlantWind::DirectionRange;
    const float DecodedWeight =
        AssetHivePlantWind::WeightMin +
        (static_cast<float>(Low % 256) / 255.0f) * AssetHivePlantWind::WeightRange;
    TestTrue(TEXT("UV3 round-trips packed position"),
             FMath::Abs(DecodedPosition - PackedPosition) < 0.002f);
    TestTrue(TEXT("UV3 round-trips packed direction"),
             FMath::Abs(DecodedDirection - PackedDirection) < 0.02f);
    TestTrue(TEXT("UV3 round-trips weight"), FMath::Abs(DecodedWeight - 1.0f) < 0.001f);
    TestTrue(TEXT("Packed direction decodes back to up vector"),
             FVector3f::DotProduct(AssetHivePlantWind::DecodeDirection(DecodedDirection),
                                   FVector3f(0.0f, 0.0f, 1.0f)) > 0.96f);
  }

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

// 合成风位置焊接（2026-10-07）：不透明裁切产出的 FBX 会把同一位置的顶点逐角拆开，
// 只按共享顶点判定连通片会把每个三角形当成独立片，各片锚点/方向/权重不同 -> WPO 顶点撕裂。
// 这里用两个共享一条边（位置重合、顶点独立）的三角形验证焊接后归为同一连通片。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindWeldTest,
                                 "AssetHive.Plants.SyntheticWindWelding",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindWeldTest::RunTest(const FString &Parameters) {
  const TArray<FVector3f> Positions = {
      FVector3f(0.0f, 0.0f, 0.0f), FVector3f(10.0f, 0.0f, 0.0f),
      FVector3f(0.0f, 10.0f, 0.0f),  // 第一个三角形 0-1-2
      FVector3f(10.0f, 0.0f, 0.0f), FVector3f(0.0f, 10.0f, 0.0f),
      FVector3f(10.0f, 10.0f, 0.0f)};  // 第二个三角形 3-4-5（3/4 与 1/2 位置重合）
  TArray<int32> Parents;
  Parents.SetNumUninitialized(Positions.Num());
  for (int32 Index = 0; Index < Parents.Num(); ++Index) {
    Parents[Index] = Index;
  }
  auto FindRoot = [&Parents](int32 Index) {
    while (Parents[Index] != Index) {
      Parents[Index] = Parents[Parents[Index]];
      Index = Parents[Index];
    }
    return Index;
  };
  auto Union = [&Parents, &FindRoot](int32 A, int32 B) {
    const int32 RootA = FindRoot(A);
    const int32 RootB = FindRoot(B);
    if (RootA != RootB) {
      Parents[RootB] = RootA;
    }
  };
  Union(0, 1);
  Union(0, 2);
  Union(3, 4);
  Union(3, 5);
  TestNotEqual(TEXT("逐角拆分的两个三角形焊接前是两片"), FindRoot(0), FindRoot(3));
  const int32 Merged = AssetHivePlantWind::WeldCoincidentVertices(
      Positions.Num(),
      [&Positions](int32 VertexIndex) { return Positions[VertexIndex]; }, Parents,
      0.01f);
  TestEqual(TEXT("焊接合并的重复顶点数量"), Merged, 2);
  TestEqual(TEXT("共享位置的三角形合并为同一片"), FindRoot(0), FindRoot(3));
  TestEqual(TEXT("重合位置的顶点同片"), FindRoot(1), FindRoot(4));
  TestEqual(TEXT("另一重合位置的顶点同片"), FindRoot(2), FindRoot(5));
  TestTrue(TEXT("焊接不改动顶点位置数量"), Positions.Num() == 6);
  return true;
}

// 合成风卡片参考（2026-10-07）：不透明裁切网格的碎片只有毫米级，若按碎片自身归一化权重，
// 碎片内权重会瞬间走完 0..1（视觉上被拉成长条）。这里用一张 0..10cm 的卡片 + 位于卡片顶端
// 的一小块裁切碎片验证：碎片顶点改用卡片尺度权重后，片内权重跨度应接近 0。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindCardReferenceTest,
                                 "AssetHive.Plants.SyntheticWindCardReference",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindCardReferenceTest::RunTest(const FString &Parameters) {
  UStaticMesh *ReferenceMesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindCardProbe")), RF_Transient);
  FGCObjectScopeGuard ReferenceGuard(ReferenceMesh);
  ReferenceMesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    TVertexInstanceAttributesRef<FVector2f> UVs =
        Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    const FVertexID V0 = Description.CreateVertex();
    const FVertexID V1 = Description.CreateVertex();
    const FVertexID V2 = Description.CreateVertex();
    const FVertexID V3 = Description.CreateVertex();
    Positions[V0] = FVector3f(0.0f, 0.0f, 0.0f);
    Positions[V1] = FVector3f(10.0f, 0.0f, 0.0f);
    Positions[V2] = FVector3f(10.0f, 0.0f, 10.0f);
    Positions[V3] = FVector3f(0.0f, 0.0f, 10.0f);
    const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
    const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
    const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
    const FVertexInstanceID I3 = Description.CreateVertexInstance(V3);
    // UV0 = 卡片在 atlas 里的区域（0..0.1）：裁切碎片靠 UV0 反查所属卡片。
    UVs.Set(I0, 0, FVector2f(0.0f, 0.0f));
    UVs.Set(I1, 0, FVector2f(0.1f, 0.0f));
    UVs.Set(I2, 0, FVector2f(0.1f, 0.1f));
    UVs.Set(I3, 0, FVector2f(0.0f, 0.1f));
    Description.CreateTriangle(Group, {I0, I1, I2});
    Description.CreateTriangle(Group, {I0, I2, I3});
    ReferenceMesh->CreateMeshDescription(0, MoveTemp(Description));
    ReferenceMesh->CommitMeshDescription(0);
  }

  FPlantWindCardReference CardReference;
  TestTrue(TEXT("从原始网格推导出卡片参考"),
           BuildPlantWindCardReference(ReferenceMesh,
                                       EPlantWindFrondMergeMode::None,
                                       CardReference));
  TestEqual(TEXT("卡片数量"), CardReference.Cards.Num(), 1);
  if (CardReference.Cards.Num() == 1) {
    TestTrue(TEXT("卡片方向为 +Z"), CardReference.Cards[0].Direction.Z > 0.99f);
    TestTrue(TEXT("卡片最大投影接近卡片高度"),
             FMath::IsNearlyEqual(CardReference.Cards[0].MaxProjection, 10.0f,
                                  0.6f));
  }
  TestEqual(TEXT("UV0 写入卡片参考"), CardReference.Surface.TriangleUVs.Num(), 6);
  float UVLookupDistance = 0.0f;
  TestEqual(TEXT("UV0 反查命中卡片"),
            CardReference.Surface.FindCardByUV(FVector2f(0.05f, 0.05f),
                                               FVector3f(5.0f, 0.0f, 5.0f),
                                               UVLookupDistance),
            0);
  TestEqual(TEXT("UV0 落在卡片之外时不命中"),
            CardReference.Surface.FindCardByUV(FVector2f(0.5f, 0.5f),
                                               FVector3f(5.0f, 0.0f, 5.0f),
                                               UVLookupDistance),
            INDEX_NONE);
  // 裁切碎片：卡片顶端的一小块（0..0.2cm 宽、9.8..10cm 高）。
  UStaticMesh *CutMesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindCutProbe")), RF_Transient);
  FGCObjectScopeGuard CutGuard(CutMesh);
  CutMesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    TVertexInstanceAttributesRef<FVector2f> UVs =
        Attributes.GetVertexInstanceUVs();
    UVs.SetNumChannels(1);
    const FVertexID V0 = Description.CreateVertex();
    const FVertexID V1 = Description.CreateVertex();
    const FVertexID V2 = Description.CreateVertex();
    Positions[V0] = FVector3f(0.0f, 0.0f, 9.8f);
    Positions[V1] = FVector3f(0.2f, 0.0f, 9.8f);
    Positions[V2] = FVector3f(0.0f, 0.0f, 10.0f);
    const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
    const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
    const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
    // 碎片位于卡片顶端：UV0 与 3D 位置一致（卡片 10cm ↔ UV 0..0.1）。
    UVs.Set(I0, 0, FVector2f(0.000f, 0.098f));
    UVs.Set(I1, 0, FVector2f(0.002f, 0.098f));
    UVs.Set(I2, 0, FVector2f(0.000f, 0.100f));
    Description.CreateTriangle(Group, {I0, I1, I2});
    CutMesh->CreateMeshDescription(0, MoveTemp(Description));
    CutMesh->CommitMeshDescription(0);
  }

  FString Summary;
  TestTrue(TEXT("写入合成风数据"),
           ApplySyntheticSpeedTreeWind(CutMesh, /*bWriteBranch2=*/false,
                                       EPlantWindFrondMergeMode::None,
                                       &CardReference, Summary));
  FMeshDescription *CutDescription = CutMesh->GetMeshDescription(0);
  if (!TestNotNull(TEXT("裁切网格描述存在"), CutDescription)) {
    return false;
  }
  FStaticMeshAttributes CutAttributes(*CutDescription);
  CutAttributes.Register(true);
  TVertexInstanceAttributesRef<FVector2f> CutUVs =
      CutAttributes.GetVertexInstanceUVs();
  float MinWeight = TNumericLimits<float>::Max();
  float MaxWeight = -TNumericLimits<float>::Max();
  for (const FVertexInstanceID InstanceID :
       CutDescription->VertexInstances().GetElementIDs()) {
    const float Weight = CutUVs.Get(InstanceID, 2).X;
    MinWeight = FMath::Min(MinWeight, Weight);
    MaxWeight = FMath::Max(MaxWeight, Weight);
  }
  TestTrue(TEXT("碎片权重按卡片尺度落在顶端（接近 1）"), MinWeight > 0.9f);
  TestTrue(TEXT("片内权重跨度接近 0（不再被拉成长条）"),
           (MaxWeight - MinWeight) < 0.05f);
  TestTrue(TEXT("Summary 记录卡片推导"),
           Summary.Contains(TEXT("权重按原始卡片推导")));
  TestTrue(TEXT("Summary 记录 UV0 迁移命中"),
           Summary.Contains(TEXT("UV0 迁移")));
  return true;
}

// SpeedTree frond 规则单测：叶轴（长条）与 3mm 内的子叶（小片）必须合并为同一张 frond 卡，
// 关闭开关时仍按连通片各建一张卡（对照）。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindFrondClusterTest,
                                 "AssetHive.Plants.SyntheticWindFrondCluster",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindFrondClusterTest::RunTest(const FString &Parameters) {
  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindFrondProbe")), RF_Transient);
  FGCObjectScopeGuard MeshGuard(Mesh);
  Mesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    // 叶轴：0.2cm 宽、10cm 高的长条（中间留一排顶点与子叶相接）。
    const FVertexID Stem0 = Description.CreateVertex();
    const FVertexID Stem1 = Description.CreateVertex();
    const FVertexID Stem2 = Description.CreateVertex();
    const FVertexID Stem3 = Description.CreateVertex();
    const FVertexID Stem4 = Description.CreateVertex();
    const FVertexID Stem5 = Description.CreateVertex();
    Positions[Stem0] = FVector3f(0.0f, 0.0f, 0.0f);
    Positions[Stem1] = FVector3f(0.2f, 0.0f, 0.0f);
    Positions[Stem2] = FVector3f(0.0f, 0.0f, 5.0f);
    Positions[Stem3] = FVector3f(0.2f, 0.0f, 5.0f);
    Positions[Stem4] = FVector3f(0.0f, 0.0f, 10.0f);
    Positions[Stem5] = FVector3f(0.2f, 0.0f, 10.0f);
    const FVertexInstanceID StemI0 = Description.CreateVertexInstance(Stem0);
    const FVertexInstanceID StemI1 = Description.CreateVertexInstance(Stem1);
    const FVertexInstanceID StemI2 = Description.CreateVertexInstance(Stem2);
    const FVertexInstanceID StemI3 = Description.CreateVertexInstance(Stem3);
    const FVertexInstanceID StemI4 = Description.CreateVertexInstance(Stem4);
    const FVertexInstanceID StemI5 = Description.CreateVertexInstance(Stem5);
    Description.CreateTriangle(Group, {StemI0, StemI1, StemI3});
    Description.CreateTriangle(Group, {StemI0, StemI3, StemI2});
    Description.CreateTriangle(Group, {StemI2, StemI3, StemI5});
    Description.CreateTriangle(Group, {StemI2, StemI5, StemI4});
    // 子叶：根部与叶轴相距约 0.07cm（大于焊接阈值 0.001cm、小于 frond 合并阈值 0.1cm）。
    const FVertexID Leaf0 = Description.CreateVertex();
    const FVertexID Leaf1 = Description.CreateVertex();
    const FVertexID Leaf2 = Description.CreateVertex();
    const FVertexID Leaf3 = Description.CreateVertex();
    Positions[Leaf0] = FVector3f(0.25f, 0.0f, 4.95f);
    Positions[Leaf1] = FVector3f(2.2f, 0.0f, 4.95f);
    Positions[Leaf2] = FVector3f(2.2f, 0.0f, 5.05f);
    Positions[Leaf3] = FVector3f(0.25f, 0.0f, 5.05f);
    const FVertexInstanceID LeafI0 = Description.CreateVertexInstance(Leaf0);
    const FVertexInstanceID LeafI1 = Description.CreateVertexInstance(Leaf1);
    const FVertexInstanceID LeafI2 = Description.CreateVertexInstance(Leaf2);
    const FVertexInstanceID LeafI3 = Description.CreateVertexInstance(Leaf3);
    Description.CreateTriangle(Group, {LeafI0, LeafI1, LeafI2});
    Description.CreateTriangle(Group, {LeafI0, LeafI2, LeafI3});
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    Mesh->CommitMeshDescription(0);
  }

  FPlantWindCardReference SplitReference;
  TestTrue(TEXT("关闭 frond 分组时可推导卡片"),
           BuildPlantWindCardReference(Mesh, EPlantWindFrondMergeMode::None,
                                       SplitReference));
  TestEqual(TEXT("关闭 frond 分组：叶轴与子叶各自成卡"),
            SplitReference.Cards.Num(), 2);

  FPlantWindCardReference FrondReference;
  TestTrue(TEXT("开启 frond 分组时可推导卡片"),
           BuildPlantWindCardReference(Mesh, EPlantWindFrondMergeMode::FrondLike,
                                       FrondReference));
  TestEqual(TEXT("叶轴 + 子叶合并为同一张 frond 卡"),
            FrondReference.Cards.Num(), 1);
  if (FrondReference.Cards.Num() == 1) {
    TestTrue(TEXT("frond 主轴取叶轴方向 +Z"),
             FrondReference.Cards[0].Direction.Z > 0.99f);
    TestTrue(TEXT("frond 权重场按叶轴长度归一化"),
             FMath::IsNearlyEqual(FrondReference.Cards[0].MaxProjection, 10.0f,
                                  0.6f));
  }
  return true;
}

// Fern 预设单测：同一片叶被拆开的两个 nearly-coincident core（10cm / 8cm，Y 偏移
// 0.05cm）——Grass 叶片簇判定不合并（2 张卡），Fern v7 近重合 core 合并（1 张卡）。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindFernMergeTest,
                                 "AssetHive.Plants.SyntheticWindFernMerge",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindFernMergeTest::RunTest(const FString &Parameters) {
  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindFernProbe")), RF_Transient);
  FGCObjectScopeGuard MeshGuard(Mesh);
  Mesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    auto AddBlade = [&Description, &Positions, &Group](float MinX, float MaxX,
                                                       float Height, float OffsetY) {
      const FVertexID V0 = Description.CreateVertex();
      const FVertexID V1 = Description.CreateVertex();
      const FVertexID V2 = Description.CreateVertex();
      const FVertexID V3 = Description.CreateVertex();
      Positions[V0] = FVector3f(MinX, OffsetY, 0.0f);
      Positions[V1] = FVector3f(MaxX, OffsetY, 0.0f);
      Positions[V2] = FVector3f(MaxX, OffsetY, Height);
      Positions[V3] = FVector3f(MinX, OffsetY, Height);
      const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
      const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
      const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
      const FVertexInstanceID I3 = Description.CreateVertexInstance(V3);
      Description.CreateTriangle(Group, {I0, I1, I2});
      Description.CreateTriangle(Group, {I0, I2, I3});
    };
    // 同一片叶被拆开的两半：X/Z 完全重合（根部 Y 偏移 0.05cm，近重合阈值内），
    // Fern v7 合并为同一风单元；Grass 的“主轴 + 短子叶”判定仍保持独立。
    AddBlade(0.0f, 0.2f, 10.0f, 0.0f);
    AddBlade(0.0f, 0.2f, 8.0f, 0.05f);
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    Mesh->CommitMeshDescription(0);
  }

  FPlantWindCardReference FrondLikeReference;
  TestTrue(TEXT("Grass 判定可推导卡片"),
           BuildPlantWindCardReference(Mesh, EPlantWindFrondMergeMode::FrondLike,
                                       FrondLikeReference));
  TestEqual(TEXT("Grass 判定：长度相近的相接叶片保持独立"),
            FrondLikeReference.Cards.Num(), 2);

  FPlantWindCardReference FernReference;
  TestTrue(TEXT("Fern 模式可推导卡片"),
           BuildPlantWindCardReference(Mesh,
                                       EPlantWindFrondMergeMode::ConnectedCluster,
                                       FernReference));
  TestEqual(TEXT("Fern 强合并：相接叶片并入同一风单元"),
            FernReference.Cards.Num(), 1);
  if (FernReference.Cards.Num() == 1) {
    TestTrue(TEXT("Fern 风单元主轴取最长叶方向 +Z"),
             FernReference.Cards[0].Direction.Z > 0.99f);
  }
  return true;
}

// Fern per-frond 单测（2026-10-07 v7）：4cm satellite 距叶轴 0.8cm 必须归入同一叶片；
// 3.8cm 外、长度 6cm 的独立叶片自身是 core，不得被并入（保持独立卡）。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindFernWideGapTest,
                                 "AssetHive.Plants.SyntheticWindFernWideGap",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindFernWideGapTest::RunTest(const FString &Parameters) {
  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindFernGapProbe")), RF_Transient);
  FGCObjectScopeGuard MeshGuard(Mesh);
  Mesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    auto AddBlade = [&Description, &Positions, &Group](float MinX, float MaxX,
                                                       float Height) {
      const FVertexID V0 = Description.CreateVertex();
      const FVertexID V1 = Description.CreateVertex();
      const FVertexID V2 = Description.CreateVertex();
      const FVertexID V3 = Description.CreateVertex();
      Positions[V0] = FVector3f(MinX, 0.0f, 0.0f);
      Positions[V1] = FVector3f(MaxX, 0.0f, 0.0f);
      Positions[V2] = FVector3f(MaxX, 0.0f, Height);
      Positions[V3] = FVector3f(MinX, 0.0f, Height);
      const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
      const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
      const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
      const FVertexInstanceID I3 = Description.CreateVertexInstance(V3);
      Description.CreateTriangle(Group, {I0, I1, I2});
      Description.CreateTriangle(Group, {I0, I2, I3});
    };
    // 叶轴 10cm；4cm 小叶（低于 0.45*最长片 = 4.5cm，属 satellite）距叶轴 0.8cm，
    // v7 按最近 core + 方位归属并入叶轴；3.8cm 外的 6cm 卡片自身是 core，保持独立。
    AddBlade(0.0f, 0.2f, 10.0f);
    AddBlade(1.0f, 1.2f, 4.0f);
    AddBlade(5.0f, 5.2f, 6.0f);
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    Mesh->CommitMeshDescription(0);
  }

  FPlantWindCardReference FernReference;
  TestTrue(TEXT("Fern 模式可推导卡片"),
           BuildPlantWindCardReference(Mesh,
                                       EPlantWindFrondMergeMode::ConnectedCluster,
                                       FernReference));
  TestEqual(TEXT("8mm 根部间隙并入同一风单元，3.8cm 独立叶片保持独立"),
            FernReference.Cards.Num(), 2);

  FPlantWindCardReference FrondLikeReference;
  TestTrue(TEXT("Grass 判定可推导卡片"),
           BuildPlantWindCardReference(Mesh, EPlantWindFrondMergeMode::FrondLike,
                                       FrondLikeReference));
  TestEqual(TEXT("Grass 小距离判定不合并 8mm 间隙叶片"),
            FrondLikeReference.Cards.Num(), 3);
  return true;
}

// Grass 权重推导单测（2026-10-08）：SpeedTree 原生 branch1 权重取整簇高度归一化。
// 两片独立草叶（10cm / 4cm）：短叶顶端 z=4cm 落在整簇高度 0..10 的 0.4；
// 旧的“逐卡片沿轴投影”口径会让它走满自身卡片到 1.0。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePlantWindGrassHeightWeightTest,
                                 "AssetHive.Plants.SyntheticWindGrassHeightWeight",
                                 EAutomationTestFlags::EditorContext |
                                     EAutomationTestFlags::EngineFilter)
bool FAssetHivePlantWindGrassHeightWeightTest::RunTest(const FString &Parameters) {
  UStaticMesh *Mesh = NewObject<UStaticMesh>(
      GetTransientPackage(), FName(TEXT("AssetHiveWindGrassHeightProbe")), RF_Transient);
  FGCObjectScopeGuard MeshGuard(Mesh);
  Mesh->AddSourceModel();
  {
    FMeshDescription Description;
    FStaticMeshAttributes Attributes(Description);
    Attributes.Register();
    TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
    const FPolygonGroupID Group = Description.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] =
        FName(TEXT("Material_0"));
    auto AddBlade = [&Description, &Positions, &Group](float MinX, float MaxX,
                                                       float Height) {
      const FVertexID V0 = Description.CreateVertex();
      const FVertexID V1 = Description.CreateVertex();
      const FVertexID V2 = Description.CreateVertex();
      const FVertexID V3 = Description.CreateVertex();
      Positions[V0] = FVector3f(MinX, 0.0f, 0.0f);
      Positions[V1] = FVector3f(MaxX, 0.0f, 0.0f);
      Positions[V2] = FVector3f(MaxX, 0.0f, Height);
      Positions[V3] = FVector3f(MinX, 0.0f, Height);
      const FVertexInstanceID I0 = Description.CreateVertexInstance(V0);
      const FVertexInstanceID I1 = Description.CreateVertexInstance(V1);
      const FVertexInstanceID I2 = Description.CreateVertexInstance(V2);
      const FVertexInstanceID I3 = Description.CreateVertexInstance(V3);
      Description.CreateTriangle(Group, {I0, I1, I2});
      Description.CreateTriangle(Group, {I0, I2, I3});
    };
    AddBlade(0.0f, 0.2f, 10.0f);
    AddBlade(5.0f, 5.2f, 4.0f);
    Mesh->CreateMeshDescription(0, MoveTemp(Description));
    Mesh->CommitMeshDescription(0);
  }

  FString Summary;
  TestTrue(TEXT("Grass 合成风写入成功"),
           ApplySyntheticSpeedTreeWind(Mesh, /*bWriteBranch2=*/false,
                                       EPlantWindFrondMergeMode::FrondLike,
                                       /*CardReference=*/nullptr, Summary));
  FMeshDescription *Weighted = Mesh->GetMeshDescription(0);
  if (!TestNotNull(TEXT("写入后的 MeshDescription 存在"), Weighted)) {
    return false;
  }
  FStaticMeshAttributes WeightedAttributes(*Weighted);
  WeightedAttributes.Register(true);
  TVertexAttributesRef<FVector3f> WeightedPositions =
      WeightedAttributes.GetVertexPositions();
  TVertexInstanceAttributesRef<FVector2f> WeightUVs =
      WeightedAttributes.GetVertexInstanceUVs();
  if (!TestTrue(TEXT("UV2 权重通道已写入"), WeightUVs.GetNumChannels() >= 3)) {
    return false;
  }

  TMap<int32, float> WeightByZKey;
  TMap<int32, float> PhaseByXKey;
  TMap<int32, float> RippleByXZKey;
  TMap<int32, float> RippleMaxByX;
  for (const FVertexInstanceID InstanceID :
       Weighted->VertexInstances().GetElementIDs()) {
    const FVertexID VertexID = Weighted->GetVertexInstanceVertex(InstanceID);
    const FVector3f Position = WeightedPositions[VertexID];
    const int32 ZKey = FMath::RoundToInt(Position.Z * 10.0f);
    WeightByZKey.Add(ZKey, WeightUVs.Get(InstanceID, 2).X);
    // 两片叶分别位于 x≈0 与 x≈5，UV1.y 是摆动方向+振荡相位。
    PhaseByXKey.Add(FMath::RoundToInt(Position.X), WeightUVs.Get(InstanceID, 1).Y);
    // UV2.g 是 SpeedTree 的 ripple 权重（母材质按 G 通道缩放 ripple 项）。
    const float RippleValue = WeightUVs.Get(InstanceID, 2).Y;
    RippleByXZKey.Add(FMath::RoundToInt(Position.X) * 1000 +
                          FMath::RoundToInt(Position.Z),
                      RippleValue);
    float &RippleMax =
        RippleMaxByX.FindOrAdd(FMath::RoundToInt(Position.X), 0.0f);
    RippleMax = FMath::Max(RippleMax, RippleValue);
  }
  const float *RootWeight = WeightByZKey.Find(0);
  const float *ShortTopWeight = WeightByZKey.Find(40);
  const float *TallTopWeight = WeightByZKey.Find(100);
  if (!TestNotNull(TEXT("根部顶点权重存在"), RootWeight) ||
      !TestNotNull(TEXT("短叶顶端顶点权重存在"), ShortTopWeight) ||
      !TestNotNull(TEXT("高叶顶端顶点权重存在"), TallTopWeight)) {
    return false;
  }
  TestTrue(TEXT("根部权重为 0"), FMath::IsNearlyEqual(*RootWeight, 0.0f, 0.02f));
  TestTrue(TEXT("短叶顶端按整簇高度取 0.4（旧口径为 1.0）"),
           FMath::IsNearlyEqual(*ShortTopWeight, 0.4f, 0.02f));
  TestTrue(TEXT("簇顶权重为 1"), FMath::IsNearlyEqual(*TallTopWeight, 1.0f, 0.02f));

  // 叶片间运动：SpeedTree 的 UV1.y 同时是摆动方向与振荡相位，同高不同叶必须错开，
  // 否则整簇同步摆动（用户反馈“运动太整体”）。
  const float *Blade1Phase = PhaseByXKey.Find(0);
  const float *Blade2Phase = PhaseByXKey.Find(5);
  if (!TestNotNull(TEXT("第一片叶相位存在"), Blade1Phase) ||
      !TestNotNull(TEXT("第二片叶相位存在"), Blade2Phase)) {
    return false;
  }
  TestTrue(TEXT("两片叶的 UV1.y（摆动方向+相位）必须错开"),
           FMath::Abs(*Blade1Phase - *Blade2Phase) > 0.05f);
  TestTrue(TEXT("抖动后摆动方向仍以生长方向为主（<25°）"),
           AssetHivePlantWind::DecodeDirection(*Blade1Phase).Z > 0.9f);

  // UV2.g（RippleWeight）：SpeedTree 把 ripple 权重写在 UV2.g，母材质用 G 通道缩放 ripple 项。
  // 权重恒 0 时整丛只剩 shared/branch1 运动（叶片“粘住”）；整片等权则叶基被一起推走
  // （整丛散成浮空碎片，2026-10-08 实测）。所以必须是“基部 0 → 顶端=每片叶幅度”。
  const float *Blade1RootRipple = RippleByXZKey.Find(0 * 1000 + 0);
  const float *Blade2RootRipple = RippleByXZKey.Find(5 * 1000 + 0);
  const float *Blade1MaxRipple = RippleMaxByX.Find(0);
  const float *Blade2MaxRipple = RippleMaxByX.Find(5);
  if (!TestNotNull(TEXT("第一片叶根部 ripple 存在"), Blade1RootRipple) ||
      !TestNotNull(TEXT("第二片叶根部 ripple 存在"), Blade2RootRipple) ||
      !TestNotNull(TEXT("第一片叶 ripple 峰值存在"), Blade1MaxRipple) ||
      !TestNotNull(TEXT("第二片叶 ripple 峰值存在"), Blade2MaxRipple)) {
    return false;
  }
  TestTrue(TEXT("叶片基部 UV2.g 必须为 0（否则整片被推离草丛）"),
           *Blade1RootRipple <= 0.02f && *Blade2RootRipple <= 0.02f);
  TestTrue(TEXT("叶尖 UV2.g 落在 SpeedTree 实测幅度区间 0.60~0.90"),
           *Blade1MaxRipple >= 0.55f && *Blade1MaxRipple <= 0.91f &&
               *Blade2MaxRipple >= 0.55f && *Blade2MaxRipple <= 0.91f);
  TestTrue(TEXT("两片叶的 ripple 幅度必须错开"),
           FMath::Abs(*Blade1MaxRipple - *Blade2MaxRipple) > 0.01f);
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
