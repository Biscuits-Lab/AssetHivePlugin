#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "AssetHiveTCPServer.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveListenerRecoveryTest, "AssetHive.Transport.ListenerRecovery",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveListenerRecoveryTest::RunTest(const FString& Parameters)
{
    TArray<TUniquePtr<FAssetHiveTCPServer>> Listeners;
    for (int32 Index = 0; Index < 10; ++Index)
    {
        auto Listener = MakeUnique<FAssetHiveTCPServer>();
        if (!Listener->Init()) break;
        for (const auto& Existing : Listeners)
            TestNotEqual(TEXT("Editors must own different ports"), Listener->GetListenPort(), Existing->GetListenPort());
        Listeners.Add(MoveTemp(Listener));
    }
    TestTrue(TEXT("At least one port must be available for the test"), Listeners.Num() > 0);
    if (Listeners.IsEmpty()) return false;
    FAssetHiveTCPServer Waiting;
    TestFalse(TEXT("Exhausted ports fail without stealing a listener"), Waiting.Init());
    const int32 ReleasedPort = Listeners[0]->GetListenPort();
    Listeners[0]->Shutdown();
    TestTrue(TEXT("A failed server can recover when a port is released"), Waiting.Init());
    TestEqual(TEXT("Recovery reuses the released port"), Waiting.GetListenPort(), ReleasedPort);
    TestTrue(TEXT("FRunnable Init is idempotent"), Waiting.Init());
    return true;
}
#endif
