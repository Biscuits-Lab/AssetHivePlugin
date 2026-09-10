#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

class FAssetHiveIdleFrameProbe : public IAutomationLatentCommand
{
public:
    explicit FAssetHiveIdleFrameProbe(FAutomationTestBase* InTest) : Test(InTest) {}
    virtual bool Update() override
    {
        const double Now = FPlatformTime::Seconds();
        if (Start == 0) Start = Now;
        if (Now - Start < 5.0) { Last = Now; return false; }
        Samples.Add((Now - Last) * 1000.0);
        Last = Now;
        if (Now - Start < 25.0) return false;
        Samples.Sort();
        int32 Over100 = 0;
        for (double Sample : Samples) if (Sample > 100.0) ++Over100;
        Test->AddInfo(FString::Printf(TEXT("AssetHive idle frame timing: samples=%d p95=%.3fms p99=%.3fms max=%.3fms over100ms=%d"),
            Samples.Num(), Samples[FMath::FloorToInt(Samples.Num() * 0.95)], Samples[FMath::FloorToInt(Samples.Num() * 0.99)], Samples.Last(), Over100));
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Start = 0, Last = 0;
    TArray<double> Samples;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveIdleTest, "AssetHive.Transport.IdleFrameTiming",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveIdleTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FAssetHiveIdleFrameProbe(this));
    return true;
}
#endif
