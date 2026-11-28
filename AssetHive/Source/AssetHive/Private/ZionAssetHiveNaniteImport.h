#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace ZionAssetHiveNaniteImport
{
	TSharedRef<FJsonObject> Run(const TSharedRef<FJsonObject>& Asset, const FString& Folder);
}
