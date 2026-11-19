#include "AssetHiveEditorToolbar.h"
#include "AssetHiveSettings.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "FoliageType.h"
#include "Framework/Application/SlateApplication.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/CoreDelegates.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/Texture2D.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#endif

#define LOCTEXT_NAMESPACE "AssetHiveEditorToolbar"

namespace {

const FName GAssetHiveToolbarOwner("AssetHiveEditorToolbar");
const FName GAssetHiveStyleSetName("AssetHiveStyle");

TSharedPtr<FSlateStyleSet> GAssetHiveStyleSet;

void RegisterAssetHiveStyle() {
  if (GAssetHiveStyleSet.IsValid()) {
    return;
  }
  TSharedPtr<IPlugin> Plugin =
      IPluginManager::Get().FindPlugin(TEXT("AssetHive"));
  if (!Plugin.IsValid()) {
    return;
  }
  const FString ResourceDir = Plugin->GetBaseDir() / TEXT("Resources");
  GAssetHiveStyleSet = MakeShared<FSlateStyleSet>(GAssetHiveStyleSetName);
  GAssetHiveStyleSet->SetContentRoot(ResourceDir);
  GAssetHiveStyleSet->SetCoreContentRoot(ResourceDir);

  const FVector2D IconSize(16.0f, 16.0f);
  const FString IconPath = ResourceDir / TEXT("Icon_V2_48.png");
  GAssetHiveStyleSet->Set("AssetHive.Icon",
                          new FSlateImageBrush(IconPath, IconSize));
  GAssetHiveStyleSet->Set("AssetHive.Icon.Small",
                          new FSlateImageBrush(IconPath, IconSize));

  FSlateStyleRegistry::RegisterSlateStyle(*GAssetHiveStyleSet.Get());
}

void UnregisterAssetHiveStyle() {
  if (GAssetHiveStyleSet.IsValid()) {
    FSlateStyleRegistry::UnRegisterSlateStyle(*GAssetHiveStyleSet.Get());
    GAssetHiveStyleSet.Reset();
  }
}

struct FUnusedAssetFolderEntry {
  FString FolderPath;
  int32 AssetCount = 0;
  TArray<FAssetData> Assets;
  bool bSelected = true;
  bool bHighlighted = false;
};

using FUnusedAssetFolderEntryPtr = TSharedPtr<FUnusedAssetFolderEntry>;

FString GetEnvironmentAssetFolder(const FString& PackagePath, const FString& EnvironmentPrefix) {
  FString Folder = PackagePath;
  const FString Leaf = FPaths::GetCleanFilename(Folder);
  if (Leaf == TEXT("Tex") || Leaf == TEXT("MI") || Leaf == TEXT("Material") ||
      Leaf == TEXT("Mesh") || Leaf == TEXT("Meshes")) {
    Folder = FPaths::GetPath(Folder);
  }
  const bool bVegetation = Folder.StartsWith(EnvironmentPrefix + TEXT("Vegetation/"));
  if (bVegetation && FPaths::GetCleanFilename(Folder) == TEXT("Op")) {
    Folder = FPaths::GetPath(Folder);
  }
  if (!Folder.StartsWith(EnvironmentPrefix)) return {};
  TArray<FString> Parts;
  Folder.Mid(EnvironmentPrefix.Len()).ParseIntoArray(Parts, TEXT("/"));
  if (Parts.Num() < (bVegetation ? 3 : 2) || Parts.Last() == TEXT("ThirdParty") ||
      Parts.Last() == TEXT("Temp")) return {};
  return Folder;
}

bool HasAssetHiveImportSource(const FAssetData& Asset) {
  FString SourceJson;
  if (!Asset.GetTagValue(UObject::SourceFileTagName(), SourceJson)) return false;
  const TOptional<FAssetImportInfo> Info = FAssetImportInfo::FromJson(MoveTemp(SourceJson));
  if (!Info.IsSet()) return false;
  return Info->SourceFiles.ContainsByPredicate([](const FAssetImportInfo::FSourceFile& Source) {
    FString Filename = Source.RelativeFilename;
    FPaths::NormalizeFilename(Filename);
    Filename = TEXT("/") + Filename;
    return Filename.Contains(TEXT("/AssetLibrary/")) || Filename.Contains(TEXT("/Temp/assethive/"));
  });
}

TArray<FUnusedAssetFolderEntryPtr> FindUnusedAssetFolders() {
  TArray<FUnusedAssetFolderEntryPtr> Result;
  FAssetRegistryModule &AssetRegistryModule =
      FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
          TEXT("AssetRegistry"));
  IAssetRegistry &AssetRegistry = AssetRegistryModule.Get();

  const FString ConfiguredRoot = UAssetHiveSettings::GetImportRootPath();
  const FString ConfiguredPrefix = ConfiguredRoot + TEXT("/");
  const FString EnvironmentRoot = UAssetHiveSettings::EnvironmentImportRootPath;
  const FString EnvironmentPrefix = EnvironmentRoot + TEXT("/");
  TArray<FString> ScanPaths{ConfiguredRoot};
  ScanPaths.AddUnique(EnvironmentRoot);
  AssetRegistry.ScanPathsSynchronous(ScanPaths, false);
  AssetRegistry.WaitForCompletion();

  FARFilter Filter;
  for (const FString& Path : ScanPaths) Filter.PackagePaths.Add(FName(*Path));
  Filter.bRecursivePaths = true;
  TArray<FAssetData> AllAssets;
  AssetRegistry.GetAssets(Filter, AllAssets);

  TMap<FString, TArray<FAssetData>> FolderToAssets;
  for (const FAssetData &Asset : AllAssets) {
    const FString PackagePath = Asset.PackagePath.ToString();
    if (PackagePath.StartsWith(EnvironmentPrefix)) {
      const FString FolderPath = GetEnvironmentAssetFolder(PackagePath, EnvironmentPrefix);
      if (!FolderPath.IsEmpty()) FolderToAssets.FindOrAdd(FolderPath).Add(Asset);
    } else if (PackagePath.StartsWith(ConfiguredPrefix)) {
      TArray<FString> Parts;
      PackagePath.Mid(ConfiguredPrefix.Len()).ParseIntoArray(Parts, TEXT("/"));
      if (Parts.Num() >= 2) FolderToAssets.FindOrAdd(ConfiguredRoot / Parts[0] / Parts[1]).Add(Asset);
    }
  }

  for (auto &Pair : FolderToAssets) {
    const FString &FolderPath = Pair.Key;
    const TArray<FAssetData> &FolderAssets = Pair.Value;
    const bool bEnvironmentFolder = FolderPath.StartsWith(EnvironmentPrefix);
    if (bEnvironmentFolder) {
      const bool bHasOtherAssets = FolderAssets.ContainsByPredicate([](const FAssetData& Asset) {
        return !Asset.IsInstanceOf(UStaticMesh::StaticClass()) && !Asset.IsInstanceOf(UTexture::StaticClass()) &&
            !Asset.IsInstanceOf(UMaterialInstanceConstant::StaticClass()) && !Asset.IsInstanceOf(UFoliageType::StaticClass());
      });
      if (bHasOtherAssets || !FolderAssets.ContainsByPredicate(HasAssetHiveImportSource)) continue;
    }
    TSet<FName> FolderPackages;
    for (const FAssetData& Asset : FolderAssets) FolderPackages.Add(Asset.PackageName);

    bool bUsedExternally = false;
    for (FName Package : FolderPackages) {
      TArray<FName> Referencers;
      AssetRegistry.GetReferencers(Package, Referencers,
                                   UE::AssetRegistry::EDependencyCategory::All);
      for (FName Referencer : Referencers) {
        if (FolderPackages.Contains(Referencer)) {
          continue;
        }
        bUsedExternally = true;
        break;
      }
      if (bUsedExternally) {
        break;
      }
    }

    if (!bUsedExternally) {
      FUnusedAssetFolderEntryPtr Entry = MakeShared<FUnusedAssetFolderEntry>();
      Entry->FolderPath = FolderPath;
      Entry->AssetCount = FolderAssets.Num();
      Entry->Assets = FolderAssets;
      Entry->bSelected = !bEnvironmentFolder;
      Result.Add(Entry);
    }
  }

  Result.Sort([](const FUnusedAssetFolderEntryPtr &A,
                 const FUnusedAssetFolderEntryPtr &B) {
    return A->FolderPath < B->FolderPath;
  });
  return Result;
}

class SUnusedAssetsDialog : public SCompoundWidget {
public:
  SLATE_BEGIN_ARGS(SUnusedAssetsDialog) {}
  SLATE_ARGUMENT(TWeakPtr<SWindow>, ParentWindow)
  SLATE_END_ARGS()

  void Construct(const FArguments &InArgs) {
    ParentWindow = InArgs._ParentWindow;

    ChildSlot
        [SNew(SVerticalBox) +
         SVerticalBox::Slot().AutoHeight().Padding(
             8)[SAssignNew(HeaderTextBlock, STextBlock)
                    .Text(FText::FromString(TEXT("")))] +
         SVerticalBox::Slot().AutoHeight().Padding(
             8,
             0)[SNew(SHorizontalBox) +
                SHorizontalBox::Slot().AutoWidth().Padding(
                    0, 0, 8, 0)[SNew(SButton)
                                    .Text(FText::FromString(TEXT("全选")))
                                    .OnClicked_Lambda([this]() {
                                      SetAllSelected(true);
                                      return FReply::Handled();
                                    })] +
                SHorizontalBox::Slot().AutoWidth().Padding(
                    0, 0, 8,
                    0)[SNew(SButton)
                           .Text(FText::FromString(TEXT("全不选")))
                           .OnClicked_Lambda([this]() {
                             SetAllSelected(false);
                             return FReply::Handled();
                           })] +
                SHorizontalBox::Slot()
                    .AutoWidth()[SNew(SButton)
                                     .Text(FText::FromString(TEXT("刷新")))
                                     .OnClicked_Lambda([this]() {
                                       RefreshList();
                                       return FReply::Handled();
                                     })]] +
         SVerticalBox::Slot().FillHeight(1.0f).Padding(
             8)[SNew(SBorder).BorderImage(FAppStyle::GetBrush(
             "ToolPanel.GroupBorder"))[SAssignNew(ScrollBox, SScrollBox)]] +
         SVerticalBox::Slot()
             .AutoHeight()
             .HAlign(HAlign_Right)
             .Padding(8)
                 [SNew(SHorizontalBox) +
                  SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
                      [SNew(SButton)
                           .Text(FText::FromString(TEXT("删除所选")))
                           .OnClicked(
                               this, &SUnusedAssetsDialog::OnDeleteClicked)] +
                  SHorizontalBox::Slot().AutoWidth()
                      [SNew(SButton)
                           .Text(FText::FromString(TEXT("关闭")))
                           .OnClicked(this,
                                      &SUnusedAssetsDialog::OnCancelClicked)]]];

    RefreshList();
  }

private:
  void RefreshList() {
    Entries = FindUnusedAssetFolders();
    SelectedEntryPtr.Reset();

    const FString HeaderText =
        Entries.Num() > 0
            ? FString::Printf(
                  TEXT("发现 %d 个未被引用的 AssetHive 资产文件夹"),
                  Entries.Num())
            : TEXT("没有发现未被引用的 AssetHive 资产文件夹");

    if (HeaderTextBlock.IsValid()) {
      HeaderTextBlock->SetText(FText::FromString(HeaderText));
    }

    ScrollBox->ClearChildren();

    for (auto &Entry : Entries) {
      const FString Label = FString::Printf(
          TEXT("%s   (%d 个文件)"), *Entry->FolderPath, Entry->AssetCount);
      ScrollBox->AddSlot()
          [SNew(SBorder)
               .HAlign(HAlign_Fill)
               .Padding(FMargin(2, 1))
               .BorderImage(TAttribute<const FSlateBrush *>::Create(
                   [Entry]() -> const FSlateBrush * {
                     return Entry->bHighlighted
                                ? FCoreStyle::Get().GetBrush("WhiteBrush")
                                : FAppStyle::GetBrush("NoBorder");
                   }))
               .BorderBackgroundColor(TAttribute<
                                      FSlateColor>::Create([Entry]()
                                                               -> FSlateColor {
                 return Entry->bHighlighted
                            ? FSlateColor(
                                  FLinearColor(0.13f, 0.47f, 0.85f, 1.0f))
                            : FSlateColor(FLinearColor::Transparent);
               }))[SNew(SHorizontalBox) +
                   SHorizontalBox::Slot().AutoWidth().Padding(
                       4, 2)[SNew(SCheckBox)
                                 .IsChecked_Lambda([Entry]() {
                                   return Entry->bSelected
                                              ? ECheckBoxState::Checked
                                              : ECheckBoxState::Unchecked;
                                 })
                                 .OnCheckStateChanged_Lambda(
                                     [Entry](ECheckBoxState NewState) {
                                       Entry->bSelected =
                                           (NewState ==
                                            ECheckBoxState::Checked);
                                     })] +
                   SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
                       [SNew(SButton)
                            .ButtonStyle(FAppStyle::Get(), "NoBorder")
                            .ContentPadding(FMargin(4, 2))
                            .ToolTipText(
                                FText::FromString(TEXT("点击选中此行")))
                            .Cursor(EMouseCursor::Hand)
                            .OnClicked_Lambda([this, Entry]() {
                              for (auto &E : Entries) {
                                E->bHighlighted = false;
                              }
                              Entry->bHighlighted = true;
                              SelectedEntryPtr = Entry;
                              FContentBrowserModule &CBM =
                                  FModuleManager::LoadModuleChecked<
                                      FContentBrowserModule>(
                                      TEXT("ContentBrowser"));
                              IContentBrowserSingleton &Browser = CBM.Get();
                              Browser.SyncBrowserToFolders({Entry->FolderPath});
                              return FReply::Handled();
                            })[SNew(STextBlock)
                                   .Text(FText::FromString(Label))
                                   .ColorAndOpacity(
                                       TAttribute<FSlateColor>::Create(
                                           [Entry]() -> FSlateColor {
                                             return Entry->bHighlighted
                                                        ? FSlateColor(
                                                              FLinearColor::
                                                                  White)
                                                        : FSlateColor::
                                                              UseForeground();
                                           }))]]]];
    }
  }

  void SetAllSelected(bool bValue) {
    for (auto &Entry : Entries) {
      Entry->bSelected = bValue;
    }
    if (ScrollBox.IsValid()) {
      ScrollBox->Invalidate(EInvalidateWidget::Paint);
    }
  }

  FReply OnDeleteClicked() {
    TArray<FAssetData> AssetsToDelete;
    for (const auto &Entry : Entries) {
      if (!Entry->bSelected) {
        continue;
      }
      AssetsToDelete.Append(Entry->Assets);
    }
    if (AssetsToDelete.Num() == 0) {
      FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT(
                                                "请先勾选要删除的资产文件夹")));
      return FReply::Handled();
    }

    TSet<FSoftObjectPath> CurrentCandidates;
    for (const auto& Entry : FindUnusedAssetFolders()) {
      for (const FAssetData& Asset : Entry->Assets) CurrentCandidates.Add(Asset.GetSoftObjectPath());
    }
    if (AssetsToDelete.ContainsByPredicate([&CurrentCandidates](const FAssetData& Asset) {
          return !CurrentCandidates.Contains(Asset.GetSoftObjectPath());
        })) {
      RefreshList();
      FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("资产引用或位置已变化，请重新勾选后再删除。")));
      return FReply::Handled();
    }
    ObjectTools::DeleteAssets(AssetsToDelete, true);

    RefreshList();
    return FReply::Handled();
  }

  FReply OnCancelClicked() {
    if (TSharedPtr<SWindow> Parent = ParentWindow.Pin()) {
      Parent->RequestDestroyWindow();
    }
    return FReply::Handled();
  }

  TArray<FUnusedAssetFolderEntryPtr> Entries;
  TSharedPtr<SScrollBox> ScrollBox;
  TSharedPtr<STextBlock> HeaderTextBlock;
  TWeakPtr<SWindow> ParentWindow;
  FUnusedAssetFolderEntryPtr SelectedEntryPtr;
};

void ShowUnusedAssetsDialog() {
  static TWeakPtr<SWindow> ExistingWindow;

  if (TSharedPtr<SWindow> PinnedWindow = ExistingWindow.Pin()) {
    PinnedWindow->BringToFront(true);
    return;
  }

  TSharedRef<SWindow> Window =
      SNew(SWindow)
          .Title(FText::FromString(TEXT("AssetHive - 清理未使用资产")))
          .ClientSize(FVector2D(720, 520))
          .SupportsMaximize(false)
          .SupportsMinimize(false);

  Window->SetContent(SNew(SUnusedAssetsDialog).ParentWindow(Window));
  FSlateApplication::Get().AddWindow(Window);
  ExistingWindow = Window;
}

void RegisterToolbarMenu() {
  static bool bRegistered = false;

  UToolMenus *ToolMenus = UToolMenus::Get();
  if (!ToolMenus) {
    return;
  }

  if (bRegistered) {
    ToolMenus->RefreshAllWidgets();
    return;
  }

  UToolMenus::UnregisterOwner(GAssetHiveToolbarOwner);

  RegisterAssetHiveStyle();

  FToolMenuOwnerScoped OwnerScope(GAssetHiveToolbarOwner);

  const FName StatusBarToolBarName(TEXT("LevelEditor.StatusBar.ToolBar"));
  UToolMenu *StatusBarMenu = ToolMenus->ExtendMenu(StatusBarToolBarName);
  if (!StatusBarMenu) {
    UE_LOG(LogTemp, Warning, TEXT("[AssetHive] Failed to extend menu %s"),
           *StatusBarToolBarName.ToString());
    return;
  }

  FToolMenuSection &Section = StatusBarMenu->FindOrAddSection(
      TEXT("AssetHiveSection"), FText::GetEmpty(),
      FToolMenuInsert(TEXT("SourceControl"), EToolMenuInsertType::Before));

  FToolMenuEntry ButtonEntry = FToolMenuEntry::InitWidget(
      TEXT("AssetHive"),
      SNew(SButton)
          .ButtonStyle(FAppStyle::Get(), "SimpleButton")
          .ContentPadding(FMargin(6.0f, 0.0f))
          .OnClicked_Lambda([]() {
            ShowUnusedAssetsDialog();
            return FReply::Handled();
          })[SNew(SHorizontalBox) +
             SHorizontalBox::Slot()
                 .AutoWidth()
                 .VAlign(VAlign_Center)
                 .Padding(0, 0, 6, 0)[SNew(SImage).Image(
                     FSlateIcon(GAssetHiveStyleSetName, "AssetHive.Icon")
                         .GetIcon())] +
             SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                 [SNew(STextBlock)
                      .Text(FText::FromString(TEXT("Asset Hive")))]],
      FText::GetEmpty());
  Section.AddEntry(ButtonEntry);

  ToolMenus->RefreshAllWidgets();

  bRegistered = true;
  UE_LOG(LogTemp, Log, TEXT("[AssetHive] Registered status-bar combo button"));
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveUnusedEnvironmentAssetsTest, "AssetHive.Editor.UnusedEnvironmentAssets",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveUnusedEnvironmentAssetsTest::RunTest(const FString&) {
  const FString Prefix = FString(UAssetHiveSettings::EnvironmentImportRootPath) / TEXT("Vegetation/Tree/ThirdParty");
  const FString RunId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
  const FString ImportedFolder = Prefix / (TEXT("Imported_") + RunId);
  const FString OtherFolder = Prefix / (TEXT("Other_") + RunId);
  TArray<UTexture2D*> Textures;
  ON_SCOPE_EXIT {
    for (UTexture2D* Texture : Textures) {
      FAssetRegistryModule::AssetDeleted(Texture);
      Texture->ClearFlags(RF_Public | RF_Standalone);
      Texture->SetFlags(RF_Transient);
      Texture->GetPackage()->SetDirtyFlag(false);
    }
  };
  auto AddTexture = [&](const FString& Folder, const TCHAR* Name, const TCHAR* Source) {
    auto* Texture = NewObject<UTexture2D>(CreatePackage(*(Folder / Name)), Name, RF_Public | RF_Standalone);
    if (Source) {
      Texture->AssetImportData = NewObject<UAssetImportData>(Texture);
      Texture->AssetImportData->UpdateFilenameOnly(Source);
    }
    FAssetRegistryModule::AssetCreated(Texture);
    Textures.Add(Texture);
  };
  AddTexture(ImportedFolder / TEXT("Tex"), TEXT("T_Masked"), TEXT("//server/share/AssetLibrary/T_Masked.png"));
  AddTexture(ImportedFolder / TEXT("Op/Tex"), TEXT("T_Op"), nullptr);
  AddTexture(OtherFolder / TEXT("Tex"), TEXT("T_Manual"), TEXT("//server/share/ManualArt/T_Manual.png"));
  const auto Entries = FindUnusedAssetFolders();
  const auto* Imported = Entries.FindByPredicate([&ImportedFolder](const FUnusedAssetFolderEntryPtr& Entry) {
    return Entry->FolderPath == ImportedFolder;
  });
  if (!TestNotNull(TEXT("AssetHive imports under the environment root are detected"), Imported)) return false;
  TestEqual(TEXT("Masked and opaque dependencies are grouped as one asset"), (*Imported)->AssetCount, 2);
  TestFalse(TEXT("Environment results require selection"), (*Imported)->bSelected);
  TestFalse(TEXT("Unrelated imports are omitted"), Entries.ContainsByPredicate([&OtherFolder](const FUnusedAssetFolderEntryPtr& Entry) {
    return Entry->FolderPath == OtherFolder;
  }));
  return true;
}
#endif

} // namespace

namespace AssetHiveEditorToolbar {
void Register() {
  if (UToolMenus::TryGet()) {
    RegisterToolbarMenu();
  } else {
    UToolMenus::RegisterStartupCallback(
        FSimpleMulticastDelegate::FDelegate::CreateStatic(
            &RegisterToolbarMenu));
  }

  FCoreDelegates::GetOnPostEngineInit().AddStatic(&RegisterToolbarMenu);
}

void Unregister() {
  if (UObjectInitialized()) {
    UToolMenus::UnregisterOwner(GAssetHiveToolbarOwner);
  }
  UnregisterAssetHiveStyle();
}
} // namespace AssetHiveEditorToolbar

#undef LOCTEXT_NAMESPACE
