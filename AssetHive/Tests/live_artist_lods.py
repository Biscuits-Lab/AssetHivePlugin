"""Verify artist SM LOD chains in a disposable UE commandlet project."""
import hashlib
import json
import os
from pathlib import Path
import unreal
import assethive_suit_import as importer

results = {}
try:
    for lod1 in sorted(Path(os.environ['ASSETHIVE_LOD_FIXTURES']).glob('*_LOD1.fbx')):
        source = lod1.with_name(lod1.name.replace('_LOD1', ''))
        task = unreal.AssetImportTask()
        task.filename = str(source)
        task.destination_path = '/Game/ArtistLodTest'
        task.destination_name = source.stem
        task.automated = True
        options = unreal.FbxImportUI()
        options.automated_import_should_detect_type = False
        options.mesh_type_to_import = unreal.FBXImportType.FBXIT_STATIC_MESH
        options.import_materials = False
        options.import_textures = False
        options.static_mesh_import_data.combine_meshes = True
        options.static_mesh_import_data.auto_generate_collision = False
        task.options = options
        task.factory = unreal.FbxFactory()
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        mesh = next(obj for obj in task.get_objects() if isinstance(obj, unreal.StaticMesh))
        entry = {'kind': 'staticMesh', 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
            {'index': 1, 'sourcePath': str(lod1), 'sourceHash': hashlib.sha256(lod1.read_bytes()).hexdigest()}]}}
        results[source.stem] = importer.apply_model_lods(mesh, entry)
        assert results[source.stem]['count'] == 6
        results[source.stem]['sameFileHash'] = hashlib.sha256(source.read_bytes()).hexdigest() == hashlib.sha256(lod1.read_bytes()).hexdigest()
    assert results, 'No artist LOD files found'
    results['ok'] = True
except Exception as error:
    results['error'] = str(error)
    if 'mesh' in globals(): results['failed_report'] = importer.lod_policy_report(mesh, entry)
    raise
finally:
    Path(os.environ['ASSETHIVE_LOD_RESULT']).write_text(json.dumps(results, indent=2), encoding='utf-8')
