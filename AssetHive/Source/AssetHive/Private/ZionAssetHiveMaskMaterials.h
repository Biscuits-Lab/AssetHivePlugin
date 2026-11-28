#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "StaticMeshAttributes.h"

namespace AssetHiveMaskMaterials {

inline FString ObjectStem(FString Name) {
	Name.RemoveFromStart(TEXT("Env_"), ESearchCase::IgnoreCase);
	if (!Name.RemoveFromStart(TEXT("Mask_"), ESearchCase::IgnoreCase))
		Name.RemoveFromStart(TEXT("MEGA_"), ESearchCase::IgnoreCase);
	return TEXT("Env_Mask_") + Name;
}

struct FLayer {
	int32 UVSet = 1;
	bool bMetallic = false;
	UTexture2D* BCR = nullptr;
	UTexture2D* Normal = nullptr;
	UTexture2D* Metallic = nullptr;
	FLinearColor Tiling = FLinearColor(1, 1, 0, 0);
};

struct FConfig {
	int32 BaseUV = 0;
	int32 MaskUV = 1;
	bool bFullAO = false;
	UMaterialInterface* Parent = nullptr;
	TMap<FString, FLayer> Layers;
};

inline bool ReadUV(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32& Value, FString& Error) {
	double Number = Value;
	if (Object->HasField(Key) && (!Object->TryGetNumberField(Key, Number) ||
			!FMath::IsFinite(Number) || Number < 0 || Number > 7 || FMath::FloorToDouble(Number) != Number)) {
		Error = FString::Printf(TEXT("Mask %s 必须是 0–7 的 UV 集索引"), Key);
		return false;
	}
	Value = static_cast<int32>(Number);
	return true;
}

inline bool LoadReference(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, UTexture2D*& Texture, FString& Error) {
	FString Path;
	Object->TryGetStringField(Key, Path);
	if (Path.IsEmpty()) return true;
	if (Path.StartsWith(TEXT("/Game/"))) Texture = LoadObject<UTexture2D>(nullptr, *Path);
	if (!Texture) {
		Error = TEXT("Mask 通道引用的项目纹理不存在：") + Path;
		return false;
	}
	// 项目父材质的 Tiling 层使用普通采样，引用纹理不能开启 VT。
	if (Texture->VirtualTextureStreaming) {
		Error = TEXT("Mask Tiling 通道需要普通采样纹理，当前引用开启了 VT：") + Path;
		return false;
	}
	return true;
}

inline bool Parse(const TSharedPtr<FJsonObject>& Object, FConfig& Config, FString& Error) {
	if (!Object.IsValid()) {
		Error = TEXT("Mask 导出缺少通道配置，请使用支持多通道 tiling 的 AssetHive 重新导出");
		return false;
	}
	if (!ReadUV(Object, TEXT("baseUvSet"), Config.BaseUV, Error) ||
			!ReadUV(Object, TEXT("maskUvSet"), Config.MaskUV, Error)) return false;
	Object->TryGetBoolField(TEXT("fullUv2Ao"), Config.bFullAO);
	const TArray<TSharedPtr<FJsonValue>>* Active = nullptr;
	const TSharedPtr<FJsonObject>* Layers = nullptr;
	if (!Object->TryGetArrayField(TEXT("activeChannels"), Active) ||
			!Object->TryGetObjectField(TEXT("channels"), Layers)) {
		Error = TEXT("Mask 通道检测结果缺失");
		return false;
	}
	const FString Order = TEXT("RGBA");
	int32 Last = INDEX_NONE;
	for (const auto& Value : *Active) {
		const FString Channel = Value.IsValid() && Value->Type == EJson::String ? Value->AsString() : FString();
		const int32 Index = Channel.Len() == 1 ? Order.Find(Channel) : INDEX_NONE;
		const TSharedPtr<FJsonObject>* LayerObject = nullptr;
		if (Index == INDEX_NONE || Config.Layers.Contains(Channel) || !(*Layers)->TryGetObjectField(Channel, LayerObject)) {
			Error = TEXT("Mask 通道配置无效：") + Channel;
			return false;
		}
		Last = FMath::Max(Last, Index);
		FLayer Layer;
		if (!ReadUV(*LayerObject, TEXT("uvSet"), Layer.UVSet, Error)) return false;
		(*LayerObject)->TryGetBoolField(TEXT("metallicEnabled"), Layer.bMetallic);
		if (!LoadReference(*LayerObject, TEXT("bcr"), Layer.BCR, Error) ||
				!LoadReference(*LayerObject, TEXT("normal"), Layer.Normal, Error) ||
				(Layer.bMetallic && !LoadReference(*LayerObject, TEXT("metallic"), Layer.Metallic, Error))) return false;
		Config.Layers.Add(Channel, Layer);
	}
	const FString Suffix = Last == INDEX_NONE ? TEXT("Base") : Order.Left(Last + 1);
	const FString ParentPath = TEXT("/Game/MaterialLibrary/Environment/MI_Mask/MI_Env_Mask_BakedDiffuse_") + Suffix;
	Config.Parent = LoadObject<UMaterialInterface>(nullptr, *ParentPath);
	if (!Config.Parent) {
		Error = TEXT("缺少 Mask 父材质：") + ParentPath;
		return false;
	}
	return true;
}

// 读取源 UV 与网格尺寸计算纹素密度，不改写模型 UV 坐标。
inline bool MeasureTiling(FConfig& Config, const TArray<UStaticMesh*>& Meshes,
		const TMap<UStaticMesh*, TArray<int32>>& NamedGroups, int32 GroupId, int32 GroupCount, FString& Error) {
	bool bMeasured = false;
	TSet<int32> RequiredUVs = {Config.BaseUV};
	if (!Config.Layers.IsEmpty()) RequiredUVs.Add(Config.MaskUV);
	if (Config.bFullAO) RequiredUVs.Add(1);
	for (const auto& Layer : Config.Layers) RequiredUVs.Add(Layer.Value.UVSet);
	for (UStaticMesh* Mesh : Meshes) {
		if (!Mesh) continue;
		const FMeshDescription* Description = Mesh->GetMeshDescription(0);
		if (!Description) { Error = TEXT("Mask 无法读取网格 LOD0：") + Mesh->GetName(); return false; }
		const FStaticMeshConstAttributes Attributes(*Description);
		const auto UVs = Attributes.GetVertexInstanceUVs();
		const auto Positions = Attributes.GetVertexPositions();
		const auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
		const auto* Assignments = NamedGroups.Find(Mesh);
		const auto& Materials = Mesh->GetStaticMaterials();
		const FVector Scale = Mesh->GetSourceModel(0).BuildSettings.BuildScale3D;
		double WorldArea = 0;
		TMap<int32, double> UVAreas;
		bool bValidatedUVs = false;
		for (const auto Triangle : Description->Triangles().GetElementIDs()) {
			const FName SlotName = SlotNames[Description->GetTrianglePolygonGroup(Triangle)];
			const int32 SlotIndex = Materials.IndexOfByPredicate([&](const FStaticMaterial& Slot) {
				return Slot.ImportedMaterialSlotName == SlotName || Slot.MaterialSlotName == SlotName;
			});
			const int32 AssignedGroup = Assignments && Assignments->IsValidIndex(SlotIndex)
					? (*Assignments)[SlotIndex] : (GroupCount == 1 ? GroupId : SlotIndex + 1);
			if (AssignedGroup != GroupId) continue;
			if (!bValidatedUVs) for (int32 UV : RequiredUVs) {
				if (UV >= UVs.GetNumChannels()) {
					Error = FString::Printf(TEXT("%s 缺少 Mask 配置所需的 UV%d（索引 %d）"), *Mesh->GetName(), UV + 1, UV);
					return false;
				}
			}
			bValidatedUVs = true;
			const auto Corners = Description->GetTriangleVertexInstances(Triangle);
			const FVector P0 = FVector(Positions[Description->GetVertexInstanceVertex(Corners[0])]) * Scale;
			const FVector P1 = FVector(Positions[Description->GetVertexInstanceVertex(Corners[1])]) * Scale;
			const FVector P2 = FVector(Positions[Description->GetVertexInstanceVertex(Corners[2])]) * Scale;
			const double Area = FVector::CrossProduct(P1 - P0, P2 - P0).Size() * 0.5;
			if (!FMath::IsFinite(Area)) { Error = TEXT("Mask 网格表面积无效：") + Mesh->GetName(); return false; }
			if (Area <= UE_SMALL_NUMBER) continue;
			WorldArea += Area;
			for (int32 UV : RequiredUVs) {
				const FVector2f A = UVs.Get(Corners[1], UV) - UVs.Get(Corners[0], UV);
				const FVector2f B = UVs.Get(Corners[2], UV) - UVs.Get(Corners[0], UV);
				UVAreas.FindOrAdd(UV) += FMath::Abs(static_cast<double>(A.X) * B.Y - static_cast<double>(A.Y) * B.X) * 0.5;
			}
		}
		if (WorldArea <= UE_SMALL_NUMBER) continue;
		for (auto& Pair : Config.Layers) {
			FLayer& Layer = Pair.Value;
			const double UVArea = UVAreas.FindRef(Layer.UVSet);
			if (UVArea <= UE_SMALL_NUMBER || !FMath::IsFinite(UVArea)) {
				Error = TEXT("Mask 通道 UV 面积无效，无法计算纹素密度：") + Mesh->GetName();
				return false;
			}
			UTexture* DensityTexture = Layer.BCR ? Layer.BCR : Layer.Normal;
			if (!DensityTexture) Config.Parent->GetTextureParameterValue(
					FMaterialParameterInfo(FName(*(TEXT("Tiling ") + Pair.Key + TEXT("_BCR")))), DensityTexture);
			if (!DensityTexture || DensityTexture->GetSurfaceWidth() <= 0 || DensityTexture->GetSurfaceHeight() <= 0) {
				Error = TEXT("Mask 通道纹理尺寸无效：") + Pair.Key;
				return false;
			}
			const double TexelsPerUV = 7.68 * FMath::Sqrt(WorldArea / UVArea); // UE 长度单位为厘米。
			const FLinearColor Tiling(TexelsPerUV / DensityTexture->GetSurfaceWidth(), TexelsPerUV / DensityTexture->GetSurfaceHeight(), 0, 0);
			if (bMeasured && (!FMath::IsNearlyEqual(Layer.Tiling.R, Tiling.R, FMath::Max(0.0001f, Tiling.R * 0.001f)) ||
					!FMath::IsNearlyEqual(Layer.Tiling.G, Tiling.G, FMath::Max(0.0001f, Tiling.G * 0.001f)))) {
				Error = TEXT("同名 Mask 材质的网格 UV 密度不同，无法用同一 Tiling 达到 768 px/m：") + Mesh->GetName();
				return false;
			}
			Layer.Tiling = Tiling;
		}
		bMeasured = true;
	}
	if (!bMeasured) Error = TEXT("Mask 材质组没有可测量的模型表面");
	return bMeasured;
}

inline bool MatchesTiling(const FConfig& Config, UMaterialInstanceConstant* Instance) {
	for (const auto& Pair : Config.Layers) {
		FLinearColor Existing;
		if (!Instance->GetVectorParameterValue(FMaterialParameterInfo(FName(*(TEXT("Tiling ") + Pair.Key + TEXT(" Tiling_Offset")))), Existing) ||
				!Existing.Equals(Pair.Value.Tiling, 0.0001f)) return false;
	}
	return true;
}

inline void Apply(const FConfig& Config, UMaterialInstanceConstant* Instance, UTexture* FullAO) {
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Base_UVSet")), Config.BaseUV);
	Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Mask_UVSet")), Config.MaskUV);
	Instance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Use Full UV2 AO")), Config.bFullAO && FullAO != nullptr);
	if (FullAO) Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Full AO_UV2_VT")), FullAO);
	for (const FString& Channel : {FString(TEXT("R")), FString(TEXT("G")), FString(TEXT("B")), FString(TEXT("A"))}) {
		const FLayer* Layer = Config.Layers.Find(Channel);
		Instance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(FName(*(TEXT("Use Tiling") + Channel))), Layer != nullptr);
		const FString Prefix = TEXT("Tiling ") + Channel;
		Instance->SetStaticSwitchParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT("_UseMetallic")))), Layer && Layer->bMetallic);
		if (!Layer) continue;
		Instance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT("_UVSet")))), Layer->UVSet);
		Instance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT(" Tiling_Offset")))), Layer->Tiling);
		if (Layer->BCR) Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT("_BCR")))), Layer->BCR);
		if (Layer->Normal) Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT("_Normal")))), Layer->Normal);
		if (Layer->bMetallic && Layer->Metallic) Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(*(Prefix + TEXT("_M")))), Layer->Metallic);
	}
}
}
