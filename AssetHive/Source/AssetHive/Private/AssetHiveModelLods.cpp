#include "AssetHiveModelCatalogLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "StaticMeshResources.h"
#include "LODUtilities.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "AssetCompilingManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "AssetHiveLodQuality.h"
#include "AssetHiveStaticLodReduction.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"

static FString GeneratedLodKey(int32 Index) { return FString::Printf(TEXT("AssetHive.ComponentLOD.%d"), Index); }
static bool IsGeneratedLod(UObject* Mesh, int32 Index) {
    return Mesh && Mesh->GetOutermost()->GetMetaData().GetValue(Mesh, *GeneratedLodKey(Index)) == TEXT("2");
}
static bool IsComponentLod(UStaticMesh* Mesh, int32 Index) {
    return IsGeneratedLod(Mesh, Index);
}

static TArray<AssetHiveLodQuality::FPart> DescribeLodStructure(USkeletalMesh* Sk, UStaticMesh* Sm, int32 Index)
{
    TArray<FVector3f> Positions;
    TArray<uint32> Indices;
    const FPositionVertexBuffer* Buffer = nullptr;
    if (Sk) {
        const auto* Data = Sk->GetResourceForRendering();
        if (!Data || !Data->LODRenderData.IsValidIndex(Index)) return {};
        const auto& Lod = Data->LODRenderData[Index];
        Buffer = &Lod.StaticVertexBuffers.PositionVertexBuffer;
        Lod.MultiSizeIndexContainer.GetIndexBuffer(Indices);
    } else if (Sm) {
        const auto* Data = Sm->GetRenderData();
        if (!Data || !Data->LODResources.IsValidIndex(Index)) return {};
        const auto& Lod = Data->LODResources[Index];
        Buffer = &Lod.VertexBuffers.PositionVertexBuffer;
        const auto View = Lod.IndexBuffer.GetArrayView();
        for (int32 I = 0; I < View.Num(); ++I) Indices.Add(View[I]);
    }
    if (!Buffer) return {};
    for (uint32 I = 0; I < Buffer->GetNumVertices(); ++I) Positions.Add(Buffer->VertexPosition(I));
    return AssetHiveLodQuality::Describe(Positions, Indices);
}

// Only per-mesh settings are changed. Shared LOD Settings assets stay untouched.
void UAssetHiveModelCatalogLibrary::ProtectImportedLod(UObject* Model, int32 Index)
{
    if (Index < 1) return;
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Model))
    {
        if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index))
        {
            Mesh->Modify();
            Mesh->GetOutermost()->GetMetaData().RemoveValue(Mesh, *GeneratedLodKey(Index));
            Info->ReductionSettings = FSkeletalMeshOptimizationSettings();
            Info->ReductionSettings.NumOfTrianglesPercentage = 1.f;
            Info->ReductionSettings.NumOfVertPercentage = 1.f;
            Info->ReductionSettings.BaseLOD = Index;
            Info->bHasBeenSimplified = false;
        }
    }
    else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Model))
    {
        StaticMesh->GetOutermost()->GetMetaData().RemoveValue(StaticMesh, *GeneratedLodKey(Index));
        if (Index < StaticMesh->GetNumSourceModels())
        {
            StaticMesh->Modify();
            auto& Settings = StaticMesh->GetSourceModel(Index).ReductionSettings;
            Settings = FMeshReductionSettings();
            Settings.PercentTriangles = 1.f;
            Settings.PercentVertices = 1.f;
            Settings.BaseLODModel = Index;
        }
    }
}

FString UAssetHiveModelCatalogLibrary::CompleteConfiguredLods(UObject* Model, const TArray<int32>& ManualLevels, bool bRegenerate)
{
    TArray<TSharedPtr<FJsonValue>> Automatic, Missing, Existing, Pending;
    const ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
    int32 Count = 0;
    // Rebuild after manual imports too: changing reduction settings alone does
    // not refresh UE's cached render data (including the imported LOD1).
    bool bChanged = bRegenerate;
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Model))
    {
        USkeletalMeshLODSettings* Preset = Mesh->GetLODSettings();
        int32 PlannedCount = Mesh->GetLODNum();
        FSkeletalMeshUpdateContext Context;
        Context.SkeletalMesh = Mesh;
        for (int32 Index = 1; Index <= 5; ++Index)
        {
            if (ManualLevels.Contains(Index)) continue;
            FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index);
            if (Info && !Info->bHasBeenSimplified && Mesh->HasMeshDescription(Index))
            {
                Existing.Add(MakeShared<FJsonValueNumber>(Index));
                continue; // Includes levels manually imported directly in UE.
            }
            if (!Info && (!Preset || Preset->GetNumberOfSettings() <= Index || (bRegenerate ? Mesh->GetLODNum() : PlannedCount) < Index))
            {
                Missing.Add(MakeShared<FJsonValueNumber>(Index));
                continue;
            }
            if (bRegenerate)
            {
                Mesh->Modify();
                if (!Info)
                {
                    // AddLODInfo applies engine defaults. Replace those defaults with
                    // the project's explicit per-level preset before reduction.
                    Mesh->AddLODInfo();
                    Preset->SetLODSettingsToMesh(Mesh, Index);
                }
                FLODUtilities::SimplifySkeletalMeshLOD(Context, Index, Platform, true);
                bChanged = true;
            }
            else if (!Info)
            {
                Pending.Add(MakeShared<FJsonValueNumber>(Index));
                PlannedCount = Index + 1;
                continue;
            }
            if (Mesh->GetLODNum() > Index) Automatic.Add(MakeShared<FJsonValueNumber>(Index));
            else Missing.Add(MakeShared<FJsonValueNumber>(Index));
        }
        Count = Mesh->GetLODNum();
    }
    else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Model))
    {
        const FStaticMeshLODGroup& Group = Platform->GetStaticMeshLODSettings().GetLODGroup(StaticMesh->GetLODGroup());
        int32 PlannedCount = StaticMesh->GetNumSourceModels();
        for (int32 Index = 1; Index <= 5; ++Index)
        {
            if (ManualLevels.Contains(Index)) continue;
            if (Index < StaticMesh->GetNumSourceModels() && StaticMesh->IsMeshDescriptionValid(Index))
            {
                Existing.Add(MakeShared<FJsonValueNumber>(Index));
                continue;
            }
            if (Index >= StaticMesh->GetNumSourceModels())
            {
                if (StaticMesh->GetLODGroup().IsNone() || Group.GetDefaultNumLODs() <= Index || (bRegenerate ? StaticMesh->GetNumSourceModels() : PlannedCount) < Index)
                {
                    Missing.Add(MakeShared<FJsonValueNumber>(Index));
                    continue;
                }
                if (!bRegenerate) { Pending.Add(MakeShared<FJsonValueNumber>(Index)); PlannedCount = Index + 1; continue; }
                StaticMesh->Modify();
                FStaticMeshSourceModel& Source = StaticMesh->AddSourceModel();
                Source.BuildSettings = StaticMesh->GetSourceModel(0).BuildSettings;
                Source.ReductionSettings = Group.GetDefaultSettings(Index);
                bChanged = true;
            }
            Automatic.Add(MakeShared<FJsonValueNumber>(Index));
            bChanged |= bRegenerate;
        }
        Count = StaticMesh->GetNumSourceModels();
    }
    if (bChanged)
    {
        Model->PostEditChange();
        FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
        Model->MarkPackageDirty();
    }
    const auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("automatic"), Automatic);
    Result->SetArrayField(TEXT("missing"), Missing);
    Result->SetArrayField(TEXT("existing"), Existing);
    Result->SetArrayField(TEXT("pending"), Pending);
    Result->SetNumberField(TEXT("count"), Count);
    FString Json;
    FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
    return Json;
}

FString UAssetHiveModelCatalogLibrary::CompleteCharacterLods(UObject* Model, const TArray<int32>& ManualLevels, bool bRegenerate, bool bSourceChanged)
{
    TArray<TSharedPtr<FJsonValue>> Automatic, Existing, Pending, MissingManual, Levels, Missing;
    USkeletalMesh* Sk = Cast<USkeletalMesh>(Model);
    UStaticMesh* Sm = Cast<UStaticMesh>(Model);
    if (!Sk && !Sm) return TEXT("{}");
    FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
    auto Triangles = [Sk, Sm](int32 Index) -> int32 {
        if (Sk) {
            const FSkeletalMeshRenderData* Data = Sk->GetResourceForRendering();
            return Data && Data->LODRenderData.IsValidIndex(Index) ? Data->LODRenderData[Index].GetTotalFaces() : 0;
        }
        const FStaticMeshRenderData* Data = Sm->GetRenderData();
        return Data && Data->LODResources.IsValidIndex(Index) ? Data->LODResources[Index].GetNumTriangles() : 0;
    };
    auto HasManual = [Sk, Sm](int32 Index) {
        if (Sk) {
            const FSkeletalMeshLODInfo* Info = Sk->GetLODInfo(Index);
            return Info && !IsGeneratedLod(Sk, Index) && !Info->bHasBeenSimplified && Sk->HasMeshDescription(Index)
                && !(Info->ReductionSettings.BaseLOD > 0 && Info->ReductionSettings.BaseLOD < Index);
        }
        return !IsComponentLod(Sm, Index) && Index < Sm->GetNumSourceModels() && Sm->IsMeshDescriptionValid(Index)
            && Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles >= 1.f;
    };
    for (int32 Index : ManualLevels) {
        if (!HasManual(Index) || Triangles(Index) <= 0) MissingManual.Add(MakeShared<FJsonValueNumber>(Index));
    }
    const bool bHasLod1 = HasManual(1) && Triangles(1) > 0;
    const bool bPlanLod1 = bHasLod1 || (!bRegenerate && ManualLevels.Contains(1));
    auto BaseIndex = [&](int32 Index) {
        int32 Base = 1;
        for (int32 Candidate = 2; Candidate < Index; ++Candidate)
            if (HasManual(Candidate)) Base = Candidate;
        return Base;
    };
    TMap<int32, TArray<AssetHiveLodQuality::FPart>> SourceStructures;
    TMap<int32, FString> SourceHashes;
    for (int32 Index = 1; Index <= 4; ++Index) if (HasManual(Index)) {
        SourceStructures.Add(Index, DescribeLodStructure(Sk, Sm, Index));
        const FMeshDescription* Description = Sk ? Sk->GetMeshDescription(Index) : Sm->GetMeshDescription(Index);
        if (Description) SourceHashes.Add(Index, FString::FromInt(Index) + TEXT(":") + FStaticMeshOperations::ComputeSHAHash(*Description, true).ToString());
    }
    auto SourceKey = [](int32 Index) { return FString::Printf(TEXT("AssetHive.LodSource.%d"), Index); };
    auto QualityPasses = [&](int32 Index) {
        const auto* Structure = SourceStructures.Find(BaseIndex(Index));
        return Structure && Triangles(Index) > 0 && Triangles(Index) <= Triangles(BaseIndex(Index))
            && AssetHiveLodQuality::PreservesStructure(*Structure, DescribeLodStructure(Sk, Sm, Index));
    };
    const ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
    for (int32 Index = 2; Index <= 5; ++Index) {
        if (ManualLevels.Contains(Index) || HasManual(Index)) {
            if (HasManual(Index)) Existing.Add(MakeShared<FJsonValueNumber>(Index));
            continue;
        }
        if (!bPlanLod1) { Missing.Add(MakeShared<FJsonValueNumber>(Index)); continue; }
        const int32 Base = BaseIndex(Index);
        const float Ratio = FMath::Pow(.5f, Index - Base);
        const FString SavedSource = Model->GetOutermost()->GetMetaData().GetValue(Model, *SourceKey(Index));
        const bool bSameSource = SavedSource.IsEmpty() ? Base == 1 && !IsComponentLod(Sm, Index) : SavedSource == SourceHashes.FindRef(Base);
        const bool bConfigured = bSameSource && (Sk
            ? Sk->GetLODInfo(Index) && Sk->GetLODInfo(Index)->ReductionSettings.BaseLOD == Base
                && Sk->GetLODInfo(Index)->ReductionSettings.NumOfTrianglesPercentage >= Ratio
            : Index < Sm->GetNumSourceModels() && (IsComponentLod(Sm, Index)
                || (Sm->GetSourceModel(Index).ReductionSettings.BaseLODModel == Base
                && Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles >= Ratio)));
        if (!bRegenerate) {
            (bConfigured && QualityPasses(Index) ? Automatic : Pending)
                .Add(MakeShared<FJsonValueNumber>(Index));
            continue;
        }
        if (!bSourceChanged && bConfigured && QualityPasses(Index)) continue;
        if (!bHasLod1 || MissingManual.Num()) continue;
        Model->Modify();
        if (Sk) {
            while (Sk->GetLODNum() <= Index) Sk->AddLODInfo();
            auto& Settings = Sk->GetLODInfo(Index)->ReductionSettings;
            Settings = FSkeletalMeshOptimizationSettings();
            Settings.BaseLOD = Base;
            Settings.NumOfTrianglesPercentage = Ratio;
            Settings.bLockEdges = true;
            Settings.bEnforceBoneBoundaries = true;
            Settings.bMergeCoincidentVertBones = false;
            Settings.MaxBonesPerVertex = Sk->GetResourceForRendering()->LODRenderData[Base].GetVertexBufferMaxBoneInfluences();
            // An old 100% fallback can carry a MeshDescription from an older
            // base. Rebuild its source too, otherwise PostEditChange restores it.
            FMeshDescription BaseDescription(*Sk->GetMeshDescription(Base));
            Sk->CreateMeshDescription(Index, MoveTemp(BaseDescription));
            Sk->CommitMeshDescription(Index);
            FSkeletalMeshUpdateContext Context;
            Context.SkeletalMesh = Sk;
            FLODUtilities::SimplifySkeletalMeshLOD(Context, Index, Platform, true);
            Sk->GetOutermost()->GetMetaData().SetValue(Sk, *GeneratedLodKey(Index), TEXT("2"));
        } else {
            while (Sm->GetNumSourceModels() <= Index) {
                auto& Source = Sm->AddSourceModel();
                Source.BuildSettings = Sm->GetSourceModel(Base).BuildSettings;
            }
            Sm->GetSourceModel(Index).BuildSettings = Sm->GetSourceModel(Base).BuildSettings;
            auto& Settings = Sm->GetSourceModel(Index).ReductionSettings;
            Settings = FMeshReductionSettings();
            Settings.BaseLODModel = Base;
            Settings.PercentTriangles = Ratio;
            Settings.WeldingThreshold = 0.f;
            Sm->ClearMeshDescription(Index);
            Sm->GetOutermost()->GetMetaData().RemoveValue(Sm, *GeneratedLodKey(Index));
            if (const FMeshDescription* Artist = Sm->GetMeshDescription(Base)) {
                FMeshDescription Reduced;
                if (AssetHiveStaticLodReduction::Reduce(*Artist, Ratio, Reduced)) {
                    if (Index > 2 && IsComponentLod(Sm, Index - 1)) {
                        const FMeshDescription* Previous = Sm->GetMeshDescription(Index - 1);
                        if (Previous && Previous->Triangles().Num() < Reduced.Triangles().Num()
                            && AssetHiveLodQuality::PreservesStructure(AssetHiveStaticLodReduction::Describe(*Artist), AssetHiveStaticLodReduction::Describe(*Previous))) {
                            Reduced = *Previous;
                        }
                    }
                    Sm->CreateMeshDescription(Index, MoveTemp(Reduced));
                    Sm->CommitMeshDescription(Index);
                    Settings.PercentTriangles = 1.f;
                    Settings.BaseLODModel = Index;
                    Sm->GetOutermost()->GetMetaData().SetValue(Sm, *GeneratedLodKey(Index), TEXT("2"));
                }
            }
            // Engine-calculated screen sizes adapt to the resulting mesh bounds.
            Sm->SetAutoComputeLODScreenSize(true);
        }
        Model->GetOutermost()->GetMetaData().SetValue(Model, *SourceKey(Index), *SourceHashes.FindRef(Base));
    }
    if (bRegenerate) {
        Model->PostEditChange();
        FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
        // Retry from the artist LOD1 with less reduction. Never accept a lost
        // component/chain hole merely because the target triangle count was met.
        for (int32 Attempt = 0; bHasLod1 && MissingManual.IsEmpty() && Attempt < 4; ++Attempt) {
            bool bRetry = false;
            for (int32 Index = 2; Index <= 5; ++Index) {
                if (ManualLevels.Contains(Index) || HasManual(Index) || QualityPasses(Index)) continue;
                if (IsComponentLod(Sm, Index)) {
                    // Render building may remove degenerates or split vertices.
                    // Check the final render result too; fall back to the nearest
                    // safe preceding level instead of accepting broken geometry.
                    int32 Safe = Index - 1;
                    while (Safe > BaseIndex(Index) && (!IsComponentLod(Sm, Safe) || !QualityPasses(Safe))) --Safe;
                    if (const FMeshDescription* Previous = Sm->GetMeshDescription(Safe)) {
                        Sm->CreateMeshDescription(Index, *Previous);
                        Sm->CommitMeshDescription(Index);
                        bRetry = true;
                    }
                    continue;
                }
                const float Current = Sk ? Sk->GetLODInfo(Index)->ReductionSettings.NumOfTrianglesPercentage
                    : Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles;
                if (Current >= 1.f) continue;
                const float Ratio = FMath::Min(1.f, Current * 2.f);
                bRetry = true;
                if (Sk) {
                    Sk->GetLODInfo(Index)->ReductionSettings.NumOfTrianglesPercentage = Ratio;
                    FSkeletalMeshUpdateContext Context;
                    Context.SkeletalMesh = Sk;
                    FLODUtilities::SimplifySkeletalMeshLOD(Context, Index, Platform, true);
                } else Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles = Ratio;
            }
            if (!bRetry) break;
            Model->PostEditChange();
            FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
        }
        Model->MarkPackageDirty();
    }
    const int32 Count = Sk ? Sk->GetLODNum() : Sm->GetNumLODs();
    for (int32 Index = 0; Index < Count; ++Index) {
        const auto Level = MakeShared<FJsonObject>();
        Level->SetNumberField(TEXT("index"), Index);
        Level->SetNumberField(TEXT("triangles"), Triangles(Index));
        if (Index >= 2 && !ManualLevels.Contains(Index) && !HasManual(Index)) {
            Level->SetBoolField(TEXT("structureVerified"), QualityPasses(Index));
            Level->SetNumberField(TEXT("baseLod"), BaseIndex(Index));
            Level->SetBoolField(TEXT("qualityFallback"), Triangles(Index) > FMath::CeilToInt(Triangles(BaseIndex(Index)) * FMath::Pow(.5f, Index - BaseIndex(Index))));
        }
        Level->SetStringField(TEXT("source"), ManualLevels.Contains(Index) ? TEXT("imported") : HasManual(Index) ? TEXT("existing") : TEXT("generated"));
        Levels.Add(MakeShared<FJsonValueObject>(Level));
    }
    if (bRegenerate && bHasLod1) {
        for (int32 Index = 2; Index <= 5; ++Index) {
            if (ManualLevels.Contains(Index) || HasManual(Index)) continue;
            (QualityPasses(Index) ? Automatic : Missing)
                .Add(MakeShared<FJsonValueNumber>(Index));
        }
    }
    const auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("policy"), TEXT("lod1-reduction-v2"));
    Result->SetNumberField(TEXT("qualityVersion"), 1);
    Result->SetArrayField(TEXT("automatic"), Automatic);
    Result->SetArrayField(TEXT("existing"), Existing);
    Result->SetArrayField(TEXT("pending"), Pending);
    Result->SetArrayField(TEXT("missingManual"), MissingManual);
    Result->SetArrayField(TEXT("missing"), Missing);
    Result->SetArrayField(TEXT("levels"), Levels);
    Result->SetNumberField(TEXT("count"), Count);
    FString Json;
    FJsonSerializer::Serialize(Result, TJsonWriterFactory<>::Create(&Json));
    return Json;
}
