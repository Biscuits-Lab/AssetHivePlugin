#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"

namespace AssetHiveZion3DMaterials {
// Match the authored material identity, preserving category/number/variant
// tokens. Never strip Kit, posun, _gaotai, or numeric duplicate suffixes.
inline FString Stem(FString Name) {
  Name.TrimStartAndEndInline();
  if (Name.StartsWith(TEXT("MI_"), ESearchCase::IgnoreCase)) Name.RightChopInline(3);
  else if (Name.StartsWith(TEXT("M_"), ESearchCase::IgnoreCase) ||
           Name.StartsWith(TEXT("T_"), ESearchCase::IgnoreCase)) Name.RightChopInline(2);
  for (const TCHAR* Resolution : {TEXT("_1K"), TEXT("_2K"), TEXT("_4K"), TEXT("_8K"), TEXT("_16K")}) {
    if (Name.EndsWith(Resolution, ESearchCase::IgnoreCase)) {
      Name.LeftChopInline(FCString::Strlen(Resolution));
      break;
    }
  }
  return Name;
}

inline FString Key(const FString& Name) { return Stem(Name).ToLower(); }

inline int32 FindGroup(const FStaticMaterial& Slot, const TMap<FString, int32>& Groups) {
  if (!Slot.ImportedMaterialSlotName.IsNone()) {
    if (const int32* Group = Groups.Find(Key(Slot.ImportedMaterialSlotName.ToString()))) return *Group;
  }
  if (!Slot.MaterialSlotName.IsNone()) {
    if (const int32* Group = Groups.Find(Key(Slot.MaterialSlotName.ToString()))) return *Group;
  }
  return INDEX_NONE;
}
}
