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

// Only per-mesh settings are changed. Shared LOD Settings assets stay untouched.
void UAssetHiveModelCatalogLibrary::ProtectImportedLod(UObject* Model, int32 Index)
{
    if (Index < 1) return;
    if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Model))
    {
        if (FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(Index))
        {
            Mesh->Modify();
            Info->ReductionSettings = FSkeletalMeshOptimizationSettings();
            Info->ReductionSettings.NumOfTrianglesPercentage = 1.f;
            Info->ReductionSettings.NumOfVertPercentage = 1.f;
            Info->ReductionSettings.BaseLOD = Index;
            Info->bHasBeenSimplified = false;
        }
    }
    else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Model))
    {
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

FString UAssetHiveModelCatalogLibrary::CompleteCharacterLods(UObject* Model, const TArray<int32>& ManualLevels, bool bRegenerate)
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
            return Info && !Info->bHasBeenSimplified && Sk->HasMeshDescription(Index);
        }
        return Index < Sm->GetNumSourceModels() && Sm->IsMeshDescriptionValid(Index)
            && Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles >= 1.f;
    };
    for (int32 Index : ManualLevels) {
        if (!HasManual(Index) || Triangles(Index) <= 0) MissingManual.Add(MakeShared<FJsonValueNumber>(Index));
    }
    const bool bHasLod1 = HasManual(1) && Triangles(1) > 0;
    const bool bPlanLod1 = bHasLod1 || (!bRegenerate && ManualLevels.Contains(1));
    // QuadricMeshReduction keeps at least 64 triangles (or the original count).
    // Small static meshes can therefore legitimately plateau across LOD levels.
    auto TriangleLimit = [&](int32 Index) {
        return FMath::Max(Sm ? FMath::Min(64, Triangles(1)) : 1,
            FMath::CeilToInt(Triangles(1) * FMath::Pow(.5f, Index - 1)));
    };
    const ITargetPlatform* Platform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
    for (int32 Index = 2; Index <= 5; ++Index) {
        if (ManualLevels.Contains(Index) || HasManual(Index)) {
            if (HasManual(Index)) Existing.Add(MakeShared<FJsonValueNumber>(Index));
            continue;
        }
        if (!bPlanLod1) { Missing.Add(MakeShared<FJsonValueNumber>(Index)); continue; }
        const float Ratio = FMath::Pow(.5f, Index - 1);
        const bool bConfigured = Sk
            ? Sk->GetLODInfo(Index) && Sk->GetLODInfo(Index)->ReductionSettings.BaseLOD == 1
                && FMath::IsNearlyEqual(Sk->GetLODInfo(Index)->ReductionSettings.NumOfTrianglesPercentage, Ratio)
            : Index < Sm->GetNumSourceModels() && Sm->GetSourceModel(Index).ReductionSettings.BaseLODModel == 1
                && FMath::IsNearlyEqual(Sm->GetSourceModel(Index).ReductionSettings.PercentTriangles, Ratio);
        if (!bRegenerate) {
            (bConfigured && Triangles(Index) > 0 && Triangles(Index) <= TriangleLimit(Index) ? Automatic : Pending)
                .Add(MakeShared<FJsonValueNumber>(Index));
            continue;
        }
        if (!bHasLod1 || MissingManual.Num()) continue;
        Model->Modify();
        if (Sk) {
            while (Sk->GetLODNum() <= Index) Sk->AddLODInfo();
            auto& Settings = Sk->GetLODInfo(Index)->ReductionSettings;
            Settings = FSkeletalMeshOptimizationSettings();
            Settings.BaseLOD = 1;
            Settings.NumOfTrianglesPercentage = Ratio;
            FSkeletalMeshUpdateContext Context;
            Context.SkeletalMesh = Sk;
            FLODUtilities::SimplifySkeletalMeshLOD(Context, Index, Platform, true);
        } else {
            while (Sm->GetNumSourceModels() <= Index) {
                auto& Source = Sm->AddSourceModel();
                Source.BuildSettings = Sm->GetSourceModel(1).BuildSettings;
            }
            auto& Settings = Sm->GetSourceModel(Index).ReductionSettings;
            Settings = FMeshReductionSettings();
            Settings.BaseLODModel = 1;
            Settings.PercentTriangles = Ratio;
            // Engine-calculated screen sizes adapt to the resulting mesh bounds.
            Sm->SetAutoComputeLODScreenSize(true);
        }
    }
    if (bRegenerate) {
        Model->PostEditChange();
        FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
        Model->MarkPackageDirty();
    }
    const int32 Count = Sk ? Sk->GetLODNum() : Sm->GetNumLODs();
    for (int32 Index = 0; Index < Count; ++Index) {
        const auto Level = MakeShared<FJsonObject>();
        Level->SetNumberField(TEXT("index"), Index);
        Level->SetNumberField(TEXT("triangles"), Triangles(Index));
        Level->SetStringField(TEXT("source"), ManualLevels.Contains(Index) ? TEXT("imported") : HasManual(Index) ? TEXT("existing") : TEXT("generated"));
        Levels.Add(MakeShared<FJsonValueObject>(Level));
    }
    if (bRegenerate && bHasLod1) {
        for (int32 Index = 2; Index <= 5; ++Index) {
            if (ManualLevels.Contains(Index) || HasManual(Index)) continue;
            (Triangles(Index) > 0 && Triangles(Index) <= TriangleLimit(Index) ? Automatic : Missing)
                .Add(MakeShared<FJsonValueNumber>(Index));
        }
    }
    const auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("policy"), TEXT("lod1-reduction-v2"));
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
