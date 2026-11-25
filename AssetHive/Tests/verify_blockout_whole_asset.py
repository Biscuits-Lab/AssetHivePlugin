"""Isolated complete SM/SK Blockout import, colors, no-op and readonly catalog regression."""
import sys,json,os,stat
from pathlib import Path
import unreal
assert Path(unreal.Paths.get_project_file_path()).stem=='CharacterLinkTest'
sys.path.insert(0,str(Path(__file__).resolve().parents[1] / 'Content/Python'))
import assethive_suit_import as api
import assethive_blockout as bo
job=json.loads(Path(os.environ['ASSETHIVE_BLOCKOUT_TEST_JOB']).read_text(encoding='utf-8'))
assert all('/BlockoutReview/' in s['targetRoot'] for s in job['suits'])
for s in job['suits']:
 for e in s['entries']:
  if e['kind']=='skeletalMesh':
   e['targetBones']=json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(unreal.load_asset(e['skeletonPath'])))
records={};api.read_registry=lambda:dict(records)
api.write_registry=lambda v:(records.clear(),records.update(v))
p=Path(unreal.Paths.project_content_dir())/'Characters/Data/DA_ModelPolycountLevels.uasset'
mode=p.stat().st_mode
try:
 os.chmod(p,stat.S_IREAD)
 r=api.inspect(job)
 assert all(e['status'] in ('ready','unchanged') for e in r['entries']),r
 job['inspectToken']=r['inspectToken'];r=api.apply_job(job)
 assert all(e['status'] in ('success','unchanged') for e in r['entries']),r
 count=0
 for s in job['suits']:
  for e in s['entries']:
   obj=api.load(e['targetPath']);assert obj,e
   if e['kind'] in ('staticMesh','skeletalMesh'):
    slots=obj.get_editor_property('static_materials' if e['kind']=='staticMesh' else 'materials')
    for slot in slots:
     name=api.slot_name(slot);assert api.receipt(slot.material_interface)['key']==e['materials'][name],(e['name'],name)
     count+=1
   else:
    assert isinstance(obj,unreal.MaterialInstanceConstant),e
    actual=bo.values(obj);expected=e['config']['blockoutMaterial']
    assert actual['parent']==bo.parent().get_path_name(),actual
    assert all(abs(a-b)<0.00001 for a,b in zip(actual['color'],expected['color'])),(actual,expected)
 r=api.inspect(job)
 assert all(e['action']=='unchanged' for e in r['entries']),r
 print('SHAMAN_WHOLE_BLOCKOUT_PASS slots='+str(count)+' entries='+str(len(r['entries'])))
finally: os.chmod(p,mode)
