#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace AssetHiveSuitImport
{
    TSharedRef<FJsonObject> Run(const TSharedRef<FJsonObject>& Job, TFunction<void(int32, const FString&)> Progress);
}
