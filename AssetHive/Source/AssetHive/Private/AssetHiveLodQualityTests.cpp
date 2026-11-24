#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "AssetHiveLodQuality.h"
#include "AssetHiveStaticLodReduction.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveLodStructureTest, "AssetHive.LOD.StructureProtection", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveLodStructureTest::RunTest(const FString& Parameters)
{
    using namespace AssetHiveLodQuality;
    // A planar chain ring: the hole and its connection must survive reduction.
    TArray<FVector3f> Points = {{-2,-2,0},{2,-2,0},{2,2,0},{-2,2,0},{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
    TArray<uint32> Ring = {0,1,5,0,5,4,1,2,6,1,6,5,2,3,7,2,7,6,3,0,4,3,4,7};
    const auto Source = Describe(Points, Ring);
    TestTrue(TEXT("Unchanged ring is safe"), PreservesStructure(Source, Describe(Points, Ring)));
    TArray<uint32> Broken = Ring;
    Broken.RemoveAt(0, 6);
    TestFalse(TEXT("Cut link is rejected even with same bounds and component count"), PreservesStructure(Source, Describe(Points, Broken)));
    Points.Append({FVector3f(0,0,3), FVector3f(1,0,3), FVector3f(0,1,3)});
    TArray<uint32> WithPendant = Ring;
    WithPendant.Append({8,9,10});
    TestFalse(TEXT("Lost independent pendant is rejected"), PreservesStructure(Describe(Points, WithPendant), Source));
    auto Shrunken = Points;
    for (auto& P : Shrunken) P *= 0.8f;
    TestFalse(TEXT("Collapsed silhouette is rejected"), PreservesStructure(Source, Describe(Shrunken, Ring)));
    return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveComponentReductionTest, "AssetHive.LOD.ComponentReduction", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveComponentReductionTest::RunTest(const FString& Parameters)
{
    FMeshDescription Mesh;
    FStaticMeshAttributes Attributes(Mesh);
    Attributes.Register();
    Attributes.GetVertexInstanceUVs().SetNumChannels(2);
    const FPolygonGroupID Group = Mesh.CreatePolygonGroup();
    Attributes.GetPolygonGroupMaterialSlotNames()[Group] = TEXT("Cloth");
    auto Vertex = [&](FVector3f P) { auto V = Mesh.CreateVertex(); Attributes.GetVertexPositions()[V] = P; return V; };
    auto Face = [&](FVertexID A, FVertexID B, FVertexID C) {
        TArray<FVertexInstanceID> Instances;
        for (auto V : {A, B, C}) {
            auto I = Mesh.CreateVertexInstance(V); Instances.Add(I);
            Attributes.GetVertexInstanceNormals()[I] = FVector3f(0,0,1);
            Attributes.GetVertexInstanceTangents()[I] = FVector3f(1,0,0);
            Attributes.GetVertexInstanceBinormalSigns()[I] = 1;
            Attributes.GetVertexInstanceColors()[I] = FVector4f(1,1,1,1);
            auto P = Attributes.GetVertexPositions()[V];
            Attributes.GetVertexInstanceUVs().Set(I,0,FVector2f(P.X/20,P.Y/20));
            Attributes.GetVertexInstanceUVs().Set(I,1,FVector2f(P.Y/20,P.X/20));
        }
        Mesh.CreatePolygon(Group, Instances);
    };
    TArray<FVertexID> Grid;
    for (int32 Y=0;Y<=20;++Y) for(int32 X=0;X<=20;++X) Grid.Add(Vertex(FVector3f(X,Y,0)));
    for(int32 Y=0;Y<20;++Y) for(int32 X=0;X<20;++X) {
        int32 A=Y*21+X; Face(Grid[A],Grid[A+1],Grid[A+22]); Face(Grid[A],Grid[A+22],Grid[A+21]);
    }
    TArray<FVertexID> Ring;
    for (FVector3f P : TArray<FVector3f>{{-2,-2,3},{2,-2,3},{2,2,3},{-2,2,3},{-1,-1,3},{1,-1,3},{1,1,3},{-1,1,3}}) Ring.Add(Vertex(P));
    for(int32 I=0;I<4;++I) { int32 J=(I+1)%4; Face(Ring[I],Ring[J],Ring[J+4]); Face(Ring[I],Ring[J+4],Ring[I+4]); }
    FMeshDescription Reduced;
    TestTrue(TEXT("Mixed garment and chain can reduce"), AssetHiveStaticLodReduction::Reduce(Mesh,.25f,Reduced));
    TestTrue(TEXT("Large surface loses triangles"), Reduced.Triangles().Num()<Mesh.Triangles().Num());
    TestTrue(TEXT("Both components and chain hole survive"), AssetHiveLodQuality::PreservesStructure(AssetHiveStaticLodReduction::Describe(Mesh),AssetHiveStaticLodReduction::Describe(Reduced)));
    TestEqual(TEXT("Both UV channels survive assembly"), FStaticMeshConstAttributes(Reduced).GetVertexInstanceUVs().GetNumChannels(),2);
    return true;
}
#endif
