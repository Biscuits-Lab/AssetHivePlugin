#include "AssetHiveAnimationImport.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "AnimationUtils.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/ReimportFbxAnimSequenceFactory.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
    bool Flag(const TSharedRef<FJsonObject>& Json, const TCHAR* Name, bool Default)
    {
        bool Value = Default;
        Json->TryGetBoolField(Name, Value);
        return Value;
    }

    bool GamePath(const FString& Path)
    {
        return Path.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Path)
            && !Path.Contains(TEXT(".."));
    }

    TSharedRef<FJsonObject> ImportOne(const TSharedRef<FJsonObject>& Entry, bool bValidateOnly = false)
    {
        auto Result = MakeShared<FJsonObject>();
        FString Key, Name, Source, Target, SkeletonPath, Kind;
        Entry->TryGetStringField(TEXT("key"), Key);
        Entry->TryGetStringField(TEXT("name"), Name);
        Entry->TryGetStringField(TEXT("kind"), Kind);
        Entry->TryGetStringField(TEXT("sourcePath"), Source);
        Entry->TryGetStringField(TEXT("targetPath"), Target);
        Entry->TryGetStringField(TEXT("skeletonPath"), SkeletonPath);
        Result->SetStringField(TEXT("key"), Key);
        Result->SetStringField(TEXT("path"), Target);
        const auto Fail = [&Result](const FString& Message)
        {
            Result->SetStringField(TEXT("status"), TEXT("failed"));
            Result->SetStringField(TEXT("message"), Message);
            return Result;
        };
        if (Key.IsEmpty() || Kind != TEXT("animSequence") || !GamePath(Target)
            || FPackageName::GetLongPackageAssetName(Target) != Name || (!SkeletonPath.IsEmpty() && !GamePath(SkeletonPath)))
            return Fail(TEXT("动画名称、目标路径或 Skeleton 路径不合法"));
        if (!Source.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase) || !FPaths::FileExists(Source))
            return Fail(TEXT("动画 FBX 文件不存在：") + Source);
        const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
        if (!Entry->TryGetObjectField(TEXT("config"), ConfigPtr) || !ConfigPtr || !ConfigPtr->IsValid())
            return Fail(TEXT("缺少动画导入设置"));
        const auto Config = ConfigPtr->ToSharedRef();
        double Version = 0, SampleRate = 0;
        FString Length, Overwrite;
        Config->TryGetNumberField(TEXT("version"), Version);
        Config->TryGetNumberField(TEXT("sampleRate"), SampleRate);
        Config->TryGetStringField(TEXT("animationLength"), Length);
        Config->TryGetStringField(TEXT("overwrite"), Overwrite);
        if (Version != 1 || Flag(Config, TEXT("importMesh"), true) || !Flag(Config, TEXT("importAnimations"), false)
            || Flag(Config, TEXT("importMaterials"), true) || Flag(Config, TEXT("importTextures"), true)
            || SampleRate != 60 || Length != TEXT("exportedTime") || Overwrite != TEXT("reimport"))
            return Fail(TEXT("不支持的动画导入设置，请更新 AssetHive"));
        const bool bShowDialog = Flag(Config, TEXT("showImportDialog"), true) && !IsRunningCommandlet();
        USkeleton* Skeleton = SkeletonPath.IsEmpty() ? nullptr : LoadObject<USkeleton>(nullptr, *SkeletonPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (!Skeleton && (!SkeletonPath.IsEmpty() || !bShowDialog)) return Fail(TEXT("Skeleton 不存在：") + SkeletonPath);
        const FString ObjectPath = Target + TEXT(".") + Name;
        const FString TargetFilename = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
        if (IFileManager::Get().FileExists(*TargetFilename) && IFileManager::Get().IsReadOnly(*TargetFilename))
            return Fail(TEXT("目标资产为只读，请先在 P4 中迁出后重试：") + Target);
        UObject* ExistingObject = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
        if (!ExistingObject && IFileManager::Get().FileExists(*TargetFilename))
            return Fail(TEXT("目标文件存在但无法读取，未覆盖：") + Target);
        UAnimSequence* Existing = Cast<UAnimSequence>(ExistingObject);
        if (ExistingObject && !Existing) return Fail(TEXT("目标位置已有非动画资产，未覆盖"));
        if (Existing && Existing->GetOutermost()->IsDirty())
            return Fail(TEXT("目标动画有未保存修改，请先在 UE 中处理后重试：") + Target);
        if (Existing && !Skeleton) Skeleton = Existing->GetSkeleton();
        if (Existing && Existing->GetSkeleton() != Skeleton)
            return Fail(TEXT("已有动画使用不同 Skeleton，请选择原 Skeleton 或更换目标目录"));

        UAnimBoneCompressionSettings* BoneSettings = Existing ? Existing->BoneCompressionSettings.Get() : FAnimationUtils::GetDefaultAnimationBoneCompressionSettings();
        UAnimCurveCompressionSettings* CurveSettings = Existing ? Existing->CurveCompressionSettings.Get() : FAnimationUtils::GetDefaultAnimationCurveCompressionSettings();
        bool bCompressionFallback = false;
        if (!BoneSettings || !BoneSettings->AreSettingsValid())
        {
            if (!Flag(Config, TEXT("allowEngineCompressionFallback"), false))
                return Fail(TEXT("骨骼压缩设置没有可用编码器，未导入；可启用“本次动画使用 UE 默认压缩”，或修复工程压缩设置"));
            BoneSettings = LoadObject<UAnimBoneCompressionSettings>(nullptr, TEXT("/Engine/Animation/DefaultAnimBoneCompressionSettings.DefaultAnimBoneCompressionSettings"));
            bCompressionFallback = true;
        }
        if (!CurveSettings || !CurveSettings->AreSettingsValid())
        {
            if (!Flag(Config, TEXT("allowEngineCompressionFallback"), false))
                return Fail(TEXT("曲线压缩设置不可用，未导入；请检查工程压缩设置"));
            CurveSettings = LoadObject<UAnimCurveCompressionSettings>(nullptr, TEXT("/Engine/Animation/DefaultAnimCurveCompressionSettings.DefaultAnimCurveCompressionSettings"));
            bCompressionFallback = true;
        }
        if (!BoneSettings || !BoneSettings->AreSettingsValid() || !CurveSettings || !CurveSettings->AreSettingsValid())
            return Fail(TEXT("UE 默认压缩设置也不可用，未导入，请检查引擎内容"));
        if (bValidateOnly)
        {
            Result->SetStringField(TEXT("status"), TEXT("ready"));
            return Result;
        }

        TStrongObjectPtr<UFbxImportUI> Options(NewObject<UFbxImportUI>());
        Options->bAutomatedImportShouldDetectType = false;
        Options->MeshTypeToImport = FBXIT_Animation;
        Options->OriginalImportType = FBXIT_Animation;
        Options->bImportAsSkeletal = true;
        Options->bImportMesh = false;
        Options->bImportAnimations = true;
        Options->bImportMaterials = false;
        Options->bImportTextures = false;
        Options->Skeleton = Skeleton;
        Options->OverrideAnimationName = Name;
        UFbxAnimSequenceImportData* Data = Options->AnimSequenceImportData;
        Data->AnimationLength = FBXALIT_ExportedTime;
        Data->bUseDefaultSampleRate = false;
        Data->CustomSampleRate = 60;
        Data->bImportBoneTracks = Flag(Config, TEXT("importBoneTracks"), true);
        Data->bImportCustomAttribute = Flag(Config, TEXT("importCustomAttribute"), true);
        Data->bRemoveRedundantKeys = Flag(Config, TEXT("removeRedundantKeys"), false);
        Data->bConvertScene = Flag(Config, TEXT("convertScene"), true);
        Data->bForceFrontXAxis = false;

        TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
        Task->Filename = Source;
        Task->DestinationPath = FPackageName::GetLongPackagePath(Target);
        Task->DestinationName = Name;
        Task->Options = Options.Get();
        Task->bAutomated = !bShowDialog;
        Task->bReplaceExisting = true;
        Task->bReplaceExistingSettings = true;
        Task->bSave = false;
        // Configure only this import's new sequence before the FBX controller
        // queues compression. A broken project default must not run first.
        TStrongObjectPtr<UAnimBoneCompressionSettings> BoneGuard(BoneSettings);
        TStrongObjectPtr<UAnimCurveCompressionSettings> CurveGuard(CurveSettings);
        const auto ConstructedHandle = FCoreUObjectDelegates::OnObjectConstructed.AddLambda(
            [ObjectPath, BoneSettings, CurveSettings](UObject* Object)
            {
                if (IsInGameThread())
                    if (auto* Sequence = Cast<UAnimSequence>(Object); Sequence && Sequence->GetPathName() == ObjectPath)
                    {
                        Sequence->BoneCompressionSettings = BoneSettings;
                        Sequence->CurveCompressionSettings = CurveSettings;
                    }
            });
        ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectConstructed.Remove(ConstructedHandle); };
        UAnimSequence* Animation = nullptr;
        if (Existing)
        {
            // Use the native animation reimporter and its result, rather than
            // assuming success because the existing object can still be loaded.
            TStrongObjectPtr<UReimportFbxAnimSequenceFactory> Factory(NewObject<UReimportFbxAnimSequenceFactory>());
            Factory->AssetImportTask = Task.Get();
            Factory->ImportUI = Options.Get();
            Factory->SetAutomatedReimport(!bShowDialog);
            Factory->SetForceShowDialog(bShowDialog);
            auto* OldData = UFbxAnimSequenceImportData::GetImportDataForAnimSequence(Existing, nullptr);
            const FString PreviousSource = OldData ? OldData->GetFirstFilename() : FString();
            auto* PreviousBoneSettings = Existing->BoneCompressionSettings.Get();
            auto* PreviousCurveSettings = Existing->CurveCompressionSettings.Get();
            Existing->WaitOnExistingCompression();
            Existing->BoneCompressionSettings = BoneSettings;
            Existing->CurveCompressionSettings = CurveSettings;
            Factory->SetReimportPaths(Existing, {Source});
            const auto Status = Factory->Reimport(Existing);
            Factory->CleanUp();
            if (Status != EReimportResult::Succeeded)
            {
                Factory->SetReimportPaths(Existing, {PreviousSource});
                Existing->BoneCompressionSettings = PreviousBoneSettings;
                Existing->CurveCompressionSettings = PreviousCurveSettings;
                return Fail(TEXT("动画重新导入失败或已取消，请查看 UE Output Log"));
            }
            Animation = Existing;
        }
        else
        {
            TStrongObjectPtr<UFbxFactory> Factory(NewObject<UFbxFactory>());
            Factory->SetDetectImportTypeOnImport(false);
            Factory->EnableShowOption();
            Task->Factory = Factory.Get();
            FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({Task.Get()});
            Factory->CleanUp();
            for (UObject* Object : Task->GetObjects())
                if (auto* Imported = Cast<UAnimSequence>(Object); Imported && Imported->GetPathName() == ObjectPath)
                    Animation = Imported;
            if (!Animation) return Fail(TEXT("动画导入失败或已取消，请查看 UE Output Log"));
        }
        // Wait before replacing settings used by the factory's async compression.
        Animation->WaitOnExistingCompression();
        Animation->BoneCompressionSettings = BoneSettings;
        Animation->CurveCompressionSettings = CurveSettings;
        Animation->bEnableRootMotion = Flag(Config, TEXT("enableRootMotion"), true);
        Animation->bForceRootLock = Flag(Config, TEXT("forceRootLock"), true);
        Animation->PostEditChange();
        Animation->WaitOnExistingCompression();
        if (!Animation->IsCompressedDataValid())
            return Fail(TEXT("动画已读取，但 UE 动画压缩失败；请检查目标工程的动画压缩设置和 Output Log"));
        Animation->MarkPackageDirty();
        UPackage* Package = Animation->GetOutermost();
        const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
        FSavePackageArgs SaveArgs;
        SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
        SaveArgs.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Animation, *Filename, SaveArgs))
            return Fail(TEXT("动画已导入但保存失败，请在 UE 中检查并保存：") + Target);
        if (!IFileManager::Get().FileExists(*Filename))
            return Fail(TEXT("动画保存后未找到文件：") + Filename);
        Result->SetStringField(TEXT("status"), TEXT("success"));
        Result->SetStringField(TEXT("action"), Existing ? TEXT("reimported") : TEXT("added"));
        Result->SetStringField(TEXT("path"), Animation->GetPathName());
        Result->SetStringField(TEXT("savedFilename"), Filename);
        Result->SetStringField(TEXT("savedHash"), LexToString(FMD5Hash::HashFile(*Filename)));
        if (Animation->AssetImportData && !Animation->AssetImportData->GetSourceData().SourceFiles.IsEmpty())
            Result->SetStringField(TEXT("sourceHash"), LexToString(Animation->AssetImportData->GetSourceData().SourceFiles[0].FileHash));
        Result->SetStringField(TEXT("boneCompressionPath"), BoneSettings->GetPathName());
        Result->SetStringField(TEXT("message"), bCompressionFallback
            ? TEXT("已保存；本次动画使用 UE 默认压缩，工程配置未修改") : TEXT("已保存"));
        if (Animation->GetSkeleton())
            Result->SetStringField(TEXT("skeletonPath"), Animation->GetSkeleton()->GetOutermost()->GetName());
        return Result;
    }
}

TSharedRef<FJsonObject> AssetHiveAnimationImport::Run(const TSharedRef<FJsonObject>& Job, TFunction<void(int32, const FString&)> Progress)
{
    check(IsInGameThread());
    auto Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("entries"), {});
    double Version = 0;
    Job->TryGetNumberField(TEXT("version"), Version);
    const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
    if (Version != 1 || !Job->TryGetArrayField(TEXT("animations"), Animations) || !Animations || Animations->IsEmpty())
    {
        Result->SetStringField(TEXT("error"), TEXT("无效的动画导入任务"));
        return Result;
    }
    TArray<TSharedPtr<FJsonValue>> Entries;
    TSet<FString> Targets, Keys;
    // Validate the entire batch before any import, so an invalid/duplicate
    // later entry cannot leave earlier successes without a result receipt.
    for (const auto& Value : *Animations)
    {
        const auto Entry = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        FString Target, Key;
        if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("targetPath"), Target)
            || !Entry->TryGetStringField(TEXT("key"), Key) || Key.IsEmpty()
            || Targets.Contains(Target.ToLower()) || Keys.Contains(Key))
        {
            Result->SetStringField(TEXT("error"), TEXT("动画任务包含无效条目或重复的目标/标识"));
            return Result;
        }
        Targets.Add(Target.ToLower());
        Keys.Add(Key);
    }
    if (Flag(Job, TEXT("inspectOnly"), false))
    {
        for (const auto& Value : *Animations)
        {
            const auto Entry = Value->AsObject();
            auto Row = MakeShared<FJsonObject>();
            const FString Target = Entry->GetStringField(TEXT("targetPath"));
            Row->SetStringField(TEXT("key"), Entry->GetStringField(TEXT("key")));
            if (!GamePath(Target))
            {
                Row->SetStringField(TEXT("error"), TEXT("UE 目标路径不合法"));
            }
            else
            {
                const FString Filename = FPackageName::LongPackageNameToFilename(Target, FPackageName::GetAssetPackageExtension());
                const bool bExists = IFileManager::Get().FileExists(*Filename);
                Row->SetBoolField(TEXT("exists"), bExists);
                UPackage* LoadedPackage = FindPackage(nullptr, *Target);
                Row->SetBoolField(TEXT("dirty"), LoadedPackage && LoadedPackage->IsDirty());
                if (bExists)
                {
                    const auto Hash = FMD5Hash::HashFile(*Filename);
                    if (!Hash.IsValid()) Row->SetStringField(TEXT("error"), TEXT("UE 资产文件无法读取"));
                    else Row->SetStringField(TEXT("savedHash"), LexToString(Hash));
                }
            }
            Entries.Add(MakeShared<FJsonValueObject>(Row));
        }
        Result->SetArrayField(TEXT("entries"), Entries);
        return Result;
    }
    bool bPreflightFailed = false;
    for (const auto& Value : *Animations)
    {
        const auto Row = ImportOne(Value->AsObject().ToSharedRef(), true);
        bPreflightFailed |= Row->GetStringField(TEXT("status")) == TEXT("failed");
        Entries.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (bPreflightFailed)
    {
        for (const auto& Value : Entries)
        {
            const auto Row = Value->AsObject();
            if (Row->GetStringField(TEXT("status")) == TEXT("ready"))
            {
                Row->SetStringField(TEXT("status"), TEXT("skipped"));
                Row->SetStringField(TEXT("message"), TEXT("本批次检查未通过，未导入任何动画"));
            }
        }
        Result->SetArrayField(TEXT("entries"), Entries);
        return Result;
    }
    Entries.Reset();
    for (int32 Index = 0; Index < Animations->Num(); ++Index)
    {
        const auto Entry = (*Animations)[Index]->AsObject();
        FString Name;
        Entry->TryGetStringField(TEXT("name"), Name);
        Progress(FMath::RoundToInt(Index * 100.0 / Animations->Num()), TEXT("正在导入动画：") + Name);
        const auto Row = ImportOne(Entry.ToSharedRef());
        Entries.Add(MakeShared<FJsonValueObject>(Row));
    }
    if (GEditor && !IsRunningCommandlet())
    {
        TArray<UObject*> SavedAnimations;
        for (const auto& Value : Entries)
        {
            const auto Row = Value->AsObject();
            if (Row->GetStringField(TEXT("status")) != TEXT("success")) continue;
            if (UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *Row->GetStringField(TEXT("path"))))
                SavedAnimations.Add(Animation);
        }
        // 在所有原生导入窗口关闭后统一选中，仅包含实际保存成功的动画。
        if (!SavedAnimations.IsEmpty())
        {
            FContentBrowserModule& ContentBrowser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser"));
            ContentBrowser.Get().SyncBrowserToAssets(SavedAnimations, true, true);
        }
    }
    Result->SetArrayField(TEXT("entries"), Entries);
    return Result;
}
