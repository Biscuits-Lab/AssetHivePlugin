#include "AssetHiveSuitImport.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Animation/Skeleton.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

namespace
{
    // Only forward the importer-owned progress marker; ordinary editor logs remain untouched.
    class FSuitProgressLog final : public FOutputDevice
    {
        TFunction<void(int32, const FString&)> Callback;
    public:
        explicit FSuitProgressLog(TFunction<void(int32, const FString&)> InCallback) : Callback(MoveTemp(InCallback)) { GLog->AddOutputDevice(this); }
        ~FSuitProgressLog() { GLog->RemoveOutputDevice(this); }
        void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName& Category) override
        {
            if (Category != TEXT("LogPython")) return;
            const FString Line(Text);
            const FString Prefix(TEXT("AssetHiveSuitProgress:"));
            if (!Line.StartsWith(Prefix)) return;
            FString Percent, Stage;
            if (Line.Mid(Prefix.Len()).Split(TEXT("|"), &Percent, &Stage)) Callback(FCString::Atoi(*Percent), Stage);
        }
    };
}

TSharedRef<FJsonObject> AssetHiveSuitImport::Run(const TSharedRef<FJsonObject>& Job, TFunction<void(int32, const FString&)> Progress)
{
    // Persist the source identity in registry tags too: moves/copies can be found without loading all assets.
    UObject::GetMetaDataTagsForAssetRegistry().Add(TEXT("AssetHive.SuitKey"));
    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("entries"), {});
    auto* Python = IPythonScriptPlugin::Get();
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("AssetHive"));
    if (!Python || !Python->IsPythonAvailable() || !Plugin.IsValid())
    {
        Result->SetStringField(TEXT("error"), TEXT("套装导入需要已加载的 Python Editor Script Plugin"));
        return Result;
    }
    const FString Script = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Content/Python/assethive_suit_import.py"));
    const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("AssetHive/Jobs"));
    IFileManager::Get().MakeDirectory(*Folder, true);
    const FString Base = Folder / FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Input = Base + TEXT(".request.json");
    const FString Output = Base + TEXT(".result.json");
    const TArray<TSharedPtr<FJsonValue>>* Suits = nullptr;
    if (Job->TryGetArrayField(TEXT("suits"), Suits))
    for (const auto& SuitValue : *Suits)
    {
        const auto Suit = SuitValue->AsObject();
        const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        if (!Suit.IsValid() || !Suit->TryGetArrayField(TEXT("entries"), Entries)) continue;
        for (const auto& EntryValue : *Entries)
        {
            const auto Entry = EntryValue->AsObject();
            FString Kind, SkeletonPath;
            if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("skeletalMesh")) continue;
            Entry->TryGetStringField(TEXT("skeletonPath"), SkeletonPath);
            TArray<TSharedPtr<FJsonValue>> BoneRows;
            if (USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath))
            {
                const auto& Reference = Skeleton->GetReferenceSkeleton();
                for (int32 Index = 0; Index < Reference.GetNum(); ++Index)
                {
                    auto Bone = MakeShared<FJsonObject>();
                    Bone->SetStringField(TEXT("name"), Reference.GetBoneName(Index).ToString());
                    const int32 Parent = Reference.GetParentIndex(Index);
                    Bone->SetStringField(TEXT("parent"), Parent == INDEX_NONE ? FString() : Reference.GetBoneName(Parent).ToString());
                    BoneRows.Add(MakeShared<FJsonValueObject>(Bone));
                }
            }
            // Always replace caller data with the editor's actual reference hierarchy.
            Entry->SetArrayField(TEXT("targetBones"), BoneRows);
        }
    }
    FString Serialized;
    FJsonSerializer::Serialize(Job, TJsonWriterFactory<>::Create(&Serialized));
    if (!FPaths::FileExists(Script) || !FFileHelper::SaveStringToFile(Serialized, *Input, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Result->SetStringField(TEXT("error"), TEXT("套装导入脚本或任务文件不可用"));
        return Result;
    }
    // JSON strings are also valid Python string literals. No task-provided code executes.
    const auto Quote = [](const FString& Value) {
        FString Encoded;
        auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Encoded);
        Writer->WriteValue(Value); Writer->Close();
        return Encoded;
    };
    const FString Command = FString::Printf(TEXT("import runpy; runpy.run_path(%s)['run_job_file'](%s, %s)"),
        *Quote(Script), *Quote(Input), *Quote(Output));
    FString Text;
    TSharedPtr<FJsonObject> Parsed;
    FSuitProgressLog ProgressLog(MoveTemp(Progress));
    if (Python->ExecPythonCommand(*Command) && FFileHelper::LoadFileToString(Text, *Output)
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed) && Parsed.IsValid())
        Result = Parsed.ToSharedRef();
    else
        Result->SetStringField(TEXT("error"), TEXT("套装导入执行失败，请查看 UE Output Log"));
    IFileManager::Get().Delete(*Input);
    IFileManager::Get().Delete(*Output);
    return Result;
}
