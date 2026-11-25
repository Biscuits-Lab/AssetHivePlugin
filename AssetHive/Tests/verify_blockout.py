"""Isolated UE regression for multi-material blockout send, repeat and color update."""
import sys, json, os
from pathlib import Path
import unreal
assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest', 'Use an isolated CharacterLinkTest project only'
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'Content' / 'Python'))
import assethive_suit_import as api
job=json.loads(Path(os.environ['ASSETHIVE_BLOCKOUT_TEST_JOB']).read_text())
assert job['suits'][0]['targetRoot'] == '/Game/Characters/Monster/1_Boss/Boss_BlockoutFixture'
unreal.EditorAssetLibrary.delete_directory(job['suits'][0]['targetRoot'])
records={}
api.read_registry=lambda: dict(records)
api.write_registry=lambda values: (records.clear(), records.update(values))
result=api.inspect(job)
assert all(row['status']=='ready' for row in result['entries']),result
job['inspectToken']=result['inspectToken']
result=api.apply_job(job)
assert all(row['status'] in ('success','unchanged') for row in result['entries']),result
entries=job['suits'][0]['entries']
for entry in entries:
    obj=unreal.EditorAssetLibrary.load_asset(entry['targetPath'])
    assert obj
    if entry['kind'] in ('staticMesh','skeletalMesh'):
        slots=obj.get_editor_property('static_materials' if entry['kind']=='staticMesh' else 'materials')
        assert len(slots)==3,slots
        for slot in slots:
            name=api.slot_name(slot)
            assert api.receipt(slot.material_interface)['key']==entry['materials'][name],name
        if entry['kind']=='skeletalMesh':
            entry['targetBones']=json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(api.load(entry['skeletonPath'])))
    else:
        assert isinstance(obj,unreal.Material)
        import assethive_blockout
        assert assethive_blockout.values(obj)['color']==entry['config']['blockoutMaterial']['color']
        assert assethive_blockout.values(obj)['emissive']==[0,0,0]
result=api.inspect(job)
assert all(row['action']=='unchanged' and row['status']=='ready' for row in result['entries']),result
changed=entries[0]
changed['config']['blockoutMaterial']['color']=[0,0,1]
changed['sourceHash']='changed-color'
changed['fingerprint']='changed-color'
result=api.inspect(job)
assert [r['key'] for r in result['entries'] if r['action']!='unchanged']==[changed['key']],result
job['inspectToken']=result['inspectToken']
result=api.apply_job(job)
assert all(row['status'] in ('success','unchanged') for row in result['entries']),result
assert all(row['action']=='unchanged' for row in api.inspect(job)['entries'])
assert assethive_blockout.values(api.load(changed['targetPath']))['color']==[0,0,1]
print('BLOCKOUT_UE_OK SM+SK 3 slots each; repeat no-op; color-only updates one material')
