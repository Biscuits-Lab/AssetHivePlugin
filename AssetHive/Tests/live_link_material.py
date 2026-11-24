"""Material/texture dependency linking, then in-place texture update."""
import copy
import json
import os
import struct
from pathlib import Path
import unreal
import assethive_link_only as linker
import assethive_suit_import as importer
assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
folder = Path(unreal.Paths.project_saved_dir()) / 'LinkMaterialAcceptance'
folder.mkdir(parents=True, exist_ok=True)
eal = unreal.EditorAssetLibrary
mel = unreal.MaterialEditingLibrary
if os.environ.get('ASSETHIVE_LINK_PHASE') != 'apply':
    source = folder / 'source.tga'
    source.write_bytes(struct.pack('<BBBHHBHHHHBB', 0,0,2,0,0,0,0,0,4,4,32,0x28) + bytes([48,96,192,255]) * 16)
    root = '/Game/Characters/Hero/Asset_PJZ/LinkMaterialProof'
    texture = {'key':'material-proof:texture:D', 'kind':'texture','name':'T_Proof','channel':'D','targetPath':root + '/T_Proof',
               'sourcePath':str(source),'sourceHash':linker.digest_file(source),'config':{'family':'suit','virtualTexture':False}}
    texture['fingerprint'] = importer.digest(texture)
    tex = importer.import_file(texture, texture['targetPath'])
    importer.apply_texture(tex, texture)
    assert eal.save_loaded_asset(tex, False)
    parent_path = '/Game/MaterialLibrary/Character/Common/M_Char_Face'
    parent = unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_Char_Face', '/Game/MaterialLibrary/Character/Common', unreal.Material, unreal.MaterialFactoryNew())
    param = mel.create_material_expression(parent, unreal.MaterialExpressionTextureSampleParameter2D, 0, 0)
    param.set_editor_property('parameter_name', 'Diffuse_VT')
    param.set_editor_property('texture', tex)
    mel.connect_material_property(param, 'RGB', unreal.MaterialProperty.MP_BASE_COLOR)
    mel.recompile_material(parent)
    assert eal.save_loaded_asset(parent, False)
    material = {'key':'material-proof:material:main','kind':'material','name':'MI_Proof','targetPath':root + '/MI_Proof',
                'textures':{'D':texture['key']},'config':{'family':'suit','heroPart':'Face','materialRole':'hero_face','parentPath':parent_path}}
    material['fingerprint'] = importer.digest(material)
    instance = unreal.AssetToolsHelpers.get_asset_tools().create_asset('MI_Proof', root, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    mel.set_material_instance_parent(instance, parent)
    mel.set_material_instance_texture_parameter_value(instance, 'Diffuse_VT', tex)
    assert eal.save_loaded_asset(instance, False)
    job = {'suits':[{'assetId':'material-proof','name':'MaterialProof','targetRoot':root,'entries':[texture,material]}]}
    (folder/'job.json').write_text(json.dumps(job), encoding='utf-8')
    result = linker.prepare(copy.deepcopy(job))
    (folder/'prepare.json').write_text(json.dumps(result,indent=2), encoding='utf-8')
    assert all(row['status']=='checking' for row in result['entries']), result
    wrong = copy.deepcopy(job)
    wrong['suits'][0]['entries'][1]['config']['parentPath'] = '/Game/DifferentParent'
    rejected = linker.prepare(wrong)
    assert rejected['entries'][1]['status'] == 'blocked', rejected
else:
    job = json.loads((folder/'job.json').read_text(encoding='utf-8'))
    checked = json.loads((folder/'prepare.json').read_text(encoding='utf-8'))
    verified = json.loads((folder/'verified.json').read_text(encoding='utf-8'))
    result = linker.apply(verified)
    assert all(row['status']=='success' for row in result['entries']), result
    for row in checked['entries']:
        assert linker.snapshot(eal.load_asset(row['path'])) == row['state']
    assert all(row['action']=='unchanged' for row in importer.inspect(copy.deepcopy(job))['entries'])
    texture, material = job['suits'][0]['entries']
    instance = eal.load_asset(material['targetPath'])
    before = linker.snapshot(instance)
    old_tex = eal.load_asset(texture['targetPath'])
    source = Path(texture['sourcePath'])
    source.write_bytes(source.read_bytes()[:18] + bytes([192,96,48,255]) * 16)
    texture['sourceHash'] = linker.digest_file(source)
    texture['fingerprint'] = importer.digest(texture)
    job['inspectToken'] = importer.inspect(copy.deepcopy(job))['inspectToken']
    updated = importer.apply_job(job)
    assert [row['status'] for row in updated['entries']] == ['success','unchanged'], updated
    assert eal.load_asset(texture['targetPath']) == old_tex
    assert mel.get_material_instance_texture_parameter_value(instance,'Diffuse_VT') == old_tex
    assert linker.snapshot(instance) == before
    assert all(row['action']=='unchanged' for row in importer.inspect(copy.deepcopy(job))['entries'])
    (folder/'proof.json').write_text(json.dumps({'linkedUnchanged':True,'wrongParentBlocked':True,'updatedSameTexture':True,'materialPreserved':True,'repeatUnchanged':True}), encoding='utf-8')
