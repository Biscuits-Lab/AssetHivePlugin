#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Misc/SecureHash.h"

namespace AssetHiveZionPlantReuse {
// 数字段按数值排序，让同批变体 1 稳定提供共享实例的默认风参数。
inline bool NaturalLess(const FString &A, const FString &B) {
  int32 I = 0, J = 0;
  while (I < A.Len() && J < B.Len()) {
    if (FChar::IsDigit(A[I]) && FChar::IsDigit(B[J])) {
      int32 EndA = I, EndB = J;
      while (EndA < A.Len() && FChar::IsDigit(A[EndA])) ++EndA;
      while (EndB < B.Len() && FChar::IsDigit(B[EndB])) ++EndB;
      while (I < EndA && A[I] == TEXT('0')) ++I;
      while (J < EndB && B[J] == TEXT('0')) ++J;
      if (EndA - I != EndB - J) return EndA - I < EndB - J;
      while (I < EndA) {
        if (A[I] != B[J]) return A[I] < B[J];
        ++I; ++J;
      }
      I = EndA; J = EndB;
    } else {
      const TCHAR Left = FChar::ToLower(A[I++]), Right = FChar::ToLower(B[J++]);
      if (Left != Right) return Left < Right;
    }
  }
  return A.Len() - I < B.Len() - J;
}

// 仅排列命名匹配的植被任务，其他类型保留原有批次位置；结果仍通过 assetId 回传。
inline void OrderNamedPlants(TArray<TSharedPtr<FJsonValue>> &Assets) {
  TArray<int32> Positions;
  TArray<TSharedPtr<FJsonValue>> Plants;
  for (int32 Index = 0; Index < Assets.Num(); ++Index) {
    if (!Assets[Index].IsValid() || Assets[Index]->Type != EJson::Object) continue;
    const auto Object = Assets[Index]->AsObject();
    FString Type, Matching;
    Object->TryGetStringField(TEXT("assetType"), Type);
    Object->TryGetStringField(TEXT("materialMatching"), Matching);
    if (Type == TEXT("3dplant") && Matching == TEXT("name")) {
      Positions.Add(Index);
      Plants.Add(Assets[Index]);
    }
  }
  const auto Name = [](const TSharedPtr<FJsonValue> &Value) {
    FString Result;
    if (!Value->AsObject()->TryGetStringField(TEXT("assetFolderName"), Result) || Result.IsEmpty())
      Value->AsObject()->TryGetStringField(TEXT("name"), Result);
    return Result;
  };
  Plants.StableSort([&](const auto &A, const auto &B) { return NaturalLess(Name(A), Name(B)); });
  for (int32 Index = 0; Index < Plants.Num(); ++Index) Assets[Positions[Index]] = Plants[Index];
}

inline FString Hash(const FString &Value) {
  uint8 Bytes[20];
  FSHA1::HashBuffer(*Value, static_cast<uint32>(Value.Len() * sizeof(TCHAR)), Bytes);
  return BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes));
}

// 排序对象键，保留数组顺序和完整数值精度；JSON 字段顺序不能改变复用判断。
inline FString ValueKey(const TSharedPtr<FJsonValue> &Value) {
  if (!Value.IsValid()) return TEXT("null");
  switch (Value->Type) {
  case EJson::Object: {
    const auto Object = Value->AsObject();
    TArray<TPair<FString, TSharedPtr<FJsonValue>>> Fields;
    for (const auto &Entry : Object->Values) Fields.Emplace(FString(*Entry.Key), Entry.Value);
    Fields.Sort([](const auto &A, const auto &B) { return A.Key < B.Key; });
    FString Result = TEXT("object:");
    for (const auto &Field : Fields) {
      const FString &Key = Field.Key;
      const FString Part = ValueKey(Field.Value);
      Result += FString::Printf(TEXT("%d:%s%d:%s"), Key.Len(), *Key, Part.Len(), *Part);
    }
    return Result;
  }
  case EJson::Array: {
    FString Result = TEXT("array:");
    for (const auto &Entry : Value->AsArray()) {
      const FString Part = ValueKey(Entry);
      Result += FString::Printf(TEXT("%d:%s"), Part.Len(), *Part);
    }
    return Result;
  }
  case EJson::Number: return FString::Printf(TEXT("number:%.17g"), Value->AsNumber());
  case EJson::Boolean: return Value->AsBool() ? TEXT("true") : TEXT("false");
  case EJson::String: return TEXT("string:") + Value->AsString();
  default: return TEXT("null");
  }
}

inline FString ObjectKey(const TSharedPtr<FJsonObject> &Object) {
  return Object.IsValid() ? ValueKey(MakeShared<FJsonValueObject>(Object)) : TEXT("null");
}

inline bool WantsRemappedUV3(const TSharedPtr<FJsonObject> &Profile,
                            const TSharedPtr<FJsonObject> &Material, bool &bDeclared) {
  bool bWanted = false;
  bDeclared = Profile.IsValid() && Profile->TryGetBoolField(TEXT("useRemappedUV3"), bWanted);
  const bool bDisabled = bDeclared && !bWanted;
  for (const auto &Source : {Profile, Material}) {
    const TSharedPtr<FJsonObject> *Switches = nullptr;
    bool bValue = false;
    if (Source.IsValid() && Source->TryGetObjectField(TEXT("staticSwitchParameters"), Switches) &&
        Switches && Switches->IsValid() && (*Switches)->TryGetBoolField(TEXT("Use Remapped UV3"), bValue)) {
      bDeclared = true;
      if (!bDisabled) bWanted = bValue;
    }
  }
  return bDeclared && bWanted;
}

inline TSharedPtr<FJsonObject> EffectiveWind(const TSharedPtr<FJsonObject> &Profile,
                                           const TSharedPtr<FJsonObject> &Material) {
  auto Scalars = MakeShared<FJsonObject>();
  auto Switches = MakeShared<FJsonObject>();
  for (const auto &Source : {Profile, Material}) {
    for (const TCHAR *Kind : {TEXT("scalarParameters"), TEXT("staticSwitchParameters")}) {
      const TSharedPtr<FJsonObject> *Values = nullptr;
      TSet<FName> Seen;
      if (Source.IsValid() && Source->TryGetObjectField(Kind, Values) && Values && Values->IsValid()) {
        for (const auto &Entry : (*Values)->Values) {
          const FName Name(*Entry.Key);
          // FName 不区分大小写；重复别名的覆盖顺序不明确时，不提供跨变体复用依据。
          if (Seen.Contains(Name)) return nullptr;
          Seen.Add(Name);
          const FString Key = FString(*Entry.Key).ToLower();
          double Number = 0;
          bool Boolean = false;
          if (FString(Kind) == TEXT("scalarParameters") && Entry.Value.IsValid() && Entry.Value->Type == EJson::Number &&
              Entry.Value->TryGetNumber(Number) && FMath::IsFinite(Number) && FMath::Abs(Number) <= MAX_flt) {
            // 与 MI 写入保持一致，比较最终 float 参数而非 JSON 的额外 double 精度。
            Scalars->SetNumberField(Key, static_cast<float>(Number));
          } else if (FString(Kind) == TEXT("staticSwitchParameters") && Entry.Value.IsValid() && Entry.Value->Type == EJson::Boolean && Entry.Value->TryGetBool(Boolean)) {
            Switches->SetBoolField(Key, Boolean);
          }
        }
      }
    }
  }
  bool bDeclared = false;
  const bool bRemapped = WantsRemappedUV3(Profile, Material, bDeclared);
  Switches->SetBoolField(TEXT("use remapped uv3"), bRemapped);
  if (bDeclared) {
    Switches->SetBoolField(TEXT("branch2enable"), bRemapped);
    Switches->SetBoolField(TEXT("hasbranch2data_internal"), bRemapped);
  }
  auto Result = MakeShared<FJsonObject>();
  Result->SetObjectField(TEXT("scalars"), Scalars);
  Result->SetObjectField(TEXT("switches"), Switches);
  return Result;
}

inline FString WindKey(const TSharedPtr<FJsonObject> &Profile,
                       const TSharedPtr<FJsonObject> &Material) {
  const auto Parameters = EffectiveWind(Profile, Material);
  return Parameters.IsValid() ? ObjectKey(Parameters) : FString();
}

inline bool HasValidUV3(const TArray<TArray<FVector2f>> &Lods) {
  if (Lods.IsEmpty()) return false;
  for (const auto &UVs : Lods) {
    if (UVs.IsEmpty()) return false;
    bool bHasData = false;
    for (const FVector2f &UV : UVs) {
      if (!FMath::IsFinite(UV.X) || !FMath::IsFinite(UV.Y)) return false;
      bHasData |= UV.X != 0.0f || UV.Y != 0.0f;
    }
    if (!bHasData) return false;
  }
  return true;
}

// 不同模型的 UV3 顶点值可以不同；共享 MI 需要完整一致的解码参数和编码约定。
inline FString RemapProof(const TSharedPtr<FJsonObject> &Profile,
                          const TSharedPtr<FJsonObject> &Material, bool bValidUV3) {
  bool bDeclared = false;
  if (!bValidUV3 || !WantsRemappedUV3(Profile, Material, bDeclared)) return FString();
  auto Decoder = MakeShared<FJsonObject>();
  const TSet<FName> Required = {FName(TEXT("UV3 Remap Bounds Mode")), FName(TEXT("xMin")), FName(TEXT("xMax")),
                               FName(TEXT("yMin")), FName(TEXT("yMax")), FName(TEXT("wMin")), FName(TEXT("wMax"))};
  for (const auto &Source : {Profile, Material}) {
    const TSharedPtr<FJsonObject> *Scalars = nullptr;
    if (!Source.IsValid() || !Source->TryGetObjectField(TEXT("scalarParameters"), Scalars) ||
        !Scalars || !Scalars->IsValid()) continue;
    TSet<FName> Seen;
    for (const auto &Entry : (*Scalars)->Values) {
      const FName Name(*Entry.Key);
      if (!Required.Contains(Name)) continue;
      double Value = 0;
      if (Seen.Contains(Name) || !Entry.Value.IsValid() || Entry.Value->Type != EJson::Number ||
          !Entry.Value->TryGetNumber(Value) || !FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt) return FString();
      Seen.Add(Name);
      // 只比较实际写入 MI 的 UV3 解码值；其他风参数统一沿用首个变体。
      Decoder->SetNumberField(FString(*Entry.Key).ToLower(), static_cast<float>(Value));
    }
  }
  for (const TCHAR *Name : {TEXT("uv3 remap bounds mode"), TEXT("xmin"), TEXT("xmax"),
                            TEXT("ymin"), TEXT("ymax"), TEXT("wmin"), TEXT("wmax")}) {
    if (!Decoder->HasField(Name)) return FString();
  }
  const double Mode = Decoder->GetNumberField(TEXT("uv3 remap bounds mode"));
  if (Mode != 1.0 && Mode != 2.0) return FString();
  const TSharedPtr<FJsonObject> *Encoding = nullptr;
  if (Profile.IsValid() && Profile->TryGetObjectField(TEXT("uv3Encoding"), Encoding) && Encoding && Encoding->IsValid()) {
    Decoder->SetObjectField(TEXT("encoding"), *Encoding);
  }
  return Hash(ObjectKey(Decoder));
}

inline FString InstanceKey(const FString &Compatibility, const FString &Variant,
                           bool bRemappedUV3, const FString &UV3Proof) {
  const FString Policy = bRemappedUV3
      ? (UV3Proof.IsEmpty() ? TEXT("variant:") + Variant : TEXT("uv3:") + UV3Proof)
      : FString(TEXT("compatible"));
  return Hash(Compatibility + TEXT("|") + Policy);
}
}
