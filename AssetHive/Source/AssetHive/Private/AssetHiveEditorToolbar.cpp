#include "AssetHiveEditorToolbar.h"
#include "AssetHiveSettings.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "Interfaces/IPluginManager.h"
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

#define LOCTEXT_NAMESPACE "AssetHiveEditorToolbar"

namespace {

const FName GAssetHiveToolbarOwner("AssetHiveEditorToolbar");
const FName GAssetHiveStyleSetName("AssetHiveStyle");
FString GetAssetHiveContentRoot() { return UAssetHiveSettings::GetImportRootPath(); }

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

TArray<FUnusedAssetFolderEntryPtr> FindUnusedAssetFolders() {
  TArray<FUnusedAssetFolderEntryPtr> Result;
  FAssetRegistryModule &AssetRegistryModule =
      FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
          TEXT("AssetRegistry"));
  IAssetRegistry &AssetRegistry = AssetRegistryModule.Get();

  TArray<FString> ScanPaths;
  ScanPaths.Add(GetAssetHiveContentRoot());
  AssetRegistry.ScanPathsSynchronous(ScanPaths, false);

  FARFilter Filter;
  Filter.PackagePaths.Add(FName(*GetAssetHiveContentRoot()));
  Filter.bRecursivePaths = true;
  TArray<FAssetData> AllAssets;
  AssetRegistry.GetAssets(Filter, AllAssets);

  TMap<FString, TArray<FAssetData>> FolderToAssets;
  for (const FAssetData &Asset : AllAssets) {
    const FString PackagePath = Asset.PackagePath.ToString();
    if (!PackagePath.StartsWith(GetAssetHiveContentRoot())) {
      continue;
    }
    FString Remainder = PackagePath.Mid(GetAssetHiveContentRoot().Len());
    if (Remainder.StartsWith(TEXT("/"))) {
      Remainder = Remainder.Mid(1);
    }
    TArray<FString> Parts;
    Remainder.ParseIntoArray(Parts, TEXT("/"), true);
    if (Parts.Num() < 2) {
      continue;
    }
    const FString FolderPath = FString::Printf(
        TEXT("%s/%s/%s"), *GetAssetHiveContentRoot(), *Parts[0], *Parts[1]);
    FolderToAssets.FindOrAdd(FolderPath).Add(Asset);
  }

  for (auto &Pair : FolderToAssets) {
    const FString &FolderPath = Pair.Key;
    const TArray<FAssetData> &FolderAssets = Pair.Value;
    const FString FolderPrefix = FolderPath + TEXT("/");

    bool bUsedExternally = false;
    for (const FAssetData &Asset : FolderAssets) {
      TArray<FName> Referencers;
      AssetRegistry.GetReferencers(Asset.PackageName, Referencers,
                                   UE::AssetRegistry::EDependencyCategory::All);
      for (const FName &Referencer : Referencers) {
        const FString RefStr = Referencer.ToString();
        if (RefStr == FolderPath || RefStr.StartsWith(FolderPrefix)) {
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
                  TEXT("在 /Game/AssetHive 下发现 %d 个未被引用的资产文件夹"),
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
    TArray<FString> FoldersToRemove;
    for (const auto &Entry : Entries) {
      if (!Entry->bSelected) {
        continue;
      }
      AssetsToDelete.Append(Entry->Assets);
      FoldersToRemove.Add(Entry->FolderPath);
    }
    if (AssetsToDelete.Num() == 0) {
      FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT(
                                                "请先勾选要删除的资产文件夹")));
      return FReply::Handled();
    }

    const FString ConfirmText = FString::Printf(
        TEXT("将删除 %d 个资产文件夹，共 %d 个文件。是否继续？"),
        FoldersToRemove.Num(), AssetsToDelete.Num());
    const EAppReturnType::Type Confirm = FMessageDialog::Open(
        EAppMsgType::YesNo, FText::FromString(ConfirmText));
    if (Confirm != EAppReturnType::Yes) {
      return FReply::Handled();
    }

    ObjectTools::DeleteAssets(AssetsToDelete, false);

    FContentBrowserModule &CBM =
        FModuleManager::LoadModuleChecked<FContentBrowserModule>(
            TEXT("ContentBrowser"));
    IContentBrowserSingleton &Browser = CBM.Get();
    for (const FString &FolderPath : FoldersToRemove) {
      FString RelPath = FolderPath.Mid(FString(TEXT("/Game/")).Len());
      const FString DiskPath = FPaths::ConvertRelativePathToFull(
          FPaths::Combine(FPaths::ProjectContentDir(), RelPath));
      IFileManager::Get().DeleteDirectory(*DiskPath, false, true);
    }
    (void)Browser;

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

  FCoreDelegates::OnPostEngineInit.AddStatic(&RegisterToolbarMenu);
}

void Unregister() {
  if (UObjectInitialized()) {
    UToolMenus::UnregisterOwner(GAssetHiveToolbarOwner);
  }
  UnregisterAssetHiveStyle();
}
} // namespace AssetHiveEditorToolbar

#undef LOCTEXT_NAMESPACE
