#include "AssetHiveModelPolycountCatalog.h"

TArray<FAssetHiveModelPolycountEntry> UAssetHiveModelPolycountCatalog::GetModelsByFamily(FName Family) const
{
    return Models.FilterByPredicate([&](const auto& Row) {
        return Row.bSkeletal && (Family.IsNone() || Row.Family == Family);
    });
}

int32 UAssetHiveModelPolycountCatalog::RemoveStaticRecords()
{
    // Stable receipt kind also upgrades catalogs written before bSkeletal existed.
    for (auto& Row : Models)
    {
        if (Row.SourceIdentity.Contains(TEXT(":skeletalMesh:"))) Row.bSkeletal = true;
        if (Row.SourceIdentity.Contains(TEXT(":staticMesh:"))) Row.bSkeletal = false;
    }
    return Models.RemoveAll([](const auto& Row) { return !Row.bSkeletal; });
}

void UAssetHiveModelPolycountCatalog::PostLoad()
{
    Super::PostLoad();
    if (RemoveStaticRecords() > 0)
    {
#if WITH_EDITOR
        MarkPackageDirty();
#endif
    }
}

TArray<FAssetHiveSuitPolycountSummary> UAssetHiveModelPolycountCatalog::GetSuitSummaries() const
{
    TArray<FAssetHiveSuitPolycountSummary> Result;
    for (const auto& Row : Models)
    {
        if (!Row.bSkeletal || !Row.HeroPart.IsNone() || Row.SuitId.IsEmpty() || !(Row.SuitPart == TEXT("head") || Row.SuitPart == TEXT("upbody") || Row.SuitPart == TEXT("downbody") || Row.SuitPart == TEXT("arm"))) continue;
        auto* Summary = Result.FindByPredicate([&](const auto& Item) { return Item.SuitId == Row.SuitId; });
        if (!Summary) { Summary = &Result.AddDefaulted_GetRef(); Summary->SuitId = Row.SuitId; }
        Summary->SuitName = Row.SuitName;
        auto* Part = Summary->Parts.FindByPredicate([&](const auto& Item) { return Item.SuitPart == Row.SuitPart; });
        if (!Part) Summary->Parts.Add(Row);
        else if (Row.Model.ToString() < Part->Model.ToString()) *Part = Row;
    }
    for (auto& Summary : Result)
    {
        Summary.bComplete = Summary.Parts.Num() == 4;
        int64 Total = 0;
        for (const auto& Part : Summary.Parts)
        {
            if (Part.TriangleCount < 0) Summary.bComplete = false;
            else Total += Part.TriangleCount;
        }
        Summary.TriangleCount = Summary.bComplete ? Total : -1;
        Summary.Level = Classify(Summary.TriangleCount);
        Summary.Parts.Sort([](const auto& A, const auto& B) { return A.SuitPart.LexicalLess(B.SuitPart); });
    }
    Result.Sort([](const auto& A, const auto& B) { return A.SuitName < B.SuitName; });
    return Result;
}
