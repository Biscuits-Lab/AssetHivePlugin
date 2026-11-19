#pragma once
#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "AssetHiveModelPolycountCatalog.generated.h"

UENUM(BlueprintType)
enum class EAssetHivePolycountLevel : uint8
{
    Pending UMETA(DisplayName="Pending"),
    UpTo100K UMETA(DisplayName="10w"),
    UpTo200K UMETA(DisplayName="20w"),
    UpTo300K UMETA(DisplayName="30w"),
    Over300K UMETA(DisplayName="Over 30w")
};

USTRUCT(BlueprintType)
struct ASSETHIVERUNTIME_API FAssetHiveModelPolycountEntry
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    TSoftObjectPtr<UObject> Model;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    EAssetHivePolycountLevel Level = EAssetHivePolycountLevel::Pending;
    // Skeletal LOD0 triangles; -1 means unavailable, never zero by fallback.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    int64 TriangleCount = -1;
    // suit (including face/hair), monster, npc, weapon, item.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    FName Family;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    FName HeroPart;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    FString SuitId;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    FString SuitName;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    FName SuitPart;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model")
    bool bSkeletal = false;
    // Stable source identity permits rename/move upserts. Not equipment metadata.
    UPROPERTY()
    FString SourceIdentity;
};

USTRUCT(BlueprintType)
struct ASSETHIVERUNTIME_API FAssetHiveSuitPolycountSummary
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    FString SuitId;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    FString SuitName;
    // One skeletal mesh per part. Static meshes are never used as fallback.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    TArray<FAssetHiveModelPolycountEntry> Parts;
    // -1 when any of the four required parts is missing or unmeasured.
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    int64 TriangleCount = -1;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    EAssetHivePolycountLevel Level = EAssetHivePolycountLevel::Pending;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Suit")
    bool bComplete = false;
};

UCLASS(BlueprintType)
class ASSETHIVERUNTIME_API UAssetHiveModelPolycountCatalog : public UDataAsset
{
    GENERATED_BODY()
public:
    virtual void PostLoad() override;
    // Upgrade old catalogs in memory. Does not delete any UE mesh resource.
    int32 RemoveStaticRecords();
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Model levels")
    TArray<FAssetHiveModelPolycountEntry> Models;
    // Derived from Models, never maintained as a second independent table.
    UFUNCTION(BlueprintPure, Category="AssetHive|Polycount")
    TArray<FAssetHiveSuitPolycountSummary> GetSuitSummaries() const;
    UFUNCTION(BlueprintPure, Category="AssetHive|Polycount")
    TArray<FAssetHiveModelPolycountEntry> GetModelsByFamily(FName Family = NAME_None) const;
    static EAssetHivePolycountLevel Classify(int64 Triangles)
    {
        if (Triangles < 0) return EAssetHivePolycountLevel::Pending;
        if (Triangles <= 100000) return EAssetHivePolycountLevel::UpTo100K;
        if (Triangles <= 200000) return EAssetHivePolycountLevel::UpTo200K;
        if (Triangles <= 300000) return EAssetHivePolycountLevel::UpTo300K;
        return EAssetHivePolycountLevel::Over300K;
    }
};
