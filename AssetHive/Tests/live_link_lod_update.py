"""Continue live_link_lods: change only artist LOD1, preserving LOD0 and rig."""
import copy,json,os
from pathlib import Path
import unreal
import assethive_suit_import as importer
import assethive_link_only as linker
assert Path(unreal.Paths.get_project_file_path()).stem=='CharacterLinkTest'
folder=Path(unreal.Paths.project_saved_dir())/'LinkLodAcceptance'
fixtures=Path(os.environ['ASSETHIVE_LOD_FIXTURES'])
job=json.loads((folder/'job.json').read_text(encoding='utf-8'))
proof=[]
for suit in job['suits']:
    entry=suit['entries'][0]
    prefix='SK' if entry['kind']=='skeletalMesh' else 'SM'
    obj=unreal.load_asset(entry['targetPath'])
    rig=obj.get_editor_property('skeleton') if prefix=='SK' else None
    entry['sourcePath']=str(fixtures/(prefix+'_WeaponCase_LodTest_LOD1.fbx'))
    entry['sourceHash']=linker.digest_file(entry['sourcePath'])
    before=importer.lod_policy_report(obj,entry)
    source=str(fixtures/(prefix+'_WeaponCase_LodTest_LOD1_updated.fbx'))
    entry['config']['lodSources']=[{'index':1,'sourcePath':source,'sourceHash':linker.digest_file(source)}]
    entry['fingerprint']=importer.digest({k:v for k,v in entry.items() if k!='fingerprint'})
    inspected=importer.inspect({'suits':[copy.deepcopy(suit)]})
    assert inspected['entries'][0]['status']=='ready' and inspected['entries'][0]['action']=='updated',inspected
    result=importer.apply_job({'suits':[copy.deepcopy(suit)],'inspectToken':inspected['inspectToken']})
    assert result['entries'][0]['status']=='success',result
    after=importer.lod_policy_report(obj,entry)
    assert unreal.load_asset(entry['targetPath'])==obj
    if rig:assert obj.get_editor_property('skeleton')==rig
    assert before['levels'][0]['triangles']==after['levels'][0]['triangles'],(before,after)
    assert before['levels'][1]['triangles']!=after['levels'][1]['triangles'],(before,after)
    assert len(after['levels'])==6
    repeated=importer.inspect({'suits':[copy.deepcopy(suit)]})
    assert repeated['entries'][0]['action']=='unchanged',repeated
    proof.append({'kind':entry['kind'],'before':before,'after':after,'sameObject':True,'lod0Preserved':True,'repeatUnchanged':True})
(folder/'lod1-update-proof.json').write_text(json.dumps(proof,indent=2),encoding='utf-8')
