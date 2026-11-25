#include "AssetHiveModelCatalogDetails.h"
#include "AssetHiveModelPolycountCatalog.h"
#include "AssetHiveModelCatalogLibrary.h"
#include "DetailLayoutBuilder.h"
#include "DetailCategoryBuilder.h"
#include "DetailWidgetRow.h"
#include "IDetailGroup.h"
#include "IPropertyUtilities.h"
#include "AssetThumbnail.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Misc/ScopedSlowTask.h"
#include "Styling/AppStyle.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "AssetHiveModelCatalog"
namespace { bool ShowSuitTotals = false; FName SelectedFamily; }

TSharedRef<IDetailCustomization> FAssetHiveModelCatalogDetails::MakeInstance()
{
    return MakeShared<FAssetHiveModelCatalogDetails>();
}

void FAssetHiveModelCatalogDetails::CustomizeDetails(IDetailLayoutBuilder& Builder)
{
    TArray<TWeakObjectPtr<UObject>> Objects;
    Builder.GetObjectsBeingCustomized(Objects);
    if (Objects.Num() != 1) return;
    const TWeakObjectPtr<UAssetHiveModelPolycountCatalog> Catalog = Cast<UAssetHiveModelPolycountCatalog>(Objects[0].Get());
    if (!Catalog.IsValid()) return;
    Catalog->RemoveStaticRecords();
    Builder.HideProperty(GET_MEMBER_NAME_CHECKED(UAssetHiveModelPolycountCatalog, Models));
    auto& Category = Builder.EditCategory(TEXT("PolycountOverview"), LOCTEXT("Overview", "骨骼模型面数"), ECategoryPriority::Important);
    const TWeakPtr<IPropertyUtilities> Utilities = Builder.GetPropertyUtilities();
    Category.AddCustomRow(LOCTEXT("Summary", "统计 刷新"), false).WholeRowContent()
    [
        SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
        [SNew(STextBlock).Text(FText::Format(LOCTEXT("Total", "{0} 个骨骼模型  ·  {1} 套套装"), FText::AsNumber(Catalog->Models.Num()), FText::AsNumber(Catalog->GetSuitSummaries().Num())))]
        + SHorizontalBox::Slot().AutoWidth().Padding(12, 0)
        [SNew(SButton).Text(LOCTEXT("Refresh", "刷新面数"))
            .ToolTipText(LOCTEXT("RefreshTip", "汇总已同步的骨骼模型并更新此表，需要签出和提交此表。日常模型导入不会修改此表。"))
            .OnClicked_Lambda([Catalog, Utilities]() {
                if (Catalog.IsValid())
                {
                    auto Rows = Catalog->Models;
                    // Discover imported meshes added by colleagues since the last snapshot.
                    TArray<FAssetData> Assets;
                    FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get()
                        .GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Assets, true);
                    for (const FAssetData& Asset : Assets)
                    {
                        if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game/"))) continue;
                        if (Rows.ContainsByPredicate([&](const auto& Row) { return Row.Model.ToSoftObjectPath() == Asset.GetSoftObjectPath(); })) continue;
                        UObject* Model = Asset.GetAsset();
                        if (!Model) continue;
                        const FString Text = Model->GetOutermost()->GetMetaData().GetValue(Model, TEXT("AssetHive.SuitReceipt"));
                        TSharedPtr<FJsonObject> Receipt;
                        FString Key;
                        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Receipt) || !Receipt.IsValid()
                            || !Receipt->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty()) continue;
                        auto& Row = Rows.AddDefaulted_GetRef();
                        Row.Model = Model;
                        Row.SourceIdentity = Key;
                    }
                    FScopedSlowTask Task(Rows.Num(), LOCTEXT("Refreshing", "正在读取模型面数…"));
                    Task.MakeDialog(true);
                    for (const auto& Row : Rows)
                    {
                        if (Task.ShouldCancel()) break;
                        Task.EnterProgressFrame(1);
                        if (UObject* Model = Row.Model.LoadSynchronous())
                            UAssetHiveModelCatalogLibrary::UpdateModelLevel(Model, Row.SourceIdentity, Row.SuitId, Row.SuitName, Row.SuitPart, true);
                    }
                }
                if (auto Pinned = Utilities.Pin()) Pinned->RequestForceRefresh();
                return FReply::Handled();
            })]
    ];
    Category.AddCustomRow(LOCTEXT("Views", "单个模型 整套套装"), false).WholeRowContent()
    [SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 8, 8, 8)
        [SNew(SButton).Text(LOCTEXT("ModelsView", "单个模型")).IsEnabled(ShowSuitTotals)
            .OnClicked_Lambda([Utilities]() { ShowSuitTotals = false; if (auto Pinned = Utilities.Pin()) Pinned->RequestForceRefresh(); return FReply::Handled(); })]
        + SHorizontalBox::Slot().AutoWidth().Padding(0, 8)
        [SNew(SButton).Text(LOCTEXT("SuitsView", "整套套装")).IsEnabled(!ShowSuitTotals)
            .OnClicked_Lambda([Utilities]() { ShowSuitTotals = true; if (auto Pinned = Utilities.Pin()) Pinned->RequestForceRefresh(); return FReply::Handled(); })]
    ];

    if (ShowSuitTotals)
    {
    auto& SuitsCategory = Category;
    const auto Suits = Catalog->GetSuitSummaries();
    SuitsCategory.AddCustomRow(LOCTEXT("SuitInfo", "套装 合计"), false).WholeRowContent()
        [SNew(STextBlock).Text(LOCTEXT("SuitRule", "四个骨骼部位合计，上限30万面；静态模型不参与。展开查看部位。" )).AutoWrapText(true)];
    for (const auto& Suit : Suits)
    {
        const FText Status = Suit.bComplete
            ? FText::Format(LOCTEXT("SuitTotal", "{0} 三角面 · {1}"), FText::AsNumber(Suit.TriangleCount),
                Suit.TriangleCount > 300000 ? LOCTEXT("SuitOver", "超出30w") : Suit.TriangleCount > 200000 ? LOCTEXT("Suit30", "30w档") : Suit.TriangleCount > 100000 ? LOCTEXT("Suit20", "20w档") : LOCTEXT("Suit10", "10w档"))
            : LOCTEXT("Incomplete", "统计不完整（缺部位或面数未知）");
        auto& Group = SuitsCategory.AddGroup(FName(*Suit.SuitId), FText::Format(LOCTEXT("SuitTitle", "{0} · {1}/4 部位 · {2}"), FText::FromString(Suit.SuitName), FText::AsNumber(Suit.Parts.Num()), Status), false, false);
        for (const auto& Part : Suit.Parts)
        {
            const FSoftObjectPath Path = Part.Model.ToSoftObjectPath();
            const FAssetData Data = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetByObjectPath(Path);
            const auto Thumbnail = MakeShared<FAssetThumbnail>(Data, 48, 48, Builder.GetThumbnailPool());
            Group.AddWidgetRow().FilterString(FText::FromString(Path.GetAssetName())).WholeRowContent()
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(4)[SNew(SBox).WidthOverride(48).HeightOverride(48)[Thumbnail->MakeThumbnailWidget()]]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0)
                [SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").ToolTipText(FText::FromString(Path.ToString()))
                    .OnClicked_Lambda([Path]() { if (UObject* Model = Path.TryLoad()) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Model); return FReply::Handled(); })
                    [SNew(STextBlock).Text(FText::FromString(Part.SuitPart.ToString() + TEXT(" · ") + Path.GetAssetName())).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0)
                [SNew(STextBlock).Text(Part.TriangleCount >= 0 ? FText::Format(LOCTEXT("PartCount", "{0} 三角面"), FText::AsNumber(Part.TriangleCount)) : LOCTEXT("PartUnknown", "面数未知"))]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0)
                [SNew(SButton).Text(LOCTEXT("Browse", "定位")).IsEnabled(Data.IsValid())
                    .OnClicked_Lambda([Data]() { FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets({Data}); return FReply::Handled(); })]
            ];
        }
    }
    return;
    }

    struct FGroup { EAssetHivePolycountLevel Level; const TCHAR* Id; FText Label; };
    const TPair<FName, FText> Families[] = {
        {NAME_None, LOCTEXT("AllFamilies", "全部")},
        {TEXT("suit"), LOCTEXT("SuitFamily", "套装（含脸部、头发）")},
        {TEXT("monster"), LOCTEXT("MonsterFamily", "怪物")},
        {TEXT("npc"), LOCTEXT("NPCFamily", "NPC")},
        {TEXT("weapon"), LOCTEXT("WeaponFamily", "武器")},
        {TEXT("item"), LOCTEXT("ItemFamily", "玩法物件")}
    };
    const auto FamilyBar = SNew(SHorizontalBox);
    for (const auto& Family : Families)
    {
        const FName Key = Family.Key;
        const int32 Count = Catalog->GetModelsByFamily(Key).Num();
        FamilyBar->AddSlot().AutoWidth().Padding(0, 4, 8, 8)
        [SNew(SButton).Text(FText::Format(LOCTEXT("FamilyCount", "{0} ({1})"), Family.Value, FText::AsNumber(Count)))
            .IsEnabled(SelectedFamily != Key)
            .OnClicked_Lambda([Key, Utilities]() { SelectedFamily = Key; if (auto Pinned = Utilities.Pin()) Pinned->RequestForceRefresh(); return FReply::Handled(); })];
    }
    Category.AddCustomRow(LOCTEXT("Families", "套装 脸部 头发 怪物 NPC 武器 玩法物件"), false).WholeRowContent()[FamilyBar];
    const auto VisibleModels = Catalog->GetModelsByFamily(SelectedFamily);
    const FGroup Groups[] = {
        {EAssetHivePolycountLevel::UpTo300K, TEXT("30w"), LOCTEXT("30w", "30w 档（20万 ＜ 面数 ≤ 30万）")},
        {EAssetHivePolycountLevel::UpTo200K, TEXT("20w"), LOCTEXT("20w", "20w 档（10万 ＜ 面数 ≤ 20万）")},
        {EAssetHivePolycountLevel::UpTo100K, TEXT("10w"), LOCTEXT("10w", "10w 档（面数 ≤ 10万）")},
        {EAssetHivePolycountLevel::Over300K, TEXT("Over"), LOCTEXT("Over", "超出 30w（面数 ＞ 30万）")}
    };
    auto& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
    for (const auto& Definition : Groups)
    {
        TArray<FAssetHiveModelPolycountEntry> Rows = VisibleModels.FilterByPredicate([&](const auto& Row) { return Row.Level == Definition.Level; });
        Rows.Sort([](const auto& A, const auto& B) { return A.Model.ToSoftObjectPath().GetAssetName() < B.Model.ToSoftObjectPath().GetAssetName(); });
        auto& Group = Category.AddGroup(FName(Definition.Id), FText::Format(LOCTEXT("GroupCount", "{0} · {1} 个模型"), Definition.Label, FText::AsNumber(Rows.Num())), false, Rows.Num() > 0 && Rows.Num() <= 8);
        if (Rows.IsEmpty())
        {
            continue;
        }
        for (const auto& Row : Rows)
        {
            const FSoftObjectPath Path = Row.Model.ToSoftObjectPath();
            const FAssetData Data = Registry.GetAssetByObjectPath(Path);
            const auto Thumbnail = MakeShared<FAssetThumbnail>(Data, 48, 48, Builder.GetThumbnailPool());
            const FString Prefix = Row.HeroPart == TEXT("face") ? TEXT("脸部 · ") : Row.HeroPart == TEXT("hair") ? TEXT("头发 · ") : TEXT("");
            const FText Name = FText::FromString(Prefix + Path.GetAssetName());
            Group.AddWidgetRow().FilterString(Name).WholeRowContent()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(4)
                [SNew(SBox).WidthOverride(48).HeightOverride(48)[Thumbnail->MakeThumbnailWidget()]]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8, 0)
                [SNew(SButton).ButtonStyle(FAppStyle::Get(), "SimpleButton").ToolTipText(FText::FromString(Path.ToString()))
                    .OnClicked_Lambda([Path]() { if (UObject* Model = Path.TryLoad()) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Model); return FReply::Handled(); })
                    [SNew(STextBlock).Text(Name).OverflowPolicy(ETextOverflowPolicy::Ellipsis)]]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0)
                [SNew(SBox).WidthOverride(150)[SNew(STextBlock).Justification(ETextJustify::Right)
                    .Text(Row.TriangleCount >= 0 ? FText::Format(LOCTEXT("Triangles", "{0} 三角面"), FText::AsNumber(Row.TriangleCount)) : LOCTEXT("Unknown", "待补齐 · 点击刷新面数"))]]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0)
                [SNew(SButton).Text(LOCTEXT("Browse", "定位")).IsEnabled(Data.IsValid())
                    .OnClicked_Lambda([Data]() { FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets({Data}); return FReply::Handled(); })]
            ];
        }
    }
    // Errors are inline notices, never a fifth tier or a false 10w classification.
    for (const auto& Row : VisibleModels)
        if (Row.TriangleCount < 0)
            Category.AddCustomRow(FText::FromString(Row.Model.ToSoftObjectPath().GetAssetName())).WholeRowContent()
            [SNew(STextBlock).Text(FText::Format(LOCTEXT("ReadError", "{0}：面数读取失败，请检查模型后刷新。"), FText::FromString(Row.Model.ToSoftObjectPath().GetAssetName()))).AutoWrapText(true)];
}
#undef LOCTEXT_NAMESPACE
