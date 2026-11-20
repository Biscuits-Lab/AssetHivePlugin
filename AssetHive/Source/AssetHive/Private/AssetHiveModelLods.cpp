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
