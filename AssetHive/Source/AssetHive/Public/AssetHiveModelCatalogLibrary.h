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
    static bool AssignExistingSkeleton(class USkeletalMesh* Model, class USkeleton* Skeleton);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static class USkeleton* CreateSkeletonForMesh(class USkeletalMesh* Model, const FString& PackagePath);

    UFUNCTION(BlueprintCallable, Category="AssetHive")
    static bool UpdateModelLevel(UObject* Model, const FString& SourceIdentity, const FString& SuitId = TEXT(""), const FString& SuitName = TEXT(""), FName SuitPart = NAME_None);
};
