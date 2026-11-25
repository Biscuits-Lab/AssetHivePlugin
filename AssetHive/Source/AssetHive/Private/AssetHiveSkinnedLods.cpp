#include "AssetHiveModelCatalogLibrary.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "AssetCompilingManager.h"
#include "SkeletalMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "MeshDescriptionToDynamicMesh.h"
#include "Operations/TransferBoneWeights.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonSerializer.h"
#include "Materials/MaterialInterface.h"
#include "Misc/SecureHash.h"
#include "ClothingAsset.h"

namespace {
bool HasBaseClothBindings(const USkeletalMesh* Model, int32 Level) {
    const auto* Imported = Model->GetImportedModel();
    if (!Imported || !Imported->LODModels.IsValidIndex(Level)) return false;
    for (const auto& Base : Imported->LODModels[0].Sections) {
        if (!Base.ClothingData.AssetGuid.IsValid()) continue;
        bool Found = false;
        for (const auto& Section : Imported->LODModels[Level].Sections) {
            if (Section.MaterialIndex == Base.MaterialIndex
                && Section.ClothingData.AssetGuid == Base.ClothingData.AssetGuid
                && Section.NumVertices > 0 && Section.ClothMappingDataLODs.Num()
                && Section.ClothMappingDataLODs[0].Num() >= Section.NumVertices) Found = true;
        }
        if (!Found) return false;
    }
    return true;
}
FString SkinKey(int32 Level) { return FString::Printf(TEXT("AssetHive.SkinLOD.%d"), Level); }
FString MeshHash(const FMeshDescription* Mesh) {
    return Mesh ? FStaticMeshOperations::ComputeSHAHash(*Mesh, true).ToString() : FString();
}
TArray<FString> GeometrySignature(const FPositionVertexBuffer& Vertices, const TArray<uint32>& Indices,
    const TArray<int32>& Materials) {
    TArray<FString> Triangles;
    for (int32 I = 0; I + 2 < Indices.Num(); I += 3) {
        TArray<FString> Corners;
        for (int32 J = 0; J < 3; ++J) {
            const FVector3f P = Vertices.VertexPosition(Indices[I + J]);
            Corners.Add(FString::Printf(TEXT("%lld,%lld,%lld"), FMath::RoundToInt64(P.X * 1000.0),
                FMath::RoundToInt64(P.Y * 1000.0), FMath::RoundToInt64(P.Z * 1000.0)));
        }
        // Vertex splitting at UV/material seams is allowed, geometry changes aren't.
        Corners.Sort(); Triangles.Add(FString::FromInt(Materials[I / 3]) + TEXT(":") + FString::Join(Corners, TEXT(";")));
    }
    Triangles.Sort();
    return Triangles;
}
FString Json(const TSharedRef<FJsonObject>& Object) {
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
    return Result;
}
}

// Geometry is copied from the static chain; only bone weights are transferred.
// All five descriptions are prepared and validated before modifying the rig.
FString UAssetHiveModelCatalogLibrary::CompleteSkinnedLodsFromStatic(USkeletalMesh* Model,
    const FString& SourcePath, const FString& SourceHash, int32 SourceLod, bool bRegenerate, const FString& SourceMaterialsJson)
{
    auto Result = MakeShared<FJsonObject>();
    auto Fail = [&](const FString& Message) { Result->SetStringField(TEXT("error"), Message); return Json(Result); };
    if (!Model || SourceHash.IsEmpty() || SourceLod < 0 || SourceLod > 1)
        return Fail(TEXT("Invalid static LOD source"));
    FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
    const FMeshDescription* Rig = Model->GetMeshDescription(0);
    if (!Rig || !Model->GetSkeleton() || Rig->Triangles().Num() == 0)
        return Fail(TEXT("SK LOD0 with a valid skeleton is required"));
    auto& Metadata = Model->GetOutermost()->GetMetaData();
    const FString RigHash = MeshHash(Rig);
    TSharedPtr<FJsonObject> SourceMaterials;
    if (!SourceMaterialsJson.IsEmpty() && !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SourceMaterialsJson), SourceMaterials))
        return Fail(TEXT("Invalid source material mapping"));
    const FString Revision = TEXT("skin-v5:") + SourceHash + TEXT(":") + FString::FromInt(SourceLod) + TEXT(":") + RigHash
        + TEXT(":") + FMD5::HashAnsiString(*SourceMaterialsJson);
    TArray<int32> Pending;
    for (int32 Level = 1; Level <= 5; ++Level) {
        const auto* Info = Model->GetLODInfo(Level);
        const FString Saved = Metadata.GetValue(Model, *SkinKey(Level));
        if (Info && Model->HasMeshDescription(Level) && Saved.IsEmpty() && !Info->bHasBeenSimplified
            && !(Info->ReductionSettings.BaseLOD > 0 && Info->ReductionSettings.BaseLOD < Level)
            && Metadata.GetValue(Model, *FString::Printf(TEXT("AssetHive.ComponentLOD.%d"), Level)) != TEXT("2")) {
            // Artist-authored levels have priority; never silently replace them.
            return CompleteCharacterLods(Model, {0}, bRegenerate, false);
        }
        const FString Expected = Revision + TEXT(":") + MeshHash(Info && Model->HasMeshDescription(Level) ? Model->GetMeshDescription(Level) : nullptr);
        const auto* Render = Model->GetResourceForRendering();
        if (!Info || Saved != Expected || !Render || !Render->LODRenderData.IsValidIndex(Level)
            || Render->LODRenderData[Level].GetTotalFaces() <= 0 || !HasBaseClothBindings(Model, Level)) Pending.Add(Level);
    }
    if (bRegenerate && !Pending.IsEmpty()) {
        TStrongObjectPtr<UStaticMesh> Reference(NewObject<UStaticMesh>());
        TStrongObjectPtr<UStaticMesh> Static(Cast<UStaticMesh>(ParseLinkMeshSource(Reference.Get(), SourcePath)));
        if (!Static || !Static->GetMeshDescription(SourceLod)) return Fail(TEXT("Cannot read SM LOD1 geometry"));
        if (SourceLod == 0) {
            FMeshDescription First(*Static->GetMeshDescription(0));
            while (Static->GetNumSourceModels() <= 1) Static->AddSourceModel();
            Static->CreateMeshDescription(1, MoveTemp(First));
            Static->CommitMeshDescription(1);
            ProtectImportedLod(Static.Get(), 1);
            Static->PostEditChange();
            FAssetCompilingManager::Get().FinishCompilationForObjects({Static.Get()});
        }
        TSharedPtr<FJsonObject> StaticReport;
        const FString Report = CompleteCharacterLods(Static.Get(), {0, 1}, true, true);
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Report), StaticReport)
            || StaticReport->GetArrayField(TEXT("missing")).Num() || StaticReport->GetArrayField(TEXT("pending")).Num())
            return Fail(TEXT("SM LOD chain did not pass structure protection"));

        UE::Geometry::FDynamicMesh3 Source;
        FMeshDescriptionToDynamicMesh Converter;
        Converter.Convert(Rig, Source);
        const auto RigAttributes = FSkeletalMeshConstAttributes(*Rig);
        const auto RigPositions = RigAttributes.GetVertexPositions();
        const auto RigNormals = RigAttributes.GetVertexInstanceNormals();
        Source.EnableVertexNormals(FVector3f::ZeroVector);
        for (int32 Vertex : Source.VertexIndicesItr()) {
            FVector3f Normal = FVector3f::ZeroVector;
            for (FVertexInstanceID Instance : Rig->GetVertexVertexInstances(Converter.VertIDMap[Vertex])) Normal += RigNormals[Instance];
            Source.SetVertexNormal(Vertex, Normal.GetSafeNormal());
        }
        FBox3d Bounds(ForceInit);
        for (FVertexID V : Rig->Vertices().GetElementIDs()) Bounds += FVector3d(RigPositions[V]);
        // Artist LODs can move surface vertices during retopology. A bounded
        // five-percent correspondence envelope tolerates that local change,
        // while still rejecting translated/unrelated parts before any write.
        const double Radius = FMath::Max(.01, Bounds.GetSize().Length() * .05);
        TArray<FMeshDescription> Prepared;
        Prepared.SetNum(6);
        TMap<FName, int32> SourceSlotToTarget;
        const auto& RefSkeleton = Model->GetRefSkeleton();
        for (int32 Level : Pending) {
            const FMeshDescription* Geometry = Static->GetMeshDescription(Level);
            if (!Geometry || Geometry->Triangles().Num() == 0) return Fail(TEXT("SM LOD geometry is missing"));
            Prepared[Level] = *Geometry;
            auto& Target = Prepared[Level];
            FSkeletalMeshAttributes Attributes(Target);
            Attributes.Register(true);
            // MeshDescription weights use global reference-skeleton indices.
            for (int32 Bone = 0; Bone < RefSkeleton.GetNum(); ++Bone) {
                const FBoneID Id = Attributes.CreateBone();
                Attributes.GetBoneNames()[Id] = RefSkeleton.GetBoneName(Bone);
                Attributes.GetBoneParentIndices()[Id] = RefSkeleton.GetParentIndex(Bone);
                Attributes.GetBonePoses()[Id] = RefSkeleton.GetRefBonePose()[Bone];
            }
            auto SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();
            TMap<FPolygonGroupID, FPolygonGroupID> GroupRemap;
            for (FPolygonGroupID Group : Target.PolygonGroups().GetElementIDs()) {
                const FName Name = SlotNames[Group];
                int32 Matches = 0;
                FName Canonical;
                for (const FSkeletalMaterial& Material : Model->GetMaterials()) {
                    if (Name == Material.ImportedMaterialSlotName || Name == Material.MaterialSlotName) {
                        ++Matches; Canonical = Material.ImportedMaterialSlotName.IsNone() ? Material.MaterialSlotName : Material.ImportedMaterialSlotName;
                    }
                }
                // Imported slot labels can differ between the SM and existing rig.
                // Resolve only a unique, already-established material identity.
                FString ExpectedPath;
                if (Matches == 0 && SourceMaterials.IsValid() && SourceMaterials->TryGetStringField(Name.ToString(), ExpectedPath)) {
                    FString PackagePath;
                    if (ExpectedPath.Split(TEXT("."), &PackagePath, nullptr)) ExpectedPath = PackagePath;
                    for (const FSkeletalMaterial& Material : Model->GetMaterials()) {
                        if (Material.MaterialInterface && Material.MaterialInterface->GetOutermost()->GetName().Equals(ExpectedPath, ESearchCase::IgnoreCase)) {
                            ++Matches; Canonical = Material.ImportedMaterialSlotName.IsNone() ? Material.MaterialSlotName : Material.ImportedMaterialSlotName;
                        }
                    }
                }
                if (Matches != 1) return Fail(TEXT("SM/SK material slots do not match uniquely: ") + Name.ToString());
                SlotNames[Group] = Canonical;
                const int32 TargetIndex = Model->GetMaterials().IndexOfByPredicate([&](const FSkeletalMaterial& Material) {
                    return (Material.ImportedMaterialSlotName.IsNone() ? Material.MaterialSlotName : Material.ImportedMaterialSlotName) == Canonical;
                });
                if (TargetIndex == INDEX_NONE) return Fail(TEXT("Missing canonical SK material slot"));
                SourceSlotToTarget.Add(Name, TargetIndex);
                GroupRemap.Add(Group, FPolygonGroupID(TargetIndex));
            }
            // UE only remaps imported material names for LOD0. Higher LODs use
            // numeric polygon-group indices, so renaming the slots alone assigns
            // the wrong materials when the SM and SK arrays have different order.
            Target.RemapPolygonGroups(GroupRemap);
            // Keep even unused slots: import data enumerates groups in ID order.
            for (int32 Index = 0; Index < Model->GetMaterials().Num(); ++Index) {
                const FPolygonGroupID Group(Index);
                if (!Target.PolygonGroups().IsValid(Group)) Target.CreatePolygonGroupWithID(Group);
                const auto& Material = Model->GetMaterials()[Index];
                SlotNames[Group] = Material.ImportedMaterialSlotName.IsNone() ? Material.MaterialSlotName : Material.ImportedMaterialSlotName;
            }
            const auto Positions = Attributes.GetVertexPositions();
            const auto Normals = Attributes.GetVertexInstanceNormals();
            for (FName Profile : RigAttributes.GetSkinWeightProfileNames()) {
                if (Profile != FSkeletalMeshAttributes::DefaultSkinWeightProfileName) Attributes.RegisterSkinWeightAttribute(Profile);
                auto Weights = Attributes.GetVertexSkinWeights(Profile);
                UE::Geometry::FTransferBoneWeights Transfer(&Source, Profile);
                Transfer.SearchRadius = Radius;
                // Artist shading normals differ between static and skinned imports;
                // they are not a reliable measure of geometric correspondence.
                // Use nearest-surface barycentrics with the bounded spatial check.
                Transfer.NormalThreshold = -1;
                Transfer.MaxNumInfluences = UE::AnimationCore::MaxInlineBoneWeightCount;
                if (Transfer.Validate() != UE::Geometry::EOperationValidationResult::Ok)
                    return Fail(TEXT("SK LOD0 skin weights cannot be transferred"));
                for (FVertexID V : Target.Vertices().GetElementIDs()) {
                    FVector3d Normal = FVector3d::ZeroVector;
                    for (FVertexInstanceID Instance : Target.GetVertexVertexInstances(V)) Normal += FVector3d(Normals[Instance]);
                    Normal.Normalize();
                    UE::AnimationCore::FBoneWeights Value;
                    if (!Transfer.TransferWeightsToPoint(Value, FVector3d(Positions[V]), nullptr, FVector3f(Normal)) || Value.Num() == 0) {
                        Transfer.NormalThreshold = -1;
                        const bool RadiusOnly = Transfer.TransferWeightsToPoint(Value, FVector3d(Positions[V]), nullptr, FVector3f(Normal));
                        Transfer.SearchRadius = -1;
                        const bool Unrestricted = Transfer.TransferWeightsToPoint(Value, FVector3d(Positions[V]), nullptr, FVector3f(Normal));
                        return Fail(FString::Printf(TEXT("SM LOD1 is not aligned with SK LOD0; skin transfer rejected (LOD%d vertex%d pos=%s bounds=%s radius=%.4f distanceCheck=%d nearest=%d weights=%d profile=%s)"),
                            Level, V.GetValue(), *FVector(Positions[V]).ToString(), *Bounds.ToString(), Radius, RadiusOnly, Unrestricted, Value.Num(), *Profile.ToString()));
                    }
                    for (const auto& Weight : Value) if (Weight.GetBoneIndex() >= RefSkeleton.GetNum())
                        return Fail(TEXT("Transferred skin contains an invalid bone index"));
                    Weights.Set(V, Value);
                }
            }
        }
        auto Install = [&](USkeletalMesh* Target) -> bool {
            Target->Modify();
            // A binding belongs to a particular topology. Never let the mesh
            // rebuild restore an old mapping onto newly generated vertices.
            // Reuse the internal cloth LOD instead of growing orphan copies.
            TMap<FGuid, TMap<int32, int32>> PreviousClothLods;
            for (UClothingAssetBase* Asset : Target->GetMeshClothingAssets()) {
                auto* Cloth = Cast<UClothingAssetCommon>(Asset);
                if (!Cloth) continue;
                for (int32 Level : Pending) {
                    if (!Cloth->LodMap.IsValidIndex(Level) || Cloth->LodMap[Level] == INDEX_NONE) continue;
                    PreviousClothLods.FindOrAdd(Cloth->GetAssetGuid()).Add(Level, Cloth->LodMap[Level]);
                    Cloth->UnbindFromSkeletalMesh(Target, Level);
                }
            }
            while (Target->GetLODNum() < 6) Target->AddLODInfo();
            Target->SetNumSourceModels(FMath::Max(6, Target->GetNumSourceModels()));
            while (Target->GetImportedModel()->LODModels.Num() < Target->GetLODNum())
                Target->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
            for (int32 Level : Pending) {
                auto* Info = Target->GetLODInfo(Level);
                Info->LODMaterialMap.Reset();
                Info->BuildSettings = Target->GetLODInfo(0)->BuildSettings;
                Info->BuildSettings.bRecomputeNormals = false;
                Info->BuildSettings.bRecomputeTangents = false;
                Info->ReductionSettings = FSkeletalMeshOptimizationSettings();
                Info->ReductionSettings.BaseLOD = Level;
                Info->ReductionSettings.NumOfTrianglesPercentage = 1.f;
                Info->ReductionSettings.NumOfVertPercentage = 1.f;
                Info->bHasBeenSimplified = false;
                Target->CreateMeshDescription(Level, FMeshDescription(Prepared[Level]));
                Target->CommitMeshDescription(Level);
            }
            Target->PostEditChange();
            FAssetCompilingManager::Get().FinishCompilationForObjects({Target});
            // Bone-weight transfer does not transfer cloth simulation bindings.
            // Keep LOD0 physics intact and rebuild render-to-simulation mappings
            // for each generated level using the same authored physical mesh.
            {
            FScopedSkeletalMeshPostEditChange ClothEdit(Target);
            const auto BaseSections = Target->GetImportedModel()->LODModels[0].Sections;
            for (const auto& Base : BaseSections) {
                if (!Base.ClothingData.AssetGuid.IsValid()) continue;
                auto* Cloth = Cast<UClothingAssetCommon>(Target->GetClothingAsset(Base.ClothingData.AssetGuid));
                if (!Cloth || !Cloth->LodData.IsValidIndex(Base.ClothingData.AssetLodIndex)
                    || Cloth->GetOuter() != Target) return false;
                const auto Physical = Cloth->LodData[Base.ClothingData.AssetLodIndex];
                for (int32 Level : Pending) {
                    auto& Sections = Target->GetImportedModel()->LODModels[Level].Sections;
                    int32 SectionIndex = INDEX_NONE;
                    for (int32 Index = 0; Index < Sections.Num(); ++Index) {
                        if (Sections[Index].MaterialIndex != Base.MaterialIndex) continue;
                        if (SectionIndex != INDEX_NONE) return false;
                        SectionIndex = Index;
                    }
                    if (SectionIndex == INDEX_NONE) return false;
                    if (Sections[SectionIndex].ClothingData.AssetGuid.IsValid()) return false;
                    // UE requires a separate internal cloth LOD per mesh LOD.
                    const auto* Previous = PreviousClothLods.Find(Cloth->GetAssetGuid());
                    const int32* Reusable = Previous ? Previous->Find(Level) : nullptr;
                    const int32 ClothLevel = Reusable && *Reusable != Base.ClothingData.AssetLodIndex
                        && Cloth->LodData.IsValidIndex(*Reusable) ? *Reusable : Cloth->AddNewLod();
                    Cloth->LodData[ClothLevel] = Physical;
                    if (!Cloth->BindToSkeletalMesh(Target, Level, SectionIndex, ClothLevel)) return false;
                    auto& Section = Sections[SectionIndex];
                    auto& User = Target->GetImportedModel()->LODModels[Level].UserSectionsData.FindOrAdd(Section.OriginalDataSectionIndex);
                    User.ClothingData = Section.ClothingData;
                    User.CorrespondClothAssetIndex = Section.CorrespondClothAssetIndex;
                }
                Cloth->BuildLodTransitionData();
            }
            }
            Target->PostEditChange();
            FAssetCompilingManager::Get().FinishCompilationForObjects({Target});
            for (int32 Level : Pending) if (!HasBaseClothBindings(Target, Level)) return false;
            return true;
        };
        // Validate the final engine build on a private copy before touching UE assets.
        TStrongObjectPtr<USkeletalMesh> Preview(DuplicateObject<USkeletalMesh>(Model, GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), USkeletalMesh::StaticClass())));
        if (!Install(Preview.Get())) return Fail(TEXT("Cannot safely rebuild generated LOD cloth bindings"));
        const auto* Render = Preview->GetResourceForRendering();
        for (int32 Level : Pending) {
            if (!Render || !Render->LODRenderData.IsValidIndex(Level)) return Fail(TEXT("SK LOD render build failed"));
            const auto& SkLod = Render->LODRenderData[Level];
            const auto& SmLod = Static->GetRenderData()->LODResources[Level];
            TArray<uint32> SkIndices, SmIndices;
            SkLod.MultiSizeIndexContainer.GetIndexBuffer(SkIndices);
            const auto SmView = SmLod.IndexBuffer.GetArrayView();
            for (int32 I = 0; I < SmView.Num(); ++I) SmIndices.Add(SmView[I]);
            TArray<int32> SkMaterials, SmMaterials;
            SkMaterials.Init(INDEX_NONE, SkIndices.Num() / 3);
            SmMaterials.Init(INDEX_NONE, SmIndices.Num() / 3);
            for (const auto& Section : SkLod.RenderSections) {
                if (!Model->GetMaterials().IsValidIndex(Section.MaterialIndex)) return Fail(TEXT("Invalid SK LOD material index"));
                for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
                    SkMaterials[Section.BaseIndex / 3 + Triangle] = Section.MaterialIndex;
            }
            for (const auto& Section : SmLod.Sections) {
                if (!Static->GetStaticMaterials().IsValidIndex(Section.MaterialIndex)) return Fail(TEXT("Invalid SM LOD material index"));
                const auto& Material = Static->GetStaticMaterials()[Section.MaterialIndex];
                const int32* TargetIndex = SourceSlotToTarget.Find(Material.ImportedMaterialSlotName.IsNone()
                    ? Material.MaterialSlotName : Material.ImportedMaterialSlotName);
                if (!TargetIndex) return Fail(TEXT("Missing SM/SK LOD material correspondence"));
                for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
                    SmMaterials[Section.FirstIndex / 3 + Triangle] = *TargetIndex;
            }
            if (SkMaterials.Contains(INDEX_NONE) || SmMaterials.Contains(INDEX_NONE)
                || GeometrySignature(SkLod.StaticVertexBuffers.PositionVertexBuffer, SkIndices, SkMaterials)
                != GeometrySignature(SmLod.VertexBuffers.PositionVertexBuffer, SmIndices, SmMaterials)) {
                return Fail(TEXT("SM/SK LOD geometry or material assignment differs after build"));
            }
            const auto* Skin = SkLod.GetSkinWeightVertexBuffer();
            for (const auto& Section : SkLod.RenderSections) {
                for (uint32 Vertex = Section.BaseVertexIndex; Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex) {
                    uint32 Sum = 0;
                    for (uint32 Influence = 0; Influence < Skin->GetMaxBoneInfluences(); ++Influence) {
                        const uint32 Weight = Skin->GetBoneWeight(Vertex, Influence);
                        if (Weight && !Section.BoneMap.IsValidIndex(Skin->GetBoneIndex(Vertex, Influence)))
                            return Fail(TEXT("SK LOD render skin has an invalid bone"));
                        Sum += Weight;
                    }
                    if (FMath::Abs(static_cast<int32>(Sum) - 65535) > 512)
                        return Fail(TEXT("SK LOD render skin is not normalized"));
                }
            }
        }
        if (!Install(Model)) return Fail(TEXT("Generated LOD cloth binding failed"));
        for (int32 Level : Pending)
            Metadata.SetValue(Model, *SkinKey(Level), *(Revision + TEXT(":") + MeshHash(Model->GetMeshDescription(Level))));
        ReleaseLinkMeshSource(Static.Get());
        Pending.Empty();
    }
    TArray<TSharedPtr<FJsonValue>> Levels, PendingJson, Automatic;
    const auto* Data = Model->GetResourceForRendering();
    for (int32 Level = 0; Data && Level < Data->LODRenderData.Num(); ++Level) {
        auto Row = MakeShared<FJsonObject>();
        Row->SetNumberField(TEXT("index"), Level);
        Row->SetNumberField(TEXT("triangles"), Data->LODRenderData[Level].GetTotalFaces());
        Row->SetStringField(TEXT("source"), Level == 0 ? TEXT("imported") : Level == 1 ? TEXT("existing") : TEXT("generated"));
        Row->SetBoolField(TEXT("structureVerified"), !Pending.Contains(Level));
        Row->SetNumberField(TEXT("baseLod"), 1);
        TArray<TSharedPtr<FJsonValue>> ClothSections;
        if (Model->GetImportedModel()->LODModels.IsValidIndex(Level)) {
            for (const auto& Section : Model->GetImportedModel()->LODModels[Level].Sections) {
                if (!Section.ClothingData.AssetGuid.IsValid()) continue;
                auto Cloth = MakeShared<FJsonObject>();
                Cloth->SetNumberField(TEXT("materialIndex"), Section.MaterialIndex);
                Cloth->SetNumberField(TEXT("originalSection"), Section.OriginalDataSectionIndex);
                Cloth->SetStringField(TEXT("assetGuid"), Section.ClothingData.AssetGuid.ToString());
                Cloth->SetNumberField(TEXT("clothLod"), Section.ClothingData.AssetLodIndex);
                Cloth->SetNumberField(TEXT("vertices"), Section.NumVertices);
                Cloth->SetNumberField(TEXT("mappingCount"), Section.ClothMappingDataLODs.Num() ? Section.ClothMappingDataLODs[0].Num() : 0);
                // Reconstruct rest positions to detect stale mappings, not just their count.
                if (const auto* Asset = Cast<UClothingAssetCommon>(Model->GetClothingAsset(Section.ClothingData.AssetGuid));
                    Asset && Asset->LodData.IsValidIndex(Section.ClothingData.AssetLodIndex) && Section.ClothMappingDataLODs.Num()) {
                    const auto& Physical = Asset->LodData[Section.ClothingData.AssetLodIndex].PhysicalMeshData;
                    const auto& Map = Section.ClothMappingDataLODs[0];
                    const int32 Influences = Section.NumVertices ? Map.Num() / Section.NumVertices : 0;
                    double MaxError = 0, SumError = 0;
                    int32 Count = 0;
                    for (int32 V = 0; V < Section.SoftVertices.Num() && Influences > 0; ++V) {
                        FVector3f Reconstructed = FVector3f::ZeroVector;
                        float Total = 0;
                        for (int32 I = 0; I < Influences; ++I) {
                            const auto& Link = Map[V * Influences + I];
                            const auto B = Link.PositionBaryCoordsAndDist;
                            FVector3f P = FVector3f::ZeroVector;
                            for (int32 K = 0; K < 3; ++K) {
                                const int32 Id = Link.SourceMeshVertIndices[K];
                                if (!Physical.Vertices.IsValidIndex(Id)) continue;
                                P += (Physical.Vertices[Id] + Physical.Normals[Id] * B.W) * B[K];
                            }
                            const float W = Influences == 1 ? 1.f : Link.Weight;
                            Reconstructed += P * W; Total += W;
                        }
                        if (Total > 0) {
                            const double Error = (Reconstructed / Total - Section.SoftVertices[V].Position).Length();
                            MaxError = FMath::Max(MaxError, Error); SumError += Error; ++Count;
                        }
                    }
                    Cloth->SetNumberField(TEXT("restMappingMaxError"), MaxError);
                    Cloth->SetNumberField(TEXT("restMappingMeanError"), Count ? SumError / Count : 0);
                }
                ClothSections.Add(MakeShared<FJsonValueObject>(Cloth));
            }
        }
        Row->SetArrayField(TEXT("clothSections"), ClothSections);
        Levels.Add(MakeShared<FJsonValueObject>(Row));
    }
    for (int32 Level : Pending) PendingJson.Add(MakeShared<FJsonValueNumber>(Level));
    for (int32 Level = 2; Level <= 5; ++Level) if (!Pending.Contains(Level)) Automatic.Add(MakeShared<FJsonValueNumber>(Level));
    Result->SetStringField(TEXT("policy"), TEXT("lod1-reduction-v2"));
    Result->SetNumberField(TEXT("qualityVersion"), 1);
    Result->SetNumberField(TEXT("skinTransferVersion"), 1);
    Result->SetStringField(TEXT("rigHash"), MeshHash(Model->GetMeshDescription(0)));
    if (MeshHash(Model->GetMeshDescription(0)) != RigHash) return Fail(TEXT("SK LOD0 changed unexpectedly"));
    Result->SetNumberField(TEXT("count"), Levels.Num());
    Result->SetArrayField(TEXT("levels"), Levels);
    Result->SetArrayField(TEXT("automatic"), Automatic);
    Result->SetArrayField(TEXT("pending"), PendingJson);
    for (const TCHAR* Field : {TEXT("existing"), TEXT("missingManual"), TEXT("missing")}) Result->SetArrayField(Field, {});
    return Json(Result);
}
