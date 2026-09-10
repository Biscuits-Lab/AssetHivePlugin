#pragma once

#include "Commandlets/Commandlet.h"
#include "AssetHiveImportCommandlet.generated.h"

class FJsonObject;

UCLASS()
class ASSETHIVE_API UAssetHiveImportCommandlet : public UCommandlet
{
    GENERATED_BODY()

public:
    UAssetHiveImportCommandlet();
    virtual int32 Main(const FString& Params) override;
    int32 ImportJob(const TSharedPtr<FJsonObject>& Job, const FString& DestinationPath,
        TFunction<void(int32, const FString&)> OnProgress);
};
