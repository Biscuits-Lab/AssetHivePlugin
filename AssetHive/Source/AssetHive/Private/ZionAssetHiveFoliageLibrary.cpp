#include "ZionAssetHiveFoliageLibrary.h"
#include "DynamicWindImportData.h"
#include "Engine/SkeletalMesh.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"

FString UZionAssetHiveFoliageLibrary::ApplyWindData(USkeletalMesh* Mesh, const FString& Filename, bool bExpectAssembly)
{
	if (!Mesh) return TEXT("Nanite Foliage 导入未生成骨骼网格体");
	if (Mesh->IsNaniteAssembly() != bExpectAssembly) return TEXT("USDA 实例与导入后的 Nanite Assembly 不一致");
	FString Text;
	FDynamicWindSkeletalImportData Data;
	if (!FFileHelper::LoadFileToString(Text, *Filename) || !FJsonObjectConverter::JsonObjectStringToUStruct(Text, &Data)
		|| Data.Joints.IsEmpty() || Data.SimulationGroups.IsEmpty()) return TEXT("Wind JSON 无效或缺少骨骼及模拟组");
	// 导入器可能规范化名称；不允许静默丢失任何已声明的风骨骼。
	for (const FDynamicWindJointImportData& Joint : Data.Joints)
	{
		if (Mesh->GetRefSkeleton().FindRawBoneIndex(Joint.JointName) == INDEX_NONE)
			return FString::Printf(TEXT("Wind 骨骼未在网格中找到：%s"), *Joint.JointName.ToString());
		if (Joint.SimulationGroupIndex < -1 || Joint.SimulationGroupIndex >= Data.SimulationGroups.Num()) return TEXT("Wind 模拟组索引越界");
	}
	Mesh->Modify();
	Mesh->NaniteSettings.bEnabled = true;
	if (!DynamicWind::ImportSkeletalData(*Mesh, Data)) return TEXT("无法应用 Dynamic Wind 数据");
	Mesh->NotifyNaniteSettingsChanged();
	return FString();
}

bool UZionAssetHiveFoliageLibrary::IsAssembly(USkeletalMesh* Mesh)
{
	return Mesh && Mesh->IsNaniteAssembly();
}
