#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Containers/Ticker.h"

struct FProgressNotificationHandle;
class FAssetHiveTCPServer;
class FJsonObject;

class FAssetHiveModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
private:
    bool TickConnection(float DeltaTime);
    void TryStartEditorIntegration();
    void OnTCPMessageReceived(const FString& Message);
    void SendJson(const TSharedRef<FJsonObject>& Json);
    void Reply(const FString& RequestId, const FString& Type, const FString& Message);
    void RunImport(const FString& RequestId, const TSharedPtr<FJsonObject>& Job);
    void RecordProgress(const FString& RequestId, float Percent, const FString& Stage);
    TUniquePtr<FAssetHiveTCPServer> TCPServer;
    TSharedPtr<TAtomic<bool>> LifetimeToken;
    bool bEditorIntegrationStarted = false;
    FString SessionId;
    FString ActiveRequestId;
    TMap<FString, TSharedPtr<FJsonObject>> RequestStates;
    TArray<FString> CompletedRequests;
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<FProgressNotificationHandle> ImportProgressHandle;
};
