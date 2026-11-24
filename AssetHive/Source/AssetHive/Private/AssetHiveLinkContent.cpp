#include "AssetHiveModelCatalogLibrary.h"
#include "GroomAsset.h"
#include "GroomAssetImportData.h"
#include "GroomImportOptions.h"
#include "HairDescription.h"
#include "HairStrandsEditor.h"
#include "HairStrandsTranslator.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/NameAsStringProxyArchive.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "AssetImportTask.h"
#include "EditorReimportHandler.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/ReimportFbxStaticMeshFactory.h"
#include "Factories/ReimportFbxSkeletalMeshFactory.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxStaticMeshImportData.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "AssetCompilingManager.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

// The FBX coordinate system, triangulation and build settings must be applied
// before comparing with UE data. Parse into transient objects ONLY, without
// AssetTools, saving packages, creating materials, or touching shared skeletons.
UObject* UAssetHiveModelCatalogLibrary::ParseLinkMeshSource(UObject* ReferenceModel, const FString& SourcePath)
{
    UStaticMesh* Static = Cast<UStaticMesh>(ReferenceModel);
    USkeletalMesh* Skeletal = Cast<USkeletalMesh>(ReferenceModel);
    if ((!Static && !Skeletal) || !FPaths::FileExists(SourcePath)) return nullptr;
    TStrongObjectPtr<UFbxFactory> Factory(NewObject<UFbxFactory>());
    auto* Task = NewObject<UAssetImportTask>(Factory.Get());
    auto* Options = NewObject<UFbxImportUI>(Factory.Get());
    Task->bAutomated = true;
    Task->Filename = SourcePath;
    Task->Options = Options;
    Factory->AssetImportTask = Task;
    Factory->SetDetectImportTypeOnImport(false);
    Options->bAutomatedImportShouldDetectType = false;
    Options->bImportAsSkeletal = Skeletal != nullptr;
    Options->MeshTypeToImport = Skeletal ? FBXIT_SkeletalMesh : FBXIT_StaticMesh;
    Options->bImportMesh = true;
    Options->bImportMaterials = false;
    Options->bImportTextures = false;
    Options->bImportAnimations = false;
    Options->bCreatePhysicsAsset = false;
    if (Static)
    {
        if (auto* Data = Cast<UFbxStaticMeshImportData>(Static->GetAssetImportData()))
            Options->StaticMeshImportData = DuplicateObject<UFbxStaticMeshImportData>(Data, Options);
        Options->StaticMeshImportData->bCombineMeshes = true;
        Options->StaticMeshImportData->bImportMeshLODs = true;
        Options->StaticMeshImportData->bAutoGenerateCollision = false;
    }
    else
    {
        if (auto* Data = Cast<UFbxSkeletalMeshImportData>(Skeletal->GetAssetImportData()))
            Options->SkeletalMeshImportData = DuplicateObject<UFbxSkeletalMeshImportData>(Data, Options);
        Options->SkeletalMeshImportData->bImportMorphTargets = true;
        Options->SkeletalMeshImportData->bUpdateSkeletonReferencePose = false;
        Options->SkeletalMeshImportData->bUseT0AsRefPose = false;
        // A private copy prevents the importer adding bones to the real rig.
        if (Skeletal->GetSkeleton())
            Options->Skeleton = DuplicateObject<USkeleton>(Skeletal->GetSkeleton(), GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), USkeleton::StaticClass()));
    }
    bool bCancelled = false;
    // FBX's static importer creates a sibling package rather than using its
    // supplied outer. Confine those in-memory packages to a unique /Temp root.
    const FString TempRoot = TEXT("/Temp/AssetHiveLink_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    UPackage* Outer = CreatePackage(*(TempRoot + TEXT("/Source")));
    Outer->SetFlags(RF_Transient);
    UE_LOG(LogTemp, Display, TEXT("AssetHive link parse begin: %s"), *SourcePath);
    UObject* Result = UFactory::StaticImportObject(ReferenceModel->GetClass(), Outer,
        TEXT("AssetHiveLinkSource"),
        RF_Public | RF_Standalone, bCancelled, *SourcePath, nullptr, Factory.Get(), nullptr, GWarn);
    UE_LOG(LogTemp, Display, TEXT("AssetHive link parse complete"));
    Factory->CleanUp();
    if (bCancelled || !Result || !Result->GetOutermost()->GetName().StartsWith(TempRoot + TEXT("/"))) return nullptr;
    UObject* ParsedObjects[] = { Result };
    FAssetCompilingManager::Get().FinishCompilationForObjects(ParsedObjects);
    UE_LOG(LogTemp, Display, TEXT("AssetHive link parse compilation complete"));
    Result->SetFlags(RF_Transient);
    Result->ClearFlags(RF_Public | RF_Standalone);
    Result->GetOutermost()->SetFlags(RF_Transient);
    Result->GetOutermost()->SetDirtyFlag(false);
    Outer->SetDirtyFlag(false);
    return Result;
}

void UAssetHiveModelCatalogLibrary::ReleaseLinkMeshSource(UObject* Model)
{
    if (!Model || !Model->GetOutermost()->GetName().StartsWith(TEXT("/Temp/AssetHiveLink_"))) return;
    UObject* ParsedObjects[] = { Model };
    FAssetCompilingManager::Get().FinishCompilationForObjects(ParsedObjects);
    FAssetRegistryModule::AssetDeleted(Model);
    Model->ClearFlags(RF_Public | RF_Standalone);
    Model->SetFlags(RF_Transient);
    Model->GetOutermost()->SetFlags(RF_Transient);
    Model->GetOutermost()->SetDirtyFlag(false);
    Model->MarkAsGarbage();
}

bool UAssetHiveModelCatalogLibrary::ReimportLinkedModel(UObject* Model, const FString& SourcePath, UObject* ImportOptions)
{
    if (!Model || !FPaths::FileExists(SourcePath) || !ImportOptions) return false;
    UFactory* Factory = nullptr;
    FReimportHandler* Handler = nullptr;
    if (Cast<UStaticMesh>(Model))
    {
        auto* Typed = NewObject<UReimportFbxStaticMeshFactory>();
        Factory = Typed;
        Handler = Typed;
    }
    else if (Cast<USkeletalMesh>(Model))
    {
        auto* Typed = NewObject<UReimportFbxSkeletalMeshFactory>();
        Factory = Typed;
        Handler = Typed;
    }
    if (!Factory) return false;
    TStrongObjectPtr<UFactory> KeepAlive(Factory);
    UAssetImportTask* Task = NewObject<UAssetImportTask>(Factory);
    Task->bAutomated = true;
    Task->Filename = SourcePath;
    Task->Options = ImportOptions;
    Factory->AssetImportTask = Task;
    // Avoid UFbxFactory's fallback ValidateAllSourceFileAndReimport, which can
    // open a desktop picker even when the original import task was automated.
    return FReimportManager::Instance()->Reimport(Model, false, false, SourcePath, Handler, INDEX_NONE, false, true, false);
}

FString UAssetHiveModelCatalogLibrary::GetLinkSkeletonBones(UObject* Object)
{
    const USkeleton* Skeleton = Cast<USkeleton>(Object);
    TArray<TSharedPtr<FJsonValue>> Bones;
    if (Skeleton)
    {
        const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
        for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
        {
            auto Bone = MakeShared<FJsonObject>();
            Bone->SetStringField(TEXT("name"), Ref.GetBoneName(Index).ToString());
            const int32 Parent = Ref.GetParentIndex(Index);
            Bone->SetStringField(TEXT("parent"), Parent == INDEX_NONE ? FString() : Ref.GetBoneName(Parent).ToString());
            Bones.Add(MakeShared<FJsonValueObject>(Bone));
        }
    }
    FString Result;
    FJsonSerializer::Serialize(Bones, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

// Translate into memory only. No import task, package creation or asset save.
UObject* UAssetHiveModelCatalogLibrary::CopyGroomImportOptions(UObject* Model)
{
    const UGroomAsset* Groom = Cast<UGroomAsset>(Model);
    const UGroomAssetImportData* Data = Groom ? Cast<UGroomAssetImportData>(Groom->AssetImportData) : nullptr;
    return Data && Data->ImportOptions ? DuplicateObject<UGroomImportOptions>(Data->ImportOptions, GetTransientPackage()) : nullptr;
}

bool UAssetHiveModelCatalogLibrary::VerifyGroomSource(UObject* Model, const FString& SourcePath)
{
    UGroomAsset* Groom = Cast<UGroomAsset>(Model);
    if (!Groom) return false;
    const UGroomAssetImportData* ImportData = Cast<UGroomAssetImportData>(Groom->AssetImportData);
    if (!ImportData || !ImportData->ImportOptions) return false;
    FModuleManager::Get().LoadModule(TEXT("AlembicHairTranslatorModule"));
    FHairDescription Source;
    bool bTranslated = false;
    for (const auto& Translator : FGroomEditor::Get().GetHairTranslators())
    {
        if (Translator->CanTranslate(SourcePath))
        {
            bTranslated = Translator->Translate(SourcePath, Source, ImportData->ImportOptions->ConversionSettings);
            break;
        }
    }
    if (!bTranslated || !Source.IsValid()) return false;
    FHairDescription Existing = Groom->GetHairDescription();
    if (!Existing.IsValid()) return false;
    auto Serialize = [](FHairDescription& Description)
    {
        TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes, true);
        FNameAsStringProxyArchive Archive(Writer);
        Description.Serialize(Archive);
        return Bytes;
    };
    return Serialize(Source) == Serialize(Existing);
}
