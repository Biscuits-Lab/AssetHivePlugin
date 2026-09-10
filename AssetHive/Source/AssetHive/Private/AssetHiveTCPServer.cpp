#include "AssetHiveTCPServer.h"
#include "Async/Async.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace { constexpr int32 MaxMessageBytes = 32 * 1024 * 1024; }
FAssetHiveTCPServer::~FAssetHiveTCPServer() { Shutdown(); }

bool FAssetHiveTCPServer::Init()
{
    // FRunnableThread::Create invokes Init too; retain the listener created by the module.
    if (ListenerSocket) return true;
    auto* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    if (!Subsystem) return false;
    auto Address = Subsystem->CreateInternetAddr();
    bool bValid = false;
    Address->SetIp(TEXT("127.0.0.1"), bValid);
    Address->SetPort(13430);
    ListenerSocket = Subsystem->CreateSocket(NAME_Stream, TEXT("AssetHiveListener"), false);
    if (!ListenerSocket) return false;
    ListenerSocket->SetNonBlocking(true);
    if (!bValid || !ListenerSocket->Bind(*Address) || !ListenerSocket->Listen(8))
    {
        Subsystem->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
        UE_LOG(LogTemp, Warning, TEXT("AssetHive: cannot listen on 127.0.0.1:13430 (another editor may own it)."));
        return false;
    }
    UE_LOG(LogTemp, Log, TEXT("AssetHive: listening on 127.0.0.1:13430 (protocol 2)"));
    return true;
}

bool FAssetHiveTCPServer::Start()
{
    if (Thread || !ListenerSocket) return Thread != nullptr;
    bRunThread = true;
    Thread = FRunnableThread::Create(this, TEXT("AssetHiveTCP"), 0, TPri_BelowNormal);
    return Thread != nullptr;
}

void FAssetHiveTCPServer::Stop() { bRunThread = false; }
void FAssetHiveTCPServer::Shutdown()
{
    Stop();
    if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
    if (ListenerSocket)
    {
        ListenerSocket->Close();
        ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(ListenerSocket);
        ListenerSocket = nullptr;
    }
}

void FAssetHiveTCPServer::SendMessage(const FString& Message)
{
    // Game thread only enqueues. All socket I/O, including partial sends, is on the worker.
    FTCHARToUTF8 Utf8(*Message);
    TArray<uint8> Bytes;
    Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    Bytes.Add('\n');
    Outgoing.Enqueue(MoveTemp(Bytes));
}

void FAssetHiveTCPServer::ProcessMessage(const FString& Message)
{
    TSharedPtr<FJsonObject> Json;
    FString Type;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message), Json) || !Json.IsValid()) return;
    Json->TryGetStringField(TEXT("type"), Type);
    if (Type == TEXT("ping"))
    {
        SendMessage(TEXT("{\"type\":\"pong\"}"));
        return; // Heartbeats never schedule game-thread work or access files/UObjects.
    }
    if (OnMessageReceived)
    {
        const auto Callback = OnMessageReceived;
        AsyncTask(ENamedThreads::GameThread, [Callback, Message]() { Callback(Message); });
    }
}

uint32 FAssetHiveTCPServer::Run()
{
    auto* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
    while (bRunThread)
    {
        bool bPending = false;
        if (!ListenerSocket->HasPendingConnection(bPending) || !bPending)
        {
            FPlatformProcess::Sleep(0.02f);
            continue;
        }
        FSocket* Client = ListenerSocket->Accept(TEXT("AssetHiveClient"));
        if (!Client) continue;
        Client->SetNonBlocking(true);
        Client->SetNoDelay(true);
        TArray<uint8> Buffer, SendBuffer;
        int32 SendOffset = 0;
        int32 ScanOffset = 0;
        bool bAlive = true;
        double LastReceive = FPlatformTime::Seconds();
        while (bRunThread && bAlive)
        {
            uint8 Chunk[65536];
            int32 Read = 0;
            // UE reports would-block as success with zero bytes; a closed stream returns false.
            if (!Client->Recv(Chunk, sizeof(Chunk), Read)) break;
            if (Read > 0)
            {
                LastReceive = FPlatformTime::Seconds();
                if (Buffer.Num() + Read > MaxMessageBytes) break;
                Buffer.Append(Chunk, Read);
                int32 LineStart = 0;
                for (int32 Index = ScanOffset; Index < Buffer.Num(); ++Index)
                {
                    if (Buffer[Index] != '\n') continue;
                    const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Buffer.GetData() + LineStart), Index - LineStart);
                    ProcessMessage(FString(Utf8.Length(), Utf8.Get()));
                    LineStart = Index + 1;
                }
                if (LineStart) Buffer.RemoveAt(0, LineStart, EAllowShrinking::No);
                ScanOffset = Buffer.Num();
            }

            if (SendBuffer.IsEmpty()) { Outgoing.Dequeue(SendBuffer); SendOffset = 0; }
            if (!SendBuffer.IsEmpty())
            {
                int32 Sent = 0;
                if (Client->Send(SendBuffer.GetData() + SendOffset, SendBuffer.Num() - SendOffset, Sent))
                {
                    SendOffset += Sent;
                    if (SendOffset == SendBuffer.Num()) SendBuffer.Reset();
                }
                else if (Subsystem->GetLastErrorCode() != SE_EWOULDBLOCK) bAlive = false;
            }
            if (FPlatformTime::Seconds() - LastReceive > 30.0) break;
            FPlatformProcess::Sleep(0.01f);
        }
        Client->Close();
        Subsystem->DestroySocket(Client);
        TArray<uint8> Discard;
        while (Outgoing.Dequeue(Discard)) {}
    }
    return 0;
}
