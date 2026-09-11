#include "AssetHiveModule.h"
#include "AssetHiveSettings.h"
#include "AssetHiveEditorToolbar.h"
#include "AssetHiveImportCommandlet.h"
#include "AssetHiveTCPServer.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Framework/Notifications/NotificationManager.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_MODULE(FAssetHiveModule, AssetHive)

void FAssetHiveModule::StartupModule()
{
    LifetimeToken = MakeShared<TAtomic<bool>>(true);
    SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
    TryStartEditorIntegration();
    if (!bEditorIntegrationStarted && !IsRunningCommandlet())
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FAssetHiveModule::TickConnection), 2.0f);
}

void FAssetHiveModule::TryStartEditorIntegration()
{
    if (bEditorIntegrationStarted || IsRunningCommandlet() || !FPaths::IsProjectFilePathSet()) return;
    bEditorIntegrationStarted = true;
    AssetHiveEditorToolbar::Register();
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
    return !bEditorIntegrationStarted; // No recurring game-thread work after startup.
}

void FAssetHiveModule::ShutdownModule()
{
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
        Info->SetNumberField(TEXT("protocolVersion"), 2);
        Info->SetStringField(TEXT("sessionId"), SessionId);
        Info->SetStringField(TEXT("projectPath"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
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
    AsyncTask(ENamedThreads::GameThread, [this, WeakLifetime, RequestId, JobData = *Job]()
    {
        const auto Lifetime = WeakLifetime.Pin();
        if (Lifetime.IsValid() && Lifetime->Load()) RunImport(RequestId, JobData);
    });
}

void FAssetHiveModule::RecordProgress(const FString& RequestId, float Percent, const FString& Stage)
{
    auto Progress = MakeShared<FJsonObject>();
    Progress->SetStringField(TEXT("type"), TEXT("progress"));
    Progress->SetStringField(TEXT("requestId"), RequestId);
    Progress->SetNumberField(TEXT("percent"), FMath::Clamp(Percent, 0.0f, 99.0f));
    Progress->SetStringField(TEXT("stage"), Stage);
    RequestStates.Add(RequestId, Progress);
    SendJson(Progress);
    if (ImportProgressHandle.IsValid())
        FSlateNotificationManager::Get().UpdateProgressNotification(*ImportProgressHandle,
            FMath::Clamp(FMath::RoundToInt(Percent), 0, 99), 0, FText::FromString(Stage));
}

void FAssetHiveModule::RunImport(const FString& RequestId, const TSharedPtr<FJsonObject>& Job)
{
    check(IsInGameThread());
    ImportProgressHandle = MakeShared<FProgressNotificationHandle>(FSlateNotificationManager::Get().StartProgressNotification(
        FText::FromString(TEXT("AssetHive 导入中")), 100));
    TStrongObjectPtr<UAssetHiveImportCommandlet> Importer(NewObject<UAssetHiveImportCommandlet>(GetTransientPackage()));
    // Read the project setting for each NEW import. Desktop payloads cannot override it.
    const FString Root = UAssetHiveSettings::GetImportRootPath();
    TArray<FString> ImportedFolders;
    const int32 ExitCode = Importer->ImportJob(Job, Root, [this, RequestId](int32 Percent, const FString& Stage)
    {
        RecordProgress(RequestId, Percent, Stage);
    }, &ImportedFolders);
    auto Result = MakeShared<FJsonObject>();
    Result->SetStringField(TEXT("type"), ExitCode == 0 ? TEXT("complete") : TEXT("error"));
    Result->SetStringField(TEXT("requestId"), RequestId);
    Result->SetStringField(TEXT("destinationPath"), Root);
    Result->SetStringField(TEXT("message"), ExitCode == 0 ? TEXT("Unreal 导入完成") : TEXT("导入失败，请查看 Unreal Output Log"));
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
    FSlateNotificationManager::Get().CancelProgressNotification(*ImportProgressHandle);
    ImportProgressHandle.Reset();
    if (ExitCode == 0 && !ImportedFolders.IsEmpty() && !IsRunningCommandlet())
    {
        FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
        ContentBrowser.Get().SyncBrowserToFolders(ImportedFolders, false, true);
    }
}
