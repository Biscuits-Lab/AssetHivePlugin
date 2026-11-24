import unreal, json, hashlib, copy, os
from pathlib import Path
import assethive_suit_import as importer
import assethive_link_only as linker
root=Path(os.environ['ASSETHIVE_SKIN_TEST_ROOT']).resolve()
assert Path(unreal.Paths.get_project_file_path()).stem=='CharacterLinkTest'
base=root/'skin-transfer-fixtures'
if os.environ.get('ASSETHIVE_SKIN_RELOAD')=='1':
 job=json.loads((root/'skin-transfer-job.json').read_text(encoding='utf-8'))
 importer.registry_path=lambda:str(root/'absent-skin-transfer-registry.json')
 checked=importer.inspect(copy.deepcopy(job))
 assert checked['entries'][0]['action']=='unchanged',checked
 assert linker.prepare(copy.deepcopy(job))['entries'][0]['alreadyLinked']
 (root/'skin-transfer-reload-proof.json').write_text(json.dumps(checked,indent=2),encoding='utf-8')
 print('STATIC_SKIN_RELOAD_COMPLETE')
 raise SystemExit(0)
entry={'kind':'skeletalMesh','key':'skinDerived:skeletalMesh:main','name':'SK_SkinDerived','targetPath':'/Game/Characters/Monster/Acceptance/SK_SkinDerived','sourcePath':str(base/'SK_Base.fbx'),'skeletonPath':'/Game/Characters/Monster/Acceptance/SKEL_SkinDerived','config':{'family':'monster','ownSkeleton':True,'createPhysicsAsset':False}}
mesh=importer.import_file(entry,entry['targetPath'])
skeleton=mesh.get_editor_property('skeleton')
slots=mesh.materials
for i,slot in enumerate(slots):
 slot.material_interface=unreal.load_asset('/Engine/EngineMaterials/WorldGridMaterial')
 slots[i]=slot
mesh.materials=slots
entry['config'].update(lodPolicy='lod1-reduction-v2',skinLodSource={'version':1,'sourceLod':0,'sourcePath':str(base/'SM_LOD1.fbx'),'sourceHash':hashlib.sha256((base/'SM_LOD1.fbx').read_bytes()).hexdigest()})
before=importer.lod_policy_report(mesh,entry)
assert before['pending'] in ([],[1,2,3,4,5]),before
report=importer.apply_model_lods(mesh,entry)
assert report['count']==6 and not report['pending'],report
assert report['levels'][0]['triangles']==before['levels'][0]['triangles'],report
assert report['rigHash']==before['rigHash'],report
assert mesh.get_editor_property('skeleton')==skeleton
assert not importer.lod_policy_report(mesh,entry)['pending']
unreal.EditorAssetLibrary.save_loaded_asset(mesh)
entry.update(sourceHash=hashlib.sha256((base/'SK_Base.fbx').read_bytes()).hexdigest(),materials={},
 externalMaterials={importer.slot_name(slot):slot.material_interface.get_path_name() for slot in mesh.materials},
 bones=json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(skeleton)),
 targetBones=json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(skeleton)))
entry['config']['ownSkeleton']=False
entry['skeletonPath']=skeleton.get_path_name()
entry['fingerprint']=importer.digest({k:v for k,v in entry.items() if k!='fingerprint'})
importer.save(mesh,entry)
job={'suits':[{'assetId':'skinDerived','name':'SkinDerived','family':'monster','entries':[entry]}]}
checked=importer.inspect(copy.deepcopy(job))
assert checked['entries'][0]['action']=='unchanged',checked
source=entry['config']['skinLodSource']
source.update(sourcePath=str(base/'SM_LOD1_Updated.fbx'),sourceHash=hashlib.sha256((base/'SM_LOD1_Updated.fbx').read_bytes()).hexdigest())
entry['fingerprint']=importer.digest({k:v for k,v in entry.items() if k!='fingerprint'})
checked=importer.inspect(copy.deepcopy(job))
assert checked['entries'][0]['status']=='ready' and checked['entries'][0]['action']=='updated',checked
def forbid_base_import(*args):raise AssertionError('LOD-only change reimported LOD0')
importer.import_file=forbid_base_import
sent=importer.apply_job({**copy.deepcopy(job),'inspectToken':checked['inspectToken']})
assert sent['entries'][0]['status']=='success',sent
assert unreal.load_asset(entry['targetPath'])==mesh and mesh.get_editor_property('skeleton')==skeleton
again=importer.inspect(copy.deepcopy(job))
assert again['entries'][0]['action']=='unchanged',again
wrong=copy.deepcopy(entry)
wrong['config']['skinLodSource'].update(sourcePath=str(base/'SM_WrongPart.fbx'),sourceHash=hashlib.sha256((base/'SM_WrongPart.fbx').read_bytes()).hexdigest())
state=linker.snapshot(mesh)
try:
 importer.apply_model_lods(mesh,wrong,entry)
 raise AssertionError('Wrong part was accepted')
except RuntimeError as error:
 assert 'not aligned' in str(error),error
assert linker.snapshot(mesh)==state
(root/'skin-transfer-job.json').write_text(json.dumps(job,indent=2),encoding='utf-8')
(root/'skin-transfer-update-proof.json').write_text(json.dumps({'sent':sent,'repeatUnchanged':True,'baseNotReimported':True,'wrongPartRejectedWithoutChange':True},indent=2),encoding='utf-8')
(root/'static-skin-lods-proof.json').write_text(json.dumps({'before':before,'after':report,'skeletonUnchanged':True,'repeatPending':False},indent=2),encoding='utf-8')
print('STATIC_SKIN_LODS_COMPLETE')
