#include "AssetHiveModelCatalogLibrary.h"
#include "AssetHiveModelPolycountCatalog.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "StaticMeshResources.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/PackageName.h"
#include "AssetCompilingManager.h"
#include "Animation/Skeleton.h"
#include "Rendering/NaniteResources.h"
#include "UObject/MetaData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "HAL/FileManager.h"

bool UAssetHiveModelCatalogLibrary::AssignExistingSkeleton(USkeletalMesh* Model, USkeleton* Skeleton)
{
    if (!Model || !Skeleton) return false;
    const FReferenceSkeleton& Source = Model->GetRefSkeleton();
    const FReferenceSkeleton& Target = Skeleton->GetReferenceSkeleton();
    for (int32 Index = 0; Index < Source.GetNum(); ++Index)
    {
        const int32 TargetIndex = Target.FindBoneIndex(Source.GetBoneName(Index));
        if (TargetIndex == INDEX_NONE) return false;
        const int32 Parent = Source.GetParentIndex(Index);
        const int32 TargetParent = Target.GetParentIndex(TargetIndex);
        if ((Parent == INDEX_NONE) != (TargetParent == INDEX_NONE)) return false;
        if (Parent != INDEX_NONE && Source.GetBoneName(Parent) != Target.GetBoneName(TargetParent)) return false;
    }
    Model->Modify();
    Model->SetSkeleton(Skeleton);
    Model->PostEditChange();
    Model->MarkPackageDirty();
    return true;
}

USkeleton* UAssetHiveModelCatalogLibrary::CreateSkeletonForMesh(USkeletalMesh* Model, const FString& PackagePath)
{
    if (!Model || !PackagePath.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(PackagePath)) return nullptr;
    const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
    if (LoadObject<UObject>(nullptr, *(PackagePath + TEXT(".") + AssetName))) return nullptr;
    auto* Skeleton = NewObject<USkeleton>(CreatePackage(*PackagePath), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
    if (!Skeleton->MergeAllBonesToBoneTree(Model, false))
    {
        Skeleton->ClearFlags(RF_Public | RF_Standalone);
        return nullptr;
    }
    FAssetRegistryModule::AssetCreated(Skeleton);
    Skeleton->MarkPackageDirty();
    return Skeleton;
}

bool UAssetHiveModelCatalogLibrary::UpdateModelLevel(UObject* Model, const FString& SourceIdentity, const FString& InSuitId, const FString& InSuitName, FName SuitPart, bool bRefreshSharedCatalog)
{
    if (!Model || !Model->IsA<USkeletalMesh>() || SourceIdentity.IsEmpty()) return false;
    // Imports already save identity/config in the mesh receipt and geometry in
    // the mesh itself. Never lock or mutate the shared snapshot during import.
    if (!bRefreshSharedCatalog) return true;
    FString SuitId = InSuitId, SuitName = InSuitName;
    FName ModelFamily, HeroPart;
    const FString ModelReceiptText = Model->GetOutermost()->GetMetaData().GetValue(Model, TEXT("AssetHive.SuitReceipt"));
    TSharedPtr<FJsonObject> ModelReceipt;
    if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ModelReceiptText), ModelReceipt) && ModelReceipt.IsValid())
    {
        const TSharedPtr<FJsonObject>* Config = nullptr;
        if (ModelReceipt->TryGetObjectField(TEXT("config"), Config))
        {
            FString FamilyValue, PartValue;
            (*Config)->TryGetStringField(TEXT("family"), FamilyValue);
            (*Config)->TryGetStringField(TEXT("heroPart"), PartValue);
            ModelFamily = FName(*FamilyValue.ToLower());
            HeroPart = FName(*PartValue.ToLower());
            if (ModelFamily == TEXT("face") || ModelFamily == TEXT("hair")) { HeroPart = ModelFamily; ModelFamily = TEXT("suit"); }
        }
    }
    if (!HeroPart.IsNone()) { SuitId.Reset(); SuitName.Reset(); SuitPart = NAME_None; }
    // Backfill old rows from the import receipt, not from an artist's filename guess.
    if (SuitId.IsEmpty() && HeroPart.IsNone())
    {
        const FString Saved = Model->GetOutermost()->GetMetaData().GetValue(Model, TEXT("AssetHive.SuitReceipt"));
        TSharedPtr<FJsonObject> Receipt;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved), Receipt) && Receipt.IsValid())
        {
            const TSharedPtr<FJsonObject>* Config = nullptr;
            FString Family;
            if (Receipt->TryGetObjectField(TEXT("config"), Config) && (*Config)->TryGetStringField(TEXT("family"), Family) && Family == TEXT("suit"))
            {
                FString Prefix, Part;
                if (SourceIdentity.Split(TEXT(":"), &Prefix, &Part, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
                {
                    Part.ToLowerInline();
                    if (Part == TEXT("head") || Part == TEXT("upbody") || Part == TEXT("downbody") || Part == TEXT("arm"))
                    {
                        SourceIdentity.Split(TEXT(":"), &SuitId, nullptr);
                        SuitPart = FName(*Part);
                        FString Target, Root;
                        Receipt->TryGetStringField(TEXT("targetPath"), Target);
                        if (Target.Split(TEXT("/Mesh/"), &Root, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
                            SuitName = FPackageName::GetLongPackageAssetName(Root);
                        if (SuitName.IsEmpty()) SuitName = SuitId;
                    }
                }
            }
        }
    }
    // Finish the imported mesh before reading its final LOD0 render geometry.
    FAssetCompilingManager::Get().FinishCompilationForObjects({Model});
    int64 Triangles = -1;
    if (const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Model))
    {
        const auto* Data = SkeletalMesh->GetResourceForRendering();
        if (Data && Data->LODRenderData.Num()) Triangles = Data->LODRenderData[0].GetTotalFaces();
    }
    else return false;
    const FString PackageName(TEXT("/Game/Characters/Data/DA_ModelPolycountLevels"));
    const FString AssetName(TEXT("DA_ModelPolycountLevels"));
    UObject* Existing = LoadObject<UObject>(nullptr, *(PackageName + TEXT(".") + AssetName));
    auto* Catalog = Cast<UAssetHiveModelPolycountCatalog>(Existing);
    if (Existing && !Catalog) return false; // Never replace an unrelated same-name asset.
    if (!Catalog)
    {
        UPackage* Package = CreatePackage(*PackageName);
        Catalog = NewObject<UAssetHiveModelPolycountCatalog>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
        FAssetRegistryModule::AssetCreated(Catalog);
    }
    const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
    // The catalog is shared and may be read-only after a Perforce sync.
    // A failed auxiliary save must never terminate the editor/import job.
    if (IFileManager::Get().IsReadOnly(*Filename))
    {
        UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
        if (!Assets || !Assets->CheckoutLoadedAsset(Catalog) || IFileManager::Get().IsReadOnly(*Filename))
        {
            UE_LOG(LogTemp, Warning, TEXT("AssetHive: model catalog checkout failed; statistics remain pending: %s"), *Filename);
            return false;
        }
    }
    const auto Level = UAssetHiveModelPolycountCatalog::Classify(Triangles);
    if (Catalog->RemoveStaticRecords() > 0) Catalog->MarkPackageDirty();
    auto* Row = Catalog->Models.FindByPredicate([&](const FAssetHiveModelPolycountEntry& Entry) {
        return Entry.SourceIdentity == SourceIdentity || Entry.Model.ToSoftObjectPath() == FSoftObjectPath(Model);
    });
    if (Row && Row->Family == ModelFamily && Row->HeroPart == HeroPart && Row->bSkeletal == Model->IsA<USkeletalMesh>() && Row->TriangleCount == Triangles && Row->Level == Level && Row->Model.ToSoftObjectPath() == FSoftObjectPath(Model)
        && Row->SourceIdentity == SourceIdentity && (SuitId.IsEmpty() || (Row->SuitId == SuitId && Row->SuitName == SuitName && Row->SuitPart == SuitPart))
        && !Catalog->GetOutermost()->IsDirty()) return Triangles >= 0;
    Catalog->Modify();
    if (!Row) Row = &Catalog->Models.AddDefaulted_GetRef();
    Row->Model = Model;
    Row->Level = Level;
    Row->TriangleCount = Triangles;
    Row->Family = ModelFamily;
    Row->HeroPart = HeroPart;
    Row->bSkeletal = Model->IsA<USkeletalMesh>();
    if (!SuitId.IsEmpty()) { Row->SuitId = SuitId; Row->SuitName = SuitName; Row->SuitPart = SuitPart; }
    if (!HeroPart.IsNone()) { Row->SuitId.Reset(); Row->SuitName.Reset(); Row->SuitPart = NAME_None; }
    Row->SourceIdentity = SourceIdentity;
    Catalog->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    const bool Saved = UPackage::SavePackage(Catalog->GetOutermost(), Catalog,
        *Filename, Args);
    return Saved && Triangles >= 0;
}
