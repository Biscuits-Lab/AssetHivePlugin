"""Run twice in isolated CharacterLinkTest: first generate, then with
ASSETHIVE_CLOTH_RELOAD=1. ASSETHIVE_CLOTH_JOB names a real suit export job.
The saved fixture must already have cloth at LOD0; no production assets touched.
"""
import json, os
from pathlib import Path
import unreal

assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
job = json.loads(Path(os.environ['ASSETHIVE_CLOTH_JOB']).read_text(encoding='utf-8-sig'))
entry = next(e for s in job['suits'] for e in s['entries']
             if e['kind'] == 'skeletalMesh' and e['name'].endswith('_Upbody'))
mesh = unreal.load_asset(entry['targetPath'])
source = entry['config']['skinLodSource']
def inspect(regenerate):
    result = json.loads(unreal.AssetHiveModelCatalogLibrary.complete_skinned_lods_from_static(
        mesh, source['sourcePath'], source['sourceHash'], source['sourceLod'], regenerate,
        json.dumps(source.get('sourceMaterials', {}))))
    assert not result.get('error'), result
    return result

before = inspect(False)
assert before['levels'][0]['clothSections'], 'Fixture must have authored cloth'
if not os.environ.get('ASSETHIVE_CLOTH_RELOAD'):
    result = inspect(True)
    assert result['levels'][0] == before['levels'][0], 'LOD0 changed'
    assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
result = inspect(False)
assert result['pending'] == [], result
for level in result['levels'][1:]:
    assert level['clothSections'], level
    for section in level['clothSections']:
        assert section['vertices'] > 0
        assert section['mappingCount'] >= section['vertices'], section
        assert section['restMappingMeanError'] < 0.1, section
print('CLOTH_SAVED_BINDINGS_PASS', 'reload' if os.environ.get('ASSETHIVE_CLOTH_RELOAD') else 'generate')
