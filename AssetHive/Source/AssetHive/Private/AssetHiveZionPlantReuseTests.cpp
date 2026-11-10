#if WITH_DEV_AUTOMATION_TESTS
#include "AssetHiveZionPlantReuse.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveZionPlantReuseTest,
    "AssetHive.Import.ZionPlantMaterialReuse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAssetHiveZionPlantReuseTest::RunTest(const FString &Parameters) {
  using namespace AssetHiveZionPlantReuse;
  const TArray<TArray<FVector2f>> UV3 = {{FVector2f(0.25f, 0.5f), FVector2f(0.75f, -0.5f)}};
  const TArray<TArray<FVector2f>> OtherUV3 = {{FVector2f(-12.5f, 8.0f)}};
  TestTrue(TEXT("完整 UV3 有效"), HasValidUV3(UV3));
  TestTrue(TEXT("不同几何可携带不同的有效 UV3"), HasValidUV3(OtherUV3));
  TestFalse(TEXT("缺少 UV3 时不提供证明"), HasValidUV3({}));
  TestFalse(TEXT("任一 LOD 缺失时不提供证明"), HasValidUV3({UV3[0], {}}));
  TestFalse(TEXT("全零 UV3 不视作有效编码"), HasValidUV3({{FVector2f::ZeroVector}}));

  auto First = MakeShared<FJsonObject>();
  auto Second = MakeShared<FJsonObject>();
  First->SetNumberField(TEXT("xMin"), 1);
  First->SetNumberField(TEXT("xMax"), 2);
  Second->SetNumberField(TEXT("xMax"), 2);
  Second->SetNumberField(TEXT("xMin"), 1);
  TestEqual(TEXT("JSON 字段顺序不影响参数比较"), ObjectKey(First), ObjectKey(Second));
  auto Profile = MakeShared<FJsonObject>();
  Profile->SetBoolField(TEXT("useRemappedUV3"), true);
  First->SetNumberField(TEXT("UV3 Remap Bounds Mode"), 2);
  First->SetNumberField(TEXT("yMin"), 3);
  First->SetNumberField(TEXT("yMax"), 4);
  First->SetNumberField(TEXT("wMin"), 5);
  First->SetNumberField(TEXT("wMax"), 6);
  Profile->SetObjectField(TEXT("scalarParameters"), First);
  const FString Proof = RemapProof(Profile, nullptr, HasValidUV3(UV3));
  TestFalse(TEXT("完整解码参数和有效 UV3 提供共享证明"), Proof.IsEmpty());
  TestEqual(TEXT("同一解码参数允许不同几何共享 MI"), Proof, RemapProof(Profile, nullptr, HasValidUV3(OtherUV3)));
  TestTrue(TEXT("模型无有效 UV3 时按变体创建 MI"), RemapProof(Profile, nullptr, false).IsEmpty());
  const FString Wind = WindKey(Profile, nullptr);
  const FString Compatible = TEXT("Grass|Leaf|Parent|Textures");
  TestEqual(TEXT("非 Remapped UV3 的兼容变体共享 MI"),
      InstanceKey(Compatible, TEXT("A"), false, TEXT("")), InstanceKey(Compatible, TEXT("B"), false, TEXT("")));
  TestNotEqual(TEXT("Remapped UV3 无证明时按变体创建 MI"),
      InstanceKey(Compatible, TEXT("A"), true, TEXT("")), InstanceKey(Compatible, TEXT("B"), true, TEXT("")));
  TestEqual(TEXT("解码完整一致时可跨变体共享 MI"),
      InstanceKey(Compatible, TEXT("A"), true, Proof), InstanceKey(Compatible, TEXT("B"), true, Proof));
  First->SetNumberField(TEXT("RippleFlexibility"), 1.e17);
  TestEqual(TEXT("普通风参数差异不阻止 UV3 兼容变体复用"), Proof, RemapProof(Profile, nullptr, true));
  First->SetNumberField(TEXT("RIPPLEFLEXIBILITY"), -1);
  TestEqual(TEXT("非解码参数的别名不参与 UV3 兼容判断"), Proof, RemapProof(Profile, nullptr, true));
  First->RemoveField(TEXT("RIPPLEFLEXIBILITY"));
  First->RemoveField(TEXT("RippleFlexibility"));
  auto Encoding = MakeShared<FJsonObject>();
  Encoding->SetNumberField(TEXT("version"), 2);
  Profile->SetObjectField(TEXT("uv3Encoding"), Encoding);
  TestNotEqual(TEXT("编码约定变化时不共享 MI"), Proof, RemapProof(Profile, nullptr, true));
  Profile->RemoveField(TEXT("uv3Encoding"));
  First->RemoveField(TEXT("wMax"));
  TestTrue(TEXT("解码参数不全时按变体创建 MI"), RemapProof(Profile, nullptr, true).IsEmpty());
  First->SetNumberField(TEXT("wMax"), 6);
  First->SetNumberField(TEXT("xMin"), 1.000001);
  TestNotEqual(TEXT("解码边界不同时不共享 MI"),
      InstanceKey(Compatible, TEXT("A"), true, Proof),
      InstanceKey(Compatible, TEXT("B"), true, RemapProof(Profile, nullptr, true)));
  auto Material = MakeShared<FJsonObject>();
  auto Overrides = MakeShared<FJsonObject>();
  Overrides->SetNumberField(TEXT("xMin"), 1);
  Material->SetObjectField(TEXT("scalarParameters"), Overrides);
  TestEqual(TEXT("材质覆盖后比较实际填入 MI 的参数"), Wind, WindKey(Profile, Material));
  TestEqual(TEXT("UV3 解码使用材质覆盖后的实际参数"), Proof, RemapProof(Profile, Material, true));
  First->SetNumberField(TEXT("xMin"), 1.0 + 1.e-12);
  TestEqual(TEXT("填入 MI 后相同的 float 值可以共享"), Wind, WindKey(Profile, nullptr));
  TestEqual(TEXT("UV3 解码按 MI 的 float 精度比较"), Proof, RemapProof(Profile, nullptr, true));
  First->SetNumberField(TEXT("XMIN"), 7);
  TestTrue(TEXT("参数别名有冲突时不提供共享依据"), WindKey(Profile, nullptr).IsEmpty());
  TestTrue(TEXT("解码参数别名有冲突时保留独立实例"), RemapProof(Profile, nullptr, true).IsEmpty());

  const auto Asset = [](const FString &Name, const FString &Type) -> TSharedPtr<FJsonValue> {
    auto Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("name"), Name);
    Object->SetStringField(TEXT("assetType"), Type);
    Object->SetStringField(TEXT("materialMatching"), TEXT("name"));
    return MakeShared<FJsonValueObject>(Object);
  };
  const auto Variant1 = Asset(TEXT("plant_01"), TEXT("3dplant"));
  const auto Variant2 = Asset(TEXT("plant_2"), TEXT("3dplant"));
  const auto Variant10 = Asset(TEXT("plant_10"), TEXT("3dplant"));
  const auto Object = Asset(TEXT("object"), TEXT("3d"));
  TArray<TSharedPtr<FJsonValue>> Assets = {Variant10, Object, Variant2, Variant1};
  OrderNamedPlants(Assets);
  TestTrue(TEXT("倒序选择仍优先以变体 1 提供共享默认值"), Assets[0] == Variant1);
  TestTrue(TEXT("其他分类的批次位置不变"), Assets[1] == Object);
  TestTrue(TEXT("变体 2 排在 10 之前"), Assets[2] == Variant2 && Assets[3] == Variant10);
  return true;
}
#endif
