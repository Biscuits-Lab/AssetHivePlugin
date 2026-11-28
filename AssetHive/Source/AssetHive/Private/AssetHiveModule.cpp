#include "AssetHiveModule.h"
#include "AssetHiveSuitImport.h"
#include "AssetHiveAnimationImport.h"
#include "AssetHiveSettings.h"
#include "AssetHiveEditorToolbar.h"
#include "AssetHiveImportCommandlet.h"
#include "AssetHiveTCPServer.h"
#include "AssetHiveThumbnailRefresh.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Dom/JsonObject.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Notifications/NotificationManager.h"
#include "UObject/StrongObjectPtr.h"
#include "PropertyEditorModule.h"
#include "AssetHiveModelCatalogDetails.h"

IMPLEMENT_MODULE(FAssetHiveModule, AssetHive)

void FAssetHiveModule::StartupModule()
{
    if (!IsRunningCommandlet())
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).RegisterCustomClassLayout(
            TEXT("AssetHiveModelPolycountCatalog"), FOnGetDetailCustomizationInstance::CreateStatic(&FAssetHiveModelCatalogDetails::MakeInstance));
    LifetimeToken = MakeShared<TAtomic<bool>>(true);
    SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    TryStartEditorIntegration();
    if (!TCPServer.IsValid() && !IsRunningCommandlet())
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FAssetHiveModule::TickConnection), 2.0f);
}

void FAssetHiveModule::TryStartEditorIntegration()
{
    if (TCPServer.IsValid() || IsRunningCommandlet() || !FPaths::IsProjectFilePathSet()) return;
    if (!bEditorIntegrationStarted)
    {
        bEditorIntegrationStarted = true;
        AssetHiveEditorToolbar::Register();
    }
    TCPServer = MakeUnique<FAssetHiveTCPServer>();
    const TWeakPtr<TAtomic<bool>> WeakLifetime = LifetimeToken;
    TCPServer->OnMessageReceived = [this, WeakLifetime](const FString& Message)
    {
        const auto Lifetime = WeakLifetime.Pin();
        if (Lifetime.IsValid() && Lifetime->Load()) OnTCPMessageReceived(Message);
    };
    if (!TCPServer->Init() || !TCPServer->Start())
    {
        TCPServer.Reset();
        return;
    }
}

bool FAssetHiveModule::TickConnection(float DeltaTime)
{
    TryStartEditorIntegration();
    return !TCPServer.IsValid(); // Stop retrying after success; no idle game-thread polling.
}

void FAssetHiveModule::ShutdownModule()
{
    if (auto* PropertyEditor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
        PropertyEditor->UnregisterCustomClassLayout(TEXT("AssetHiveModelPolycountCatalog"));
    AssetHiveThumbnailRefresh::Shutdown();
    if (LifetimeToken.IsValid()) LifetimeToken->Store(false);
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    if (bEditorIntegrationStarted) AssetHiveEditorToolbar::Unregister();
    if (TCPServer.IsValid()) { TCPServer->Shutdown(); TCPServer.Reset(); }
    if (ImportProgressHandle.IsValid())
        FSlateNotificationManager::Get().CancelProgressNotification(*ImportProgressHandle);
    LifetimeToken.Reset();
}

void FAssetHiveModule::SendJson(const TSharedRef<FJsonObject>& Json)
{
    FString Serialized;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
    if (FJsonSerializer::Serialize(Json, Writer) && TCPServer.IsValid()) TCPServer->SendMessage(Serialized);
}

void FAssetHiveModule::Reply(const FString& RequestId, const FString& Type, const FString& Message)
{
    const auto Response = MakeShared<FJsonObject>();
    Response->SetStringField(TEXT("type"), Type);
    Response->SetStringField(TEXT("requestId"), RequestId);
    Response->SetStringField(TEXT("message"), Message);
    if (Type == TEXT("error")) Response->SetBoolField(TEXT("rejectedBeforeImport"), true);
    SendJson(Response);
}

void FAssetHiveModule::OnTCPMessageReceived(const FString& Message)
{
    check(IsInGameThread());
    TSharedPtr<FJsonObject> Json;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message), Json) || !Json.IsValid()) return;
    FString Type, RequestId;
    Json->TryGetStringField(TEXT("type"), Type);
    Json->TryGetStringField(TEXT("requestId"), RequestId);
    if (Type == TEXT("hello"))
    {
        auto Info = MakeShared<FJsonObject>();
        Info->SetStringField(TEXT("type"), TEXT("hello"));
        Info->SetArrayField(TEXT("capabilities"), {MakeShared<FJsonValueString>(TEXT("zion3dMaterialNames")),
                                                 MakeShared<FJsonValueString>(TEXT("zionPlantMaterialReuse")),
                                                 MakeShared<FJsonValueString>(TEXT("plantExportV2")),
                                                 MakeShared<FJsonValueString>(TEXT("plantCantileverWeights")),
                                                 MakeShared<FJsonValueString>(TEXT("meshOnlyOverwrite")),
                                                 MakeShared<FJsonValueString>(TEXT("selectiveOverwrite")),
                                                 MakeShared<FJsonValueString>(TEXT("maskMaterials")),
                                                 MakeShared<FJsonValueString>(TEXT("naniteFoliage")),
                                                 MakeShared<FJsonValueString>(TEXT("characterSuitV1")),
                                                 MakeShared<FJsonValueString>(TEXT("characterLodsV2")),
                                                 MakeShared<FJsonValueString>(TEXT("characterLodTargets40_15_10_2")),
                                                 MakeShared<FJsonValueString>(TEXT("characterStatusV2")),
                                                 MakeShared<FJsonValueString>(TEXT("characterLinkOnlyV1")),
                                                 MakeShared<FJsonValueString>(TEXT("characterLinkOnlyV2")),
                                                 MakeShared<FJsonValueString>(TEXT("characterUpgradeV1")),
                                                 MakeShared<FJsonValueString>(TEXT("characterAnimationV1")),
                                                 MakeShared<FJsonValueString>(TEXT("characterAnimationInspectV1")),
                                                 MakeShared<FJsonValueString>(TEXT("atlasTextureOnly"))});
        Info->SetNumberField(TEXT("protocolVersion"), 2);
        Info->SetStringField(TEXT("sessionId"), SessionId);
        Info->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
        FString ProjectId;
        FGuid ProjectGuid;
        if (GConfig && GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectID"), ProjectId, GGameIni)
            && FGuid::Parse(ProjectId, ProjectGuid) && ProjectGuid.IsValid()) {
            Info->SetStringField(TEXT("projectId"), ProjectGuid.ToString(EGuidFormats::Digits));
        }
        Info->SetStringField(TEXT("editorPath"), FPlatformProcess::ExecutablePath());
        Info->SetNumberField(TEXT("pid"), FPlatformProcess::GetCurrentProcessId());
        Info->SetStringField(TEXT("importRootPath"), UAssetHiveSettings::GetImportRootPath());
        SendJson(Info);
        return;
    }
    if (RequestId.IsEmpty()) return;
    if (Type == TEXT("query") || Type == TEXT("import"))
    {
        if (const auto* Existing = RequestStates.Find(RequestId))
        {
            SendJson(Existing->ToSharedRef()); // Idempotent: return status instead of importing again.
            return;
        }
    }
    if (Type == TEXT("query"))
    {
        Reply(RequestId, TEXT("unknown"), TEXT("原任务不存在或已过期，请检查已导入资产"));
        return;
    }
    if (Type != TEXT("import")) return;
    FString ProjectPath;
    Json->TryGetStringField(TEXT("projectPath"), ProjectPath);
    FString CurrentPath = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
    ProjectPath = FPaths::ConvertRelativePathToFull(ProjectPath);
    FPaths::NormalizeFilename(ProjectPath);
    FPaths::NormalizeFilename(CurrentPath);
    if (!ProjectPath.Equals(CurrentPath, ESearchCase::IgnoreCase))
    {
        Reply(RequestId, TEXT("error"), TEXT("目标项目与当前编辑器项目不一致"));
        return;
    }
    const TSharedPtr<FJsonObject>* Job = nullptr;
    double ProtocolVersion = 0;
    Json->TryGetNumberField(TEXT("protocolVersion"), ProtocolVersion);
    if (ProtocolVersion != 2 || !Json->TryGetObjectField(TEXT("job"), Job) || !Job || !Job->IsValid())
    {
        Reply(RequestId, TEXT("error"), TEXT("请更新 AssetHive；此插件需要内存导入任务"));
        return;
    }
    if (!ActiveRequestId.IsEmpty())
    {
        Reply(RequestId, TEXT("error"), TEXT("Unreal 正在处理另一个导入任务，请稍后重试"));
        return;
    }
    ActiveRequestId = RequestId;
    auto Accepted = MakeShared<FJsonObject>();
    Accepted->SetStringField(TEXT("type"), TEXT("accepted"));
    Accepted->SetStringField(TEXT("requestId"), RequestId);
    RequestStates.Add(RequestId, Accepted);
    SendJson(Accepted);
    const TWeakPtr<TAtomic<bool>> WeakLifetime = LifetimeToken;
    // Run from the editor tick so modal confirmation and synchronous Interchange can pump their event loops.
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this, WeakLifetime, RequestId, JobData = *Job](float)
    {
        const auto Lifetime = WeakLifetime.Pin();
        if (Lifetime.IsValid() && Lifetime->Load()) RunImport(RequestId, JobData);
        return false;
    }));
}

void FAssetHiveModule::RecordProgress(const FString& RequestId, float Percent, const FString& Stage, bool bShowInEditor)
{
    auto Progress = MakeShared<FJsonObject>();
    Progress->SetStringField(TEXT("type"), TEXT("progress"));
    Progress->SetStringField(TEXT("requestId"), RequestId);
    Progress->SetNumberField(TEXT("percent"), FMath::Clamp(Percent, 0.0f, 99.0f));
    Progress->SetStringField(TEXT("stage"), Stage);
    RequestStates.Add(RequestId, Progress);
    SendJson(Progress);
    // Preparation/compositing still reports to AssetHive, but never opens or
    // retains an editor progress notification. Real import stages opt in.
    if (!bShowInEditor) {
        if (ImportProgressHandle.IsValid()) {
            FSlateNotificationManager::Get().CancelProgressNotification(*ImportProgressHandle);
            ImportProgressHandle.Reset();
        }
        return;
    }
    if (!ImportProgressHandle.IsValid()) {
        ImportProgressHandle = MakeShared<FProgressNotificationHandle>(FSlateNotificationManager::Get().StartProgressNotification(
            FText::FromString(TEXT("AssetHive 导入中")), 100));
    }
    if (ImportProgressHandle.IsValid())
        FSlateNotificationManager::Get().UpdateProgressNotification(*ImportProgressHandle,
            FMath::Clamp(FMath::RoundToInt(Percent), 0, 99), 0, FText::FromString(Stage));
}

void FAssetHiveModule::RunImport(const FString& RequestId, const TSharedPtr<FJsonObject>& Job)
{
    check(IsInGameThread());

    const FString Root = UAssetHiveSettings::GetImportRootPath();
    TArray<FString> ImportedFolders;
    auto Result = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject>* SuitJob = nullptr;
    const TSharedPtr<FJsonObject>* AnimationJob = nullptr;
    if (Job->TryGetObjectField(TEXT("characterAnimation"), AnimationJob) && AnimationJob && AnimationJob->IsValid())
    {
        const auto AnimationResult = AssetHiveAnimationImport::Run(AnimationJob->ToSharedRef(), [this, RequestId](int32 Percent, const FString& Stage)
        {
            RecordProgress(RequestId, Percent, Stage, false);
        });
        Result->SetStringField(TEXT("type"), TEXT("complete"));
        Result->SetObjectField(TEXT("animationResult"), AnimationResult);
        Result->SetStringField(TEXT("message"), TEXT("动画逐项处理完成"));
    }
    else if (Job->TryGetObjectField(TEXT("characterSuit"), SuitJob) && SuitJob && SuitJob->IsValid())
    {
        RecordProgress(RequestId, 0, TEXT("正在检查/处理套装资源"), false);
        const auto SuitResult = AssetHiveSuitImport::Run(SuitJob->ToSharedRef(), [this, RequestId](int32 Percent, const FString& Stage)
        {
            RecordProgress(RequestId, Percent, Stage, false);
        });
        Result->SetStringField(TEXT("type"), TEXT("complete"));
        Result->SetObjectField(TEXT("suitResult"), SuitResult);
        Result->SetStringField(TEXT("message"), TEXT("套装逐项处理完成"));
    }
    else
    {
        TStrongObjectPtr<UAssetHiveImportCommandlet> Importer(NewObject<UAssetHiveImportCommandlet>(GetTransientPackage()));
        TArray<TSharedPtr<FJsonValue>> AssetResults;
        const int32 ExitCode = Importer->ImportJob(Job, Root, [this, RequestId](int32 Percent, const FString& Stage, bool bShowInEditor)
        {
            RecordProgress(RequestId, Percent, Stage, bShowInEditor);
        }, &ImportedFolders, &AssetResults, [](const FString& AssetName, const FString& AssetFolder)
        {
            const FText Message = FText::Format(
                NSLOCTEXT("AssetHive", "OverwriteMessage", "「{0}」的目标目录已有资产。\n{1}\n\n是否按当前覆盖策略继续导入？\n选择「否」将跳过此资产。"),
                FText::FromString(AssetName), FText::FromString(AssetFolder));
            return FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, Message,
                NSLOCTEXT("AssetHive", "OverwriteTitle", "AssetHive · 确认覆盖")) == EAppReturnType::Yes;
        });
        Result->SetStringField(TEXT("type"), ExitCode == 0 ? TEXT("complete") : TEXT("error"));
        Result->SetArrayField(TEXT("assetResults"), AssetResults);
        Result->SetStringField(TEXT("message"), ExitCode == 0 ? TEXT("Unreal 导入完成") : TEXT("导入失败，请查看 Unreal Output Log"));
    }
    Result->SetStringField(TEXT("requestId"), RequestId);
    Result->SetStringField(TEXT("destinationPath"), Root);
    RequestStates.Add(RequestId, Result);
    CompletedRequests.Add(RequestId);
    // Bounded session-local history for reconnect queries. Never evict an active import.
    while (CompletedRequests.Num() > 256)
    {
        RequestStates.Remove(CompletedRequests[0]);
        CompletedRequests.RemoveAt(0);
    }
    ActiveRequestId.Empty();
    SendJson(Result);
    if (ImportProgressHandle.IsValid())
        FSlateNotificationManager::Get().CancelProgressNotification(*ImportProgressHandle);
    ImportProgressHandle.Reset();
    if (!ImportedFolders.IsEmpty() && !IsRunningCommandlet())
    {
        FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        ContentBrowser.Get().SyncBrowserToFolders(ImportedFolders, false, true);
    }
}
