"""Run with UnrealEditor-Cmd PythonScript in an isolated, disposable project."""
import hashlib
import json
import os
from pathlib import Path
import unreal
import assethive_suit_import as importer

root = Path(os.environ['ASSETHIVE_LOD_FIXTURES'])
results = {}
try:
    for prefix, kind in [('SM', 'staticMesh'), ('SK', 'skeletalMesh')]:
        source = root / '01-initial/GamePlay_WeaponCase_LodTest' / (prefix + '_WeaponCase_LodTest_LOD0.fbx')
        task = unreal.AssetImportTask()
        task.filename = str(source)
        task.destination_path = '/Game/LodV2Test'
        task.destination_name = prefix + '_LodV2'
        task.automated = True
        task.replace_existing = True
        options = unreal.FbxImportUI()
        options.automated_import_should_detect_type = False
        options.import_as_skeletal = prefix == 'SK'
        options.mesh_type_to_import = unreal.FBXImportType.FBXIT_SKELETAL_MESH if prefix == 'SK' else unreal.FBXImportType.FBXIT_STATIC_MESH
        options.import_materials = False
        options.import_textures = False
        options.create_physics_asset = False
        if prefix == 'SM':
            options.static_mesh_import_data.combine_meshes = True
            options.static_mesh_import_data.auto_generate_collision = False
        task.options = options
        task.factory = unreal.FbxFactory()
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        mesh = next(obj for obj in task.get_objects() if isinstance(obj, unreal.SkeletalMesh if prefix == 'SK' else unreal.StaticMesh))
        lod1 = source.with_name(prefix + '_WeaponCase_LodTest_LOD1.fbx')
        entry = {'kind': kind, 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
            {'index': 1, 'sourcePath': str(lod1), 'sourceHash': hashlib.sha256(lod1.read_bytes()).hexdigest()}]}}
        results[prefix] = importer.apply_model_lods(mesh, entry)
        assert results[prefix]['count'] == 6, results[prefix]
        results[prefix + '_recheck'] = importer.lod_policy_report(mesh, entry)
        importer.verify_lod_report(entry, results[prefix + '_recheck'])
        assert not results[prefix + '_recheck']['pending'], results[prefix + '_recheck']
        lod0_triangles = results[prefix]['levels'][0]['triangles']
        skeleton = mesh.get_editor_property('skeleton') if prefix == 'SK' else None
        updated = root / '03-update-LOD1' / (prefix + '_WeaponCase_LodTest_LOD1.fbx')
        updated_entry = {'kind': kind, 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
            {'index': 1, 'sourcePath': str(updated), 'sourceHash': hashlib.sha256(updated.read_bytes()).hexdigest()}]}}
        results[prefix + '_updated'] = importer.apply_model_lods(mesh, updated_entry, entry)
        assert results[prefix + '_updated']['levels'][0]['triangles'] == lod0_triangles
        if prefix == 'SK': assert mesh.get_editor_property('skeleton') == skeleton
        lod2 = root / '02-add-LOD2' / (prefix + '_WeaponCase_LodTest_LOD2.fbx')
        updated_entry['config']['lodSources'].append({'index': 2, 'sourcePath': str(lod2), 'sourceHash': hashlib.sha256(lod2.read_bytes()).hexdigest()})
        results[prefix + '_manual2'] = importer.apply_model_lods(mesh, updated_entry)
        assert results[prefix + '_manual2']['levels'][2]['source'] == 'imported'
        manual2_triangles = results[prefix + '_manual2']['levels'][2]['triangles']
        # Removing a source from this send must preserve the existing artist LOD in UE.
        updated_entry['config']['lodSources'].pop()
        results[prefix + '_preserved2'] = importer.apply_model_lods(mesh, updated_entry)
        assert results[prefix + '_preserved2']['levels'][2]['source'] == 'existing'
        assert results[prefix + '_preserved2']['levels'][2]['triangles'] == manual2_triangles
    results['ok'] = True
except Exception as error:
    results['error'] = str(error)
    if 'mesh' in globals():
        results['failed_report'] = importer.lod_policy_report(mesh, entry)
    raise
finally:
    Path(os.environ['ASSETHIVE_LOD_RESULT']).write_text(json.dumps(results, indent=2), encoding='utf-8')
