#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

namespace AssetHiveAnimationImport
{
    TSharedRef<FJsonObject> Run(const TSharedRef<FJsonObject>& Job, TFunction<void(int32, const FString&)> Progress);
}
