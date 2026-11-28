#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "ZionAssetHiveFoliageLibrary.generated.h"

class USkeletalMesh;

UCLASS()
class ASSETHIVE_API UZionAssetHiveFoliageLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	// 无文件对话框的风数据导入；返回空串表示成功。
	UFUNCTION(BlueprintCallable, Category = "AssetHive|Nanite Foliage")
	static FString ApplyWindData(USkeletalMesh* Mesh, const FString& Filename, bool bExpectAssembly);

	UFUNCTION(BlueprintPure, Category = "AssetHive|Nanite Foliage")
	static bool IsAssembly(USkeletalMesh* Mesh);
};
