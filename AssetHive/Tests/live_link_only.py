"""Prepare an isolated content comparison; phase apply validates no package changes."""
import unreal, runpy, pathlib, json, os
assert pathlib.Path(unreal.Paths.get_project_file_path()).stem == 'LodTest'
module = runpy.run_path(str(pathlib.Path(__file__).resolve().parents[1] / 'Content/Python/assethive_link_only.py'))
folder = pathlib.Path(unreal.Paths.project_dir()) / 'Saved/LinkOnlyTest'
folder.mkdir(parents=True, exist_ok=True)
if os.environ.get('ASSETHIVE_LINK_PHASE') == 'apply':
    job = json.loads((folder/'apply.json').read_text(encoding='utf-8'))
    result = module['apply'](job)
    assert all(row['status'] == 'success' for row in result['entries']), result
    original = json.loads((folder/'inspect.json').read_text(encoding='utf-8'))
    for row in original['entries']:
        obj = unreal.EditorAssetLibrary.load_asset(row['path'])
        assert module['snapshot'](obj) == row['state']
        assert module['dependencies'](obj) == row['dependencies']
    (folder/'applied.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
else:
    obj = unreal.EditorAssetLibrary.load_asset('/Game/LinkOnlyTest/SM_Test') if unreal.EditorAssetLibrary.does_asset_exist('/Game/LinkOnlyTest/SM_Test') else None
    if not obj:
        obj = unreal.EditorAssetLibrary.duplicate_asset('/Engine/BasicShapes/Cube', '/Game/LinkOnlyTest/SM_Test')
        assert unreal.EditorAssetLibrary.save_loaded_asset(obj, False)
    assert obj
    source = folder/'source.fbx'
    task = unreal.AssetExportTask()
    task.object = obj; task.filename = str(source); task.automated = True; task.prompt = False; task.replace_identical = True
    task.exporter = unreal.StaticMeshExporterFBX(); task.options = unreal.FbxExportOption(); task.options.level_of_detail = False
    assert unreal.Exporter.run_asset_export_task(task)
    key = str(unreal.EditorAssetLibrary.get_metadata_tag(obj, 'AssetHive.SuitKey')) or 'link-test:staticMesh:main'
    job = {'suits':[{'assetId':key.split(':')[0], 'entries':[{'key':key,'name':'SM_LodV2','kind':'staticMesh','targetPath':obj.get_path_name(), 'sourcePath':str(source),'sourceHash':module['digest_file'](source)}]}]}
    result = module['prepare'](job)
    (folder/'inspect.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    assert result['entries'][0]['status'] == 'checking', result
