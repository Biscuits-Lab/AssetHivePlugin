"""Read-only association of real imported SM/SK artist LODs in a disposable project."""
import json
import os
import copy
from pathlib import Path
import unreal
import assethive_suit_import as importer
import assethive_link_only as linker
assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
folder = Path(unreal.Paths.project_saved_dir()) / 'LinkLodAcceptance'
folder.mkdir(parents=True, exist_ok=True)
fixtures = Path(os.environ['ASSETHIVE_LOD_FIXTURES'])
if os.environ.get('ASSETHIVE_LINK_PHASE') == 'apply':
    job=json.loads((folder/'job.json').read_text(encoding='utf-8'))
    prepared=json.loads((folder/'prepare.json').read_text(encoding='utf-8'))
    proof=json.loads((folder/'verified.json').read_text(encoding='utf-8'))
    result=linker.apply(proof)
    assert all(row['status']=='success' for row in result['entries']),result
    for row in prepared['entries']:
        obj=unreal.load_asset(row['path'])
        assert linker.snapshot(obj)==row['state']
        assert linker.dependencies(obj)==row['dependencies']
    results=[]
    for suit in job['suits']:
        entry=suit['entries'][0]
        obj=unreal.load_asset(entry['targetPath'])
        skeleton=obj.get_editor_property('skeleton') if entry['kind']=='skeletalMesh' else None
        inspected=importer.inspect({'suits':[copy.deepcopy(suit)]})
        assert inspected['entries'][0]['status']=='ready',inspected
        # Explicit Send can now complete the reduction policy; linking did not.
        sent=importer.apply_job({'suits':[copy.deepcopy(suit)],'inspectToken':inspected['inspectToken']})
        assert sent['entries'][0]['status'] in ('success','unchanged'),sent
        prefix='SK' if skeleton else 'SM'
        entry['sourcePath']=str(fixtures/(prefix+'_WeaponCase_LodTest_LOD1.fbx'))
        entry['sourceHash']=linker.digest_file(entry['sourcePath'])
        entry['fingerprint']=importer.digest({k:v for k,v in entry.items() if k!='fingerprint'})
        inspected=importer.inspect({'suits':[copy.deepcopy(suit)]})
        assert inspected['entries'][0]['action']=='updated' and inspected['entries'][0]['status']=='ready',inspected
        sent=importer.apply_job({'suits':[copy.deepcopy(suit)],'inspectToken':inspected['inspectToken']})
        assert sent['entries'][0]['status']=='success',sent
        assert unreal.load_asset(entry['targetPath'])==obj
        if skeleton:assert obj.get_editor_property('skeleton')==skeleton
        subsystem_type=unreal.SkeletalMeshEditorSubsystem if skeleton else unreal.StaticMeshEditorSubsystem
        subsystem=unreal.get_editor_subsystem(subsystem_type) or unreal.get_default_object(subsystem_type)
        assert subsystem.get_lod_count(obj)==6
        repeated=importer.inspect({'suits':[copy.deepcopy(suit)]})
        assert repeated['entries'][0]['action']=='unchanged',repeated
        results.append({'kind':entry['kind'],'linkUnchanged':True,'updateSameObject':True,'skeletonRetained':bool(skeleton),'lodCount':6,'repeatUnchanged':True})
    (folder/'proof.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
    raise SystemExit(0)
suits = []
for prefix, kind in [('SM', 'staticMesh'), ('SK', 'skeletalMesh')]:
    source = fixtures / (prefix + '_WeaponCase_LodTest_LOD0.fbx')
    lod = fixtures / (prefix + '_WeaponCase_LodTest_LOD1.fbx')
    task = unreal.AssetImportTask()
    task.filename = str(source)
    task.destination_path = '/Game/Characters/Monster/LinkLodProofV3'
    task.destination_name = prefix + '_Existing'
    task.automated = True
    task.factory = unreal.FbxFactory()
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
    target = task.destination_path + '/' + task.destination_name
    if unreal.EditorAssetLibrary.does_asset_exist(target):
        model = unreal.EditorAssetLibrary.load_asset(target)
    else:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
        model = next(obj for obj in task.get_objects() if isinstance(obj, unreal.SkeletalMesh if prefix == 'SK' else unreal.StaticMesh))
    entry = {'key':'lodProofV3' + prefix + ':mesh:main','name':model.get_name(),'kind':kind,'targetPath':target,'materials':{},'externalMaterials':{},
             'sourcePath':str(source),'sourceHash':linker.digest_file(source),'config':{'lodSources':[
                 {'index':1,'sourcePath':str(lod),'sourceHash':linker.digest_file(lod)}]}}
    importer.apply_model_lods(model, entry)
    if prefix == 'SK':
        assert unreal.EditorAssetLibrary.save_loaded_asset(model.get_editor_property('skeleton'), False)
    assert unreal.EditorAssetLibrary.save_loaded_asset(model, False)
    entry['config'].update(family='monster',lodPolicy='lod1-reduction-v2')
    if prefix=='SK':
        entry['config']['ownSkeleton']=True
        entry['skeletonPath']='/Game/Characters/Monster/LinkLodProofV3/SKEL_Canonical'
        entry['bones']=json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(model.get_editor_property('skeleton')))
    entry['fingerprint']=importer.digest(entry)
    suits.append({'assetId':prefix,'entries':[entry]})
job={'suits':suits}
(folder/'job.json').write_text(json.dumps(job,indent=2),encoding='utf-8')
result = linker.prepare(job)
(folder/'prepare.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
assert all(row['status'] == 'checking' for row in result['entries']), result
