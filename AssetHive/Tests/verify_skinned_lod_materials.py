"""Run in isolated CharacterLinkTest with GeometryScripting enabled.
ASSETHIVE_SKIN_LOD_JOB supplies a fixture job whose source files and SM/SK assets exist.
Only in-memory isolated SK assets are modified, never saved; compare every rendered triangle's material.
"""
import collections
import json
import os
from pathlib import Path
import unreal

assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
job = json.loads(Path(os.environ['ASSETHIVE_SKIN_LOD_JOB']).read_text(encoding='utf-8-sig'))
api = unreal.AssetHiveModelCatalogLibrary

def signature(model, level, skeletal):
    mesh = unreal.DynamicMesh()
    fn = (unreal.GeometryScript_AssetUtils.copy_mesh_from_skeletal_mesh if skeletal
          else unreal.GeometryScript_AssetUtils.copy_mesh_from_static_mesh)
    _, outcome = fn(model, mesh, unreal.GeometryScriptCopyMeshFromAssetOptions(),
                    unreal.GeometryScriptMeshReadLOD(lod_index=level, lod_type=unreal.GeometryScriptLODType.RENDER_DATA))
    assert outcome == unreal.GeometryScriptOutcomePins.SUCCESS
    points = unreal.GeometryScript_List.convert_vector_list_to_array(
        unreal.GeometryScript_MeshQueries.get_all_vertex_positions(mesh, False)[1])
    triangles = unreal.GeometryScript_List.convert_triangle_list_to_array(
        unreal.GeometryScript_MeshQueries.get_all_triangle_indices(mesh, False)[1])
    if skeletal:
        materials = [slot.material_interface for slot in model.get_editor_property('materials')]
    else:
        # Static conversion emits section IDs, not the asset's material-slot IDs.
        materials = list(unreal.GeometryScript_AssetUtils.get_section_material_list_from_static_mesh(
            model, unreal.GeometryScriptMeshReadLOD(lod_index=level, lod_type=unreal.GeometryScriptLODType.RENDER_DATA))[0])
    result = collections.Counter()
    for i, triangle in enumerate(triangles):
        material_index, valid = unreal.GeometryScript_Materials.get_triangle_material_id(mesh, i)
        assert valid and 0 <= material_index < len(materials)
        material = materials[material_index]
        corners = tuple(sorted(tuple(round(v, 3) for v in (points[j].x, points[j].y, points[j].z))
                               for j in (triangle.x, triangle.y, triangle.z)))
        result[(corners, material.get_path_name() if material else '')] += 1
    return result

def geometry(signature):
    result = collections.Counter()
    for (triangle, material), count in signature.items():
        result[triangle] += count
    return result

results = []
for suit in job['suits']:
    for entry in suit['entries']:
        source = entry.get('config', {}).get('skinLodSource')
        if not source:
            continue
        original = unreal.load_asset(entry['targetPath'])
        static = unreal.load_asset(entry['targetPath'].replace('/SkeletalMesh/', '/StaticMesh/').replace('/SK_', '/SM_').replace('.SK_', '.SM_'))
        assert original and static
        before_base = signature(original, 0, True)
        before_geometry = {level: geometry(signature(original, level, True)) for level in range(1, 6)}
        copy = original
        args = (source['sourcePath'], source['sourceHash'], source['sourceLod'])
        materials_json = json.dumps(source.get('sourceMaterials', {}), sort_keys=True)
        before = json.loads(api.complete_skinned_lods_from_static(copy, *args, False, materials_json))
        report = json.loads(api.complete_skinned_lods_from_static(copy, *args, True, materials_json))
        assert not report.get('error') and not report.get('pending') and report['count'] == 6, report
        assert report['rigHash'] == before['rigHash']
        base_cloth = before['levels'][0].get('clothSections', [])
        assert report['levels'][0].get('clothSections', []) == base_cloth
        for level in report['levels'][1:]:
            cloth = level.get('clothSections', [])
            assert {(r['assetGuid'], r['materialIndex']) for r in cloth} == {(r['assetGuid'], r['materialIndex']) for r in base_cloth}
            assert all(r['vertices'] > 0 and r['mappingCount'] >= r['vertices'] and r['mappingCount'] % r['vertices'] == 0 for r in cloth)

        assert signature(copy, 0, True) == before_base, 'LOD0 changed'
        for level in range(1, 6):
            actual = signature(copy, level, True)
            expected = signature(static, level, False)
            # DynamicMesh conversion can drop different non-manifold triangles for
            # SM and SK. Preserve the exact existing SK geometry; compare material
            # assignments on every shared triangle. The native generation check
            # separately compares ALL raw rendered triangles without this conversion.
            assert geometry(actual) == before_geometry[level], (entry['name'], level, 'geometry changed')
            shared = geometry(actual) & geometry(expected)
            assignments = []
            for signature_value in (actual, expected):
                by_triangle = collections.defaultdict(collections.Counter)
                for (triangle, material), count in signature_value.items():
                    by_triangle[triangle][material] += count
                assignments.append(by_triangle)
            for triangle in shared:
                a, b = (mapping[triangle] for mapping in assignments)
                assert set(a) == set(b) and (a == b or sum(a.values()) != sum(b.values())), (entry['name'], level, triangle, a, b)
        repeat = json.loads(api.complete_skinned_lods_from_static(copy, *args, False, materials_json))
        assert not repeat.get('pending') and not repeat.get('error'), repeat
        # The isolated model is deliberately left unsaved.
        results.append({'name': entry['name'], 'levelsVerified': [1,2,3,4,5], 'lod0Unchanged': True, 'repeatUnchanged': True})
        print('SKIN_MATERIAL_VERIFIED', entry['name'])
assert results
print('SKIN_MATERIAL_REGRESSION_OK', json.dumps(results))
