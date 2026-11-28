#include "ZionAssetHiveNaniteImport.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

TSharedRef<FJsonObject> ZionAssetHiveNaniteImport::Run(const TSharedRef<FJsonObject>& Asset, const FString& Folder)
{
	auto Result = MakeShared<FJsonObject>();
	auto* Python = IPythonScriptPlugin::Get();
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AssetHive"));
	if (!Python || !Python->IsPythonAvailable() || !Plugin.IsValid())
	{
		Result->SetStringField(TEXT("error"), TEXT("Nanite Foliage 需要 Python Editor Script Plugin"));
		return Result;
	}
	const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Content/Python/zion_assethive_nanite_foliage.py"));
	const FString JobFolder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetHive/Jobs"));
	IFileManager::Get().MakeDirectory(*JobFolder, true);
	const FString Base = JobFolder / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Input = Base + TEXT(".request.json"), Output = Base + TEXT(".result.json");
	auto Job = MakeShared<FJsonObject>();
	Job->SetObjectField(TEXT("asset"), Asset);
	// 分支使用不同父材质，不能覆盖同资产原始 FBX 使用的材质实例。
	Job->SetStringField(TEXT("folder"), Folder / TEXT("NaniteFoliage"));
	FString Serialized;
	FJsonSerializer::Serialize(Job, TJsonWriterFactory<>::Create(&Serialized));
	if (!FPaths::FileExists(Script) || !FFileHelper::SaveStringToFile(Serialized, *Input, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result->SetStringField(TEXT("error"), TEXT("Nanite Foliage 导入脚本或任务文件不可用"));
		return Result;
	}
	const auto Quote = [](const FString& Value)
	{
		FString Encoded;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Encoded);
		Writer->WriteValue(Value); Writer->Close();
		return Encoded;
	};
	const FString Command = FString::Printf(TEXT("import runpy; runpy.run_path(%s)['run_job_file'](%s, %s)"), *Quote(Script), *Quote(Input), *Quote(Output));
	FString Text;
	TSharedPtr<FJsonObject> Parsed;
	if (Python->ExecPythonCommand(*Command) && FFileHelper::LoadFileToString(Text, *Output)
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed) && Parsed.IsValid()) Result = Parsed.ToSharedRef();
	else Result->SetStringField(TEXT("error"), TEXT("Nanite Foliage 导入失败，请查看 UE Output Log"));
	IFileManager::Get().Delete(*Input);
	IFileManager::Get().Delete(*Output);
	return Result;
}
