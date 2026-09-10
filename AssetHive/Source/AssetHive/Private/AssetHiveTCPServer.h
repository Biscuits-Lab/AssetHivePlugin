#pragma once
#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Containers/Queue.h"

class FSocket;
class FAssetHiveTCPServer : public FRunnable
{
public:
    virtual ~FAssetHiveTCPServer();
    virtual bool Init() override;
    virtual uint32 Run() override;
    virtual void Stop() override;
    bool Start();
    void Shutdown();
    void SendMessage(const FString& Message);
    TFunction<void(const FString&)> OnMessageReceived;
private:
    void ProcessMessage(const FString& Message);
    FSocket* ListenerSocket = nullptr;
    FRunnableThread* Thread = nullptr;
    TAtomic<bool> bRunThread { false };
    TQueue<TArray<uint8>, EQueueMode::Mpsc> Outgoing;
};
