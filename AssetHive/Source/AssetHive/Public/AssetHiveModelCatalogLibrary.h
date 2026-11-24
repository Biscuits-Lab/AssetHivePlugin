#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "AssetHiveModelCatalogLibrary.generated.h"

UCLASS()
class ASSETHIVE_API UAssetHiveModelCatalogLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static bool VerifyGroomSource(UObject* Model, const FString& SourcePath);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static UObject* CopyGroomImportOptions(UObject* Model);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static FString GetLinkSkeletonBones(UObject* Skeleton);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static bool ReimportLinkedModel(UObject* Model, const FString& SourcePath, UObject* ImportOptions);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static UObject* ParseLinkMeshSource(UObject* ReferenceModel, const FString& SourcePath);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static void ReleaseLinkMeshSource(UObject* Model);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static void ProtectImportedLod(UObject* Model, int32 Index);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static FString CompleteConfiguredLods(UObject* Model, const TArray<int32>& ManualLevels, bool bRegenerate);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static FString CompleteCharacterLods(UObject* Model, const TArray<int32>& ManualLevels, bool bRegenerate);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static bool AssignExistingSkeleton(class USkeletalMesh* Model, class USkeleton* Skeleton);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static class USkeleton* CreateSkeletonForMesh(class USkeletalMesh* Model, const FString& PackagePath);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static bool UpdateModelLevel(UObject* Model, const FString& SourceIdentity, const FString& SuitId = TEXT(""), const FString& SuitName = TEXT(""), FName SuitPart = NAME_None);
};
