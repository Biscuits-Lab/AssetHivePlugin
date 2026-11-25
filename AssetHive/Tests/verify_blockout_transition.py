"""Run in isolated CharacterLinkTest with -ForceDependsGathering and
ASSETHIVE_BLOCKOUT_TEST_JOB pointing to a BlockoutReview SM manifest."""
import sys,json,os
from pathlib import Path
import unreal
assert Path(unreal.Paths.get_project_file_path()).stem=='CharacterLinkTest'
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'Content' / 'Python'))
import assethive_suit_import as api
import assethive_blockout as bo
job=json.loads(Path(os.environ['ASSETHIVE_BLOCKOUT_TEST_JOB']).read_text())
assert all(s['targetRoot'].startswith('/Game/Characters/Hero/Asset_PJZ/BlockoutReview/') for s in job['suits'])
records={};api.read_registry=lambda:dict(records)
api.write_registry=lambda v:(records.clear(),records.update(v))
r=api.inspect(job);assert all(e['status'] in ('ready','unchanged') for e in r['entries']),r
job['inspectToken']=r['inspectToken'];r=api.apply_job(job)
assert all(e['status'] in ('success','unchanged') for e in r['entries']),r
count=0
for s in job['suits']:
 for e in s['entries']:
  obj=api.load(e['targetPath']);assert obj,e
  if e['kind']=='staticMesh':
   for slot in obj.get_editor_property('static_materials'):
    name=api.slot_name(slot);assert api.receipt(slot.material_interface)['key']==e['materials'][name],(e['name'],name)
    count+=1
  else:
   actual=bo.values(obj);expected=e['config']['blockoutMaterial']
   assert all(abs(a-b)<0.00001 for a,b in zip(actual['color'],expected['color'])),(actual,expected)
r=api.inspect(job);assert all(e['action']=='unchanged' for e in r['entries']),r
print('REAL_BLOCKOUT_OK slots='+str(count)+' entries='+str(len(r['entries'])))
import copy
old_materials=[e['targetPath'] for s in job['suits'] for e in s['entries'] if e['kind']=='material']
mesh_entry=next(e for s in job['suits'] for e in s['entries'] if e['kind']=='staticMesh')
shared_path=mesh_entry['targetPath']+'_SharedReferenceTest'
unreal.EditorAssetLibrary.delete_asset(shared_path) if unreal.EditorAssetLibrary.does_asset_exist(shared_path) else None
shared=unreal.EditorAssetLibrary.duplicate_asset(mesh_entry['targetPath'],shared_path)
assert shared
unreal.EditorAssetLibrary.remove_metadata_tag(shared,api.KEY)
unreal.EditorAssetLibrary.remove_metadata_tag(shared,api.STAMP)
unreal.EditorAssetLibrary.save_loaded_asset(shared)
kept={slot.material_interface.get_path_name() for slot in shared.get_editor_property('static_materials')}
formal=copy.deepcopy(job)
for s in formal['suits']:
 s['entries']=[e for e in s['entries'] if e['kind']=='staticMesh']
 for e in s['entries']:
  e['config'].pop('materialMode',None)
  e['config'].pop('blockoutFallback',None)
  e['externalMaterials']={n:'/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial' for n in e['materials']}
  e['materials']={}
  e['fingerprint']='formal-transition-'+e['key']
r=api.inspect(formal)
assert all(e['status'] in ('ready','unchanged') for e in r['entries']),r
formal['inspectToken']=r['inspectToken']
r=api.apply_job(formal)
assert all(e['status'] in ('success','unchanged') for e in r['entries']),r
for s in formal['suits']:
 for e in s['entries']:
  for slot in api.load(e['targetPath']).get_editor_property('static_materials'):
   assert slot.material_interface.get_path_name()=='/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial'
for p in old_materials:
 assert unreal.EditorAssetLibrary.does_asset_exist(p), p
assert unreal.EditorAssetLibrary.delete_asset(shared_path)
r=api.inspect(formal);formal['inspectToken']=r['inspectToken'];r=api.apply_job(formal)
assert all(unreal.EditorAssetLibrary.does_asset_exist(p) for p in old_materials),r
assert all(e['action']=='unchanged' for e in api.inspect(formal)['entries'])
print('BLOCKOUT_TO_FORMAL_PASS binding replaced, old materials retained for manual cleanup, repeat noop')
