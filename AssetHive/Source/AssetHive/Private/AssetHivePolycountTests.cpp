#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AssetHiveModelPolycountCatalog.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHivePolycountTest, "AssetHive.Character.PolycountTotals", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHivePolycountTest::RunTest(const FString& Parameters)
{
    auto* Catalog = NewObject<UAssetHiveModelPolycountCatalog>();
    auto Add = [&](const TCHAR* Part, int64 Count, bool Skeletal, const TCHAR* Owner = TEXT("suit1")) {
        auto& Row = Catalog->Models.AddDefaulted_GetRef();
        Row.SuitId = Owner; Row.SuitName = Owner; Row.SuitPart = FName(Part); Row.bSkeletal = Skeletal;
        Row.TriangleCount = Count; Row.Level = UAssetHiveModelPolycountCatalog::Classify(Count);
        Row.Model = FSoftObjectPath(FString::Printf(TEXT("/Game/Test/%s_%s.%s_%s"), Skeletal ? TEXT("SK") : TEXT("SM"), Part, Skeletal ? TEXT("SK") : TEXT("SM"), Part));
    };
    Add(TEXT("head"), 25000, false);
    Add(TEXT("head"), 50000, true);
    Add(TEXT("upbody"), 100000, true);
    Add(TEXT("downbody"), 100000, true);
    auto Summaries = Catalog->GetSuitSummaries();
    TestFalse(TEXT("Missing arm is incomplete"), Summaries[0].bComplete);
    TestEqual(TEXT("Incomplete is unknown, not zero"), Summaries[0].TriangleCount, int64(-1));
    Add(TEXT("arm"), 50000, true);
    Add(TEXT("weapon"), 900000, true);
    Add(TEXT("hair"), 900000, true);
    Summaries = Catalog->GetSuitSummaries();
    TestEqual(TEXT("SM/SK and non-suit parts not double counted"), Summaries[0].TriangleCount, int64(300000));
    TestEqual(TEXT("Exactly four selected parts"), Summaries[0].Parts.Num(), 4);
    TestTrue(TEXT("Four known parts complete"), Summaries[0].bComplete);
    TestEqual(TEXT("300000 is 30w"), Summaries[0].Level, EAssetHivePolycountLevel::UpTo300K);
    Catalog->Models[4].TriangleCount = 50001;
    TestEqual(TEXT("300001 is over"), Catalog->GetSuitSummaries()[0].Level, EAssetHivePolycountLevel::Over300K);
    Catalog->Models[4].TriangleCount = -1;
    TestFalse(TEXT("Unknown part incomplete"), Catalog->GetSuitSummaries()[0].bComplete);
    TestEqual(TEXT("Unknown not low tier"), Catalog->GetSuitSummaries()[0].Level, EAssetHivePolycountLevel::Pending);
    Add(TEXT("head"), 12, false, TEXT("suit2"));
    TestEqual(TEXT("Static-only suit excluded"), Catalog->GetSuitSummaries().Num(), 1);
    Catalog->Models[4].bSkeletal = false;
    Catalog->Models[4].TriangleCount = 50000;
    TestFalse(TEXT("Static arm cannot fill missing skeletal arm"), Catalog->GetSuitSummaries()[0].bComplete);
    Catalog->Models[4].bSkeletal = true;
    Add(TEXT("head"), 12, true, TEXT("suit2"));
    TestEqual(TEXT("Separate suit identities"), Catalog->GetSuitSummaries().Num(), 2);
    TestEqual(TEXT("Old static records removed"), Catalog->RemoveStaticRecords(), 2);
    TestTrue(TEXT("Every remaining row skeletal"), !Catalog->Models.ContainsByPredicate([](const auto& Row) { return !Row.bSkeletal; }));
    TestEqual(TEXT("100000 boundary"), UAssetHiveModelPolycountCatalog::Classify(100000), EAssetHivePolycountLevel::UpTo100K);
    TestEqual(TEXT("100001 boundary"), UAssetHiveModelPolycountCatalog::Classify(100001), EAssetHivePolycountLevel::UpTo200K);
    TestEqual(TEXT("200000 boundary"), UAssetHiveModelPolycountCatalog::Classify(200000), EAssetHivePolycountLevel::UpTo200K);
    TestEqual(TEXT("200001 boundary"), UAssetHiveModelPolycountCatalog::Classify(200001), EAssetHivePolycountLevel::UpTo300K);
    auto* Families = NewObject<UAssetHiveModelPolycountCatalog>();
    for (const FName Family : {FName(TEXT("suit")), FName(TEXT("suit")), FName(TEXT("monster")), FName(TEXT("npc")), FName(TEXT("weapon")), FName(TEXT("item"))})
    {
        auto& Row = Families->Models.AddDefaulted_GetRef(); Row.Family = Family; Row.bSkeletal = true;
    }
    Families->Models[0].HeroPart = TEXT("face");
    Families->Models[1].HeroPart = TEXT("hair");
    Families->Models[0].SuitId = TEXT("not_a_four_piece_suit");
    Families->Models[0].SuitPart = TEXT("head");
    TestEqual(TEXT("All character families visible"), Families->GetModelsByFamily().Num(), 6);
    TestEqual(TEXT("Face and hair under suits"), Families->GetModelsByFamily(TEXT("suit")).Num(), 2);
    TestEqual(TEXT("NPC kept separate"), Families->GetModelsByFamily(TEXT("npc")).Num(), 1);
    TestEqual(TEXT("Face not added to four-piece total"), Families->GetSuitSummaries().Num(), 0);
    Families->Models[0].bSkeletal = false;
    TestEqual(TEXT("Family filter never includes static"), Families->GetModelsByFamily(TEXT("suit")).Num(), 1);
    return true;
}
#endif
