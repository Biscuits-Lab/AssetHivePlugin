#if WITH_DEV_AUTOMATION_TESTS
#include "AssetHiveAnimationImport.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBoneCompressionSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LATENT_AUTOMATION_COMMAND_FOUR_PARAMETER(FZionAssetHiveCheckAnimationSelection,
	FAutomationTestBase*, Test, TArray<FString>, ExpectedPaths, TSharedRef<FJsonObject>, InspectJob, double, Deadline);

bool FZionAssetHiveCheckAnimationSelection::Update()
{
	// 内容浏览器在后续 Slate tick 应用选择，等待完成后再核对回执。
	auto& Browser = FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get();
	TArray<FAssetData> SelectedAssets;
	Browser.GetSelectedAssets(SelectedAssets);
	TArray<FString> ActualPaths;
	for (const FAssetData& Asset : SelectedAssets)
		ActualPaths.Add(Asset.GetSoftObjectPath().ToString());
	ActualPaths.Sort();
	ExpectedPaths.Sort();
	if (ActualPaths != ExpectedPaths)
	{
		if (FPlatformTime::Seconds() < Deadline) return false;
		Test->AddError(TEXT("Content Browser selection does not match the successfully saved animation batch"));
		return true;
	}
	Test->TestEqual(TEXT("All successful animations are selected after the batch"), ActualPaths.Num(), ExpectedPaths.Num());
	AssetHiveAnimationImport::Run(InspectJob, [](int32, const FString&) {});
	TArray<FAssetData> AfterInspection;
	Browser.GetSelectedAssets(AfterInspection);
	Test->TestEqual(TEXT("Version inspection preserves the selection"), AfterInspection.Num(), SelectedAssets.Num());
	for (const FAssetData& Asset : SelectedAssets)
		Test->TestTrue(TEXT("Version inspection keeps each selected animation"), AfterInspection.Contains(Asset));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAssetHiveAnimationImportTest, "AssetHive.Import.Animation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAssetHiveAnimationImportTest::RunTest(const FString& Parameters)
{
    const auto NoProgress = [](int32, const FString&) {};
    auto Invalid = MakeShared<FJsonObject>();
    TestTrue(TEXT("Reject unsupported job before importing"), AssetHiveAnimationImport::Run(Invalid, NoProgress)->HasField(TEXT("error")));
    FString Source, Rig;
    if (!FParse::Value(FCommandLine::Get(), TEXT("AssetHiveAnimationValidationFbx="), Source)
        || !FParse::Value(FCommandLine::Get(), TEXT("AssetHiveAnimationValidationRig="), Rig))
    {
        AddInfo(TEXT("Supply animation and rig fixtures in an isolated project to test native import and reimport."));
        return true;
    }
    // Everything created by this test lives in the isolated validation project.
    const FString Folder = TEXT("/Game/AssetHiveAnimationValidation/") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    TStrongObjectPtr<UFbxImportUI> RigOptions(NewObject<UFbxImportUI>());
    RigOptions->bAutomatedImportShouldDetectType = false;
    RigOptions->MeshTypeToImport = FBXIT_SkeletalMesh;
    RigOptions->bImportAsSkeletal = true;
    RigOptions->bImportMesh = true;
    RigOptions->bImportAnimations = false;
    RigOptions->bImportMaterials = false;
    RigOptions->bImportTextures = false;
    RigOptions->bCreatePhysicsAsset = false;
    TStrongObjectPtr<UFbxFactory> RigFactory(NewObject<UFbxFactory>());
    RigFactory->ImportUI = RigOptions.Get();
    RigFactory->SetDetectImportTypeOnImport(false);
    TStrongObjectPtr<UAssetImportTask> RigTask(NewObject<UAssetImportTask>());
    RigTask->Filename = Rig;
    RigTask->DestinationPath = Folder;
    RigTask->DestinationName = TEXT("SK_Fixture");
    RigTask->bAutomated = true;
    RigTask->bSave = true;
    RigTask->Factory = RigFactory.Get();
    RigTask->Options = RigOptions.Get();
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().ImportAssetTasks({RigTask.Get()});
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, *(Folder + TEXT("/SK_Fixture.SK_Fixture")));
    if (!TestNotNull(TEXT("Fixture mesh imported"), Mesh) || !TestNotNull(TEXT("Fixture skeleton imported"), Mesh->GetSkeleton())) return false;

    auto Config = MakeShared<FJsonObject>();
    Config->SetNumberField(TEXT("version"), 1);
    Config->SetBoolField(TEXT("importMesh"), false);
    Config->SetBoolField(TEXT("importAnimations"), true);
    Config->SetBoolField(TEXT("importMaterials"), false);
    Config->SetBoolField(TEXT("importTextures"), false);
    Config->SetNumberField(TEXT("sampleRate"), 60);
    Config->SetStringField(TEXT("animationLength"), TEXT("exportedTime"));
    Config->SetStringField(TEXT("overwrite"), TEXT("reimport"));
    Config->SetBoolField(TEXT("showImportDialog"), false);
    Config->SetBoolField(TEXT("allowEngineCompressionFallback"), FParse::Param(FCommandLine::Get(), TEXT("AssetHiveAnimationInvalidDefaults")));
    auto Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("key"), TEXT("fixture"));
    Entry->SetStringField(TEXT("kind"), TEXT("animSequence"));
    Entry->SetStringField(TEXT("name"), TEXT("AS_Fixture"));
    Entry->SetStringField(TEXT("sourcePath"), Source);
    Entry->SetStringField(TEXT("targetPath"), Folder + TEXT("/AS_Fixture"));
    Entry->SetStringField(TEXT("skeletonPath"), Mesh->GetSkeleton()->GetPathName().LeftChop(Mesh->GetSkeleton()->GetName().Len() + 1));
    Entry->SetObjectField(TEXT("config"), Config);
    auto Job = MakeShared<FJsonObject>();
    Job->SetNumberField(TEXT("version"), 1);
    Job->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry)});
    const auto Status = [&]() { return AssetHiveAnimationImport::Run(Job, NoProgress)->GetArrayField(TEXT("entries"))[0]->AsObject(); };
    auto InvalidEntry = MakeShared<FJsonObject>();
    InvalidEntry->Values = Entry->Values;
    InvalidEntry->SetStringField(TEXT("key"), TEXT("invalid"));
    InvalidEntry->SetStringField(TEXT("name"), TEXT("AS_Invalid"));
    InvalidEntry->SetStringField(TEXT("targetPath"), Folder + TEXT("/AS_Invalid"));
    InvalidEntry->SetStringField(TEXT("sourcePath"), Source + TEXT(".missing.fbx"));
    Job->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry), MakeShared<FJsonValueObject>(InvalidEntry)});
    const auto Preflight = AssetHiveAnimationImport::Run(Job, NoProgress)->GetArrayField(TEXT("entries"));
    TestEqual(TEXT("Invalid later entry skips earlier valid import"), Preflight[0]->AsObject()->GetStringField(TEXT("status")), FString(TEXT("skipped")));
    TestFalse(TEXT("Failed batch did not save earlier asset"), IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Folder + TEXT("/AS_Fixture"), FPackageName::GetAssetPackageExtension())));
    Job->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry)});
    const auto FirstImport = Status();
    if (FirstImport->GetStringField(TEXT("status")) != TEXT("success")) AddError(FirstImport->GetStringField(TEXT("message")));
    TestEqual(TEXT("Native animation import succeeds"), FirstImport->GetStringField(TEXT("status")), FString(TEXT("success")));
    auto* Animation = LoadObject<UAnimSequence>(nullptr, *(Folder + TEXT("/AS_Fixture.AS_Fixture")));
    if (!TestNotNull(TEXT("AnimSequence exists"), Animation)) return false;
    TestTrue(TEXT("Root motion enabled"), Animation->bEnableRootMotion);
    TestTrue(TEXT("Root lock enabled"), Animation->bForceRootLock);
    const auto* Data = Cast<UFbxAnimSequenceImportData>(Animation->AssetImportData);
    if (TestNotNull(TEXT("Native FBX import metadata"), Data))
    {
        TestEqual(TEXT("60 FPS sampling"), Data->CustomSampleRate, 60);
        TestTrue(TEXT("Bone tracks imported"), Data->bImportBoneTracks);
        TestTrue(TEXT("Custom attributes imported"), Data->bImportCustomAttribute);
        TestFalse(TEXT("Redundant keys retained"), Data->bRemoveRedundantKeys);
    }
    Config->SetBoolField(TEXT("enableRootMotion"), false);
    Config->SetBoolField(TEXT("forceRootLock"), false);
    const auto Reimport = Status();
    TestEqual(TEXT("Reimport succeeds"), Reimport->GetStringField(TEXT("status")), FString(TEXT("success")));
    TestEqual(TEXT("Reimport is reported"), Reimport->GetStringField(TEXT("action")), FString(TEXT("reimported")));
    TestTrue(TEXT("Existing object/reference retained"), LoadObject<UAnimSequence>(nullptr, *Animation->GetPathName()) == Animation);
    TestFalse(TEXT("Root motion choice applied"), Animation->bEnableRootMotion);
    TestFalse(TEXT("Root lock choice applied"), Animation->bForceRootLock);
    const FString Filename = FPackageName::LongPackageNameToFilename(Animation->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    TestTrue(TEXT("Success is saved on disk"), IFileManager::Get().FileExists(*Filename));
    TestEqual(TEXT("Receipt gives actual asset object path"), Reimport->GetStringField(TEXT("path")), Animation->GetPathName());
    TestEqual(TEXT("Receipt includes imported FBX hash"), Reimport->GetStringField(TEXT("sourceHash")).Len(), 32);
    TestEqual(TEXT("Receipt includes saved package hash"), Reimport->GetStringField(TEXT("savedHash")).Len(), 32);
    auto InspectJob = MakeShared<FJsonObject>();
    InspectJob->SetNumberField(TEXT("version"), 1);
    InspectJob->SetBoolField(TEXT("inspectOnly"), true);
    InspectJob->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry)});
    const FDateTime BeforeInspection = IFileManager::Get().GetTimeStamp(*Filename);
    const auto Inspected = AssetHiveAnimationImport::Run(InspectJob, NoProgress)->GetArrayField(TEXT("entries"))[0]->AsObject();
    TestTrue(TEXT("Inspection finds saved asset"), Inspected->GetBoolField(TEXT("exists")));
    TestEqual(TEXT("Inspection matches saved receipt"), Inspected->GetStringField(TEXT("savedHash")), Reimport->GetStringField(TEXT("savedHash")));
    TestTrue(TEXT("Inspection does not write asset file"), BeforeInspection == IFileManager::Get().GetTimeStamp(*Filename));
    Animation->MarkPackageDirty();
    TestTrue(TEXT("Inspection detects unsaved engine edits"), AssetHiveAnimationImport::Run(InspectJob, NoProgress)->GetArrayField(TEXT("entries"))[0]->AsObject()->GetBoolField(TEXT("dirty")));
    Animation->GetOutermost()->SetDirtyFlag(false);
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, true);
    TestEqual(TEXT("Read-only target is rejected"), Status()->GetStringField(TEXT("status")), FString(TEXT("failed")));
    FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Filename, false);
    Animation->MarkPackageDirty();
    TestEqual(TEXT("Unsaved user changes are protected"), Status()->GetStringField(TEXT("status")), FString(TEXT("failed")));
    Animation->GetOutermost()->SetDirtyFlag(false);
    auto* PreviousCompression = Animation->BoneCompressionSettings.Get();
    Animation->BoneCompressionSettings = NewObject<UAnimBoneCompressionSettings>();
    Config->SetBoolField(TEXT("allowEngineCompressionFallback"), false);
    TestEqual(TEXT("Invalid compression is rejected before reimport"), Status()->GetStringField(TEXT("status")), FString(TEXT("failed")));
    Config->SetBoolField(TEXT("allowEngineCompressionFallback"), true);
    const auto Fallback = Status();
    TestEqual(TEXT("Scoped compression fallback saves the animation"), Fallback->GetStringField(TEXT("status")), FString(TEXT("success")));
    TestTrue(TEXT("Compression is valid after fallback"), Animation->IsCompressedDataValid());
    TestTrue(TEXT("Engine compression selected for this asset"), Animation->BoneCompressionSettings == PreviousCompression);
	// 批次完成后仅选中已保存动画，版本检查不得改变当前选择。
	auto SecondEntry = MakeShared<FJsonObject>();
	SecondEntry->Values = Entry->Values;
	SecondEntry->SetStringField(TEXT("key"), TEXT("fixture-second"));
	SecondEntry->SetStringField(TEXT("name"), TEXT("AS_Fixture_Second"));
	SecondEntry->SetStringField(TEXT("targetPath"), Folder + TEXT("/AS_Fixture_Second"));
	Job->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry), MakeShared<FJsonValueObject>(SecondEntry)});
	const auto Batch = AssetHiveAnimationImport::Run(Job, NoProgress)->GetArrayField(TEXT("entries"));
	TestEqual(TEXT("Batch saves both animations"), Batch.Num(), 2);
	for (const auto& Item : Batch)
		TestEqual(TEXT("Batch entry saved successfully"), Item->AsObject()->GetStringField(TEXT("status")), FString(TEXT("success")));
	if (!IsRunningCommandlet())
	{
		TArray<FString> ExpectedPaths;
		for (const auto& Item : Batch)
			ExpectedPaths.Add(Item->AsObject()->GetStringField(TEXT("path")));
		ADD_LATENT_AUTOMATION_COMMAND(FZionAssetHiveCheckAnimationSelection(this, ExpectedPaths, InspectJob, FPlatformTime::Seconds() + 15.0));
	}
	Job->SetArrayField(TEXT("animations"), {MakeShared<FJsonValueObject>(Entry)});
    Entry->SetStringField(TEXT("sourcePath"), Source + TEXT(".missing.fbx"));
    TestEqual(TEXT("Missing source is failed, not false success on existing asset"), Status()->GetStringField(TEXT("status")), FString(TEXT("failed")));
    Entry->SetStringField(TEXT("sourcePath"), Source);
    Entry->SetStringField(TEXT("skeletonPath"), TEXT("/Game/AssetHiveAnimationValidation/MissingSkeleton"));
    TestEqual(TEXT("Missing Skeleton is reported"), Status()->GetStringField(TEXT("status")), FString(TEXT("failed")));
    return true;
}
#endif
