"""Run only in the disposable LodTest project; creates its own test materials."""
import json
from pathlib import Path
import unreal
import assethive_suit_import as importer

assert Path(unreal.Paths.get_project_file_path()).stem == 'LodTest'
result = {}
output = Path(unreal.Paths.project_dir()) / 'suit-path-result.json'
try:
    root = '/Game/Characters/Hero/Asset_PJZ/Male_01/Mesh/M01_Heavy999_SuitPathTest/Materials'
    tools = unreal.AssetToolsHelpers.get_asset_tools()
    parent_path = '/Game/MaterialLibrary/Character/Common/M_Char_Face'
    parent = importer.load(parent_path) or tools.create_asset('M_Char_Face', '/Game/MaterialLibrary/Character/Common', unreal.Material, unreal.MaterialFactoryNew())
    source = root + '/MI_M01_Heavy999_SuitPathTest_head'
    target = root + '/MI_M01_Heavy999_SuitPathTest_Head'
    obj = importer.load(source) or tools.create_asset(source.rsplit('/', 1)[1], root, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    unreal.MaterialEditingLibrary.set_material_instance_parent(obj, parent)
    unreal.EditorAssetLibrary.remove_metadata_tag(obj, importer.KEY)
    unreal.EditorAssetLibrary.remove_metadata_tag(obj, importer.STAMP)
    dependent = importer.load(root + '/MI_Dependent') or tools.create_asset('MI_Dependent', root, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    unreal.MaterialEditingLibrary.set_material_instance_parent(dependent, obj)
    for material in [parent, obj, dependent]:
        assert unreal.EditorAssetLibrary.save_loaded_asset(material, False)
    key = 'suit-path-regression:material:head'
    entry = {'key': key, 'name': target.rsplit('/', 1)[1], 'kind': 'material', 'targetPath': target,
             'fingerprint': 'case-only-regression-v1', 'textures': {},
             'config': {'family': 'suit', 'materialRole': 'hero_face', 'parentPath': parent_path}}
    suit = {'assetId': 'suit-path-regression', 'name': 'SuitPathTest', 'family': 'suit', 'targetRoot': root, 'entries': [entry]}
    job = {'suits': [suit]}
    checked = importer.inspect(job)
    assert checked['entries'][0]['action'] == 'adopt', checked
    job.update(inspectToken=checked['inspectToken'], decisions={key: 'adopt'})
    applied = importer.apply_job(job)
    assert applied['entries'][0]['status'] == 'success', applied
    assert importer.load(target) == obj
    assert dependent.get_editor_property('parent') == obj
    assert str(unreal.EditorAssetLibrary.get_metadata_tag(obj, importer.KEY)) == key
    result['caseOnlyAdoption'] = applied['entries'][0]
    original_registry = importer.read_registry
    importer.read_registry = lambda: {}
    status_job = {'suits': [{'assetId': suit['assetId'], 'entries': [
        {'key': key, 'path': source}, {'key': 'suit-path-regression:staticMesh:deleted', 'path': root + '/SM_Deleted'}]}]}
    status = importer.suit_status(status_job)
    assert status['entries'][0]['present'] is True, status
    assert status['entries'][1]['present'] is False, status
    result['withoutLocalRegistry'] = status
    unreal.EditorAssetLibrary.remove_metadata_tag(obj, importer.KEY)
    pending = importer.suit_status(status_job)['entries'][0]
    assert pending['associationPending'] and pending['present'], pending
    result['unassociatedExisting'] = pending
    importer.read_registry = original_registry
    result['ok'] = True
finally:
    output.write_text(json.dumps(result, indent=2), encoding='utf-8')
