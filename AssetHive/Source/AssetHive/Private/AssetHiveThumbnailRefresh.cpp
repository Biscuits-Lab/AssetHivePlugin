#include "AssetHiveThumbnailRefresh.h"
#include "Containers/Ticker.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "RenderCommandFence.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "RHI.h"

namespace AssetHiveThumbnailRefresh
{
namespace
{
    struct FPendingRefresh
    {
        TWeakObjectPtr<UMaterialInstanceConstant> Material;
        FRenderCommandFence Fence;
        bool bFenceStarted = false;
    };
    TArray<TSharedPtr<FPendingRefresh>> Pending;
    FTSTicker::FDelegateHandle TickHandle;

    bool Tick(float)
    {
        for (int32 Index = Pending.Num() - 1; Index >= 0; --Index)
        {
            const TSharedPtr<FPendingRefresh> Entry = Pending[Index];
            UMaterialInstanceConstant* Material = Entry->Material.Get();
            if (!Material) { Pending.RemoveAtSwap(Index); continue; }
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
            const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
#else
            const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIFeatureLevel);
#endif
            if (Resource && !Resource->IsCompilationFinished())
            {
                Entry->bFenceStarted = false;
                continue;
            }
            if (!Entry->bFenceStarted)
            {
                // Shader completion is a game-thread state. Uniform/resource updates
                // must reach the render thread before a thumbnail is requested.
                Entry->Fence.BeginFence();
                Entry->bFenceStarted = true;
                continue;
            }
            if (!Entry->Fence.IsFenceComplete()) continue;
            Pending.RemoveAtSwap(Index);
            // ThumbnailManager listens here: invalidates both package and browser
            // caches and enqueues a redraw. Do not PostEditChange/recompile or save.
            FPropertyChangedEvent Event(nullptr, EPropertyChangeType::Unspecified);
            FCoreUObjectDelegates::OnObjectPropertyChanged.Broadcast(Material, Event);
            UE_LOG(LogTemp, Log, TEXT("AssetHive: refreshed material thumbnail %s"), *Material->GetPathName());
        }
        if (!Pending.IsEmpty()) return true;
        TickHandle.Reset();
        return false;
    }
}

void Queue(UMaterialInstanceConstant* Material)
{
    if (!Material || IsRunningCommandlet()) return;
    check(IsInGameThread());
    for (const TSharedPtr<FPendingRefresh>& Entry : Pending)
    {
        if (Entry->Material == Material)
        {
            Entry->bFenceStarted = false;
            return;
        }
    }
    TSharedPtr<FPendingRefresh> Entry = MakeShared<FPendingRefresh>();
    Entry->Material = Material;
    Pending.Add(Entry);
    if (!TickHandle.IsValid())
        TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick), 0.25f);
}

void Shutdown()
{
    if (TickHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    TickHandle.Reset();
    Pending.Reset();
}
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Package.h"

namespace
{
    class FWaitForAssetHiveThumbnailRefresh : public IAutomationLatentCommand
    {
    public:
        explicit FWaitForAssetHiveThumbnailRefresh(FAutomationTestBase* InTest)
            : Test(InTest), Start(FPlatformTime::Seconds()), Material(NewObject<UMaterialInstanceConstant>())
        {
            bWasDirty = Material->GetOutermost()->IsDirty();
            Handle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddLambda(
                [this](UObject* Object, FPropertyChangedEvent&) { if (Object == Material.Get()) ++Notifications; });
            AssetHiveThumbnailRefresh::Queue(Material.Get());
            AssetHiveThumbnailRefresh::Queue(Material.Get());
        }
        ~FWaitForAssetHiveThumbnailRefresh()
        {
            FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(Handle);
        }
        bool Update() override
        {
            if (Notifications == 0 && FPlatformTime::Seconds() - Start < 10.0) return false;
            Test->TestEqual(TEXT("Duplicate requests produce one redraw notification"), Notifications, 1);
            Test->TestEqual(TEXT("Thumbnail refresh preserves package dirty state"), Material->GetOutermost()->IsDirty(), bWasDirty);
            Test->TestTrue(TEXT("Refresh ticker stops when the queue is empty"), AssetHiveThumbnailRefresh::Pending.IsEmpty());
            return true;
        }
    private:
        FAutomationTestBase* Test;
        double Start;
        TStrongObjectPtr<UMaterialInstanceConstant> Material;
        FDelegateHandle Handle;
        int32 Notifications = 0;
        bool bWasDirty = false;
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveThumbnailRefreshTest,
    "AssetHive.Import.ThumbnailRefresh", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveThumbnailRefreshTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForAssetHiveThumbnailRefresh(this));
    return true;
}
#endif
