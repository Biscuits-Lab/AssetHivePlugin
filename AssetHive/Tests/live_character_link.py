"""Disposable real-editor acceptance: link, no-op, update, add, no-op.

The desktop must compare prepare.json and write verified.json between phases.
Never run in a production project.
"""
import copy
import json
import os
from pathlib import Path
import unreal
import assethive_link_only as linker
import assethive_suit_import as importer

assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
folder = Path(unreal.Paths.project_saved_dir()) / 'CharacterLinkAcceptance'
folder.mkdir(parents=True, exist_ok=True)
eal = unreal.EditorAssetLibrary


def write(name, value):
    (folder / name).write_text(json.dumps(value, indent=2), encoding='utf-8')


def send(job):
    job['inspectToken'] = importer.inspect(copy.deepcopy(job))['inspectToken']
    return importer.apply_job(job)


def export(mesh, file, lods=False):
    task = unreal.AssetExportTask()
    task.object = mesh
    task.filename = str(file)
    task.automated = True
    task.prompt = False
    task.replace_identical = True
    task.exporter = unreal.StaticMeshExporterFBX()
    task.options = unreal.FbxExportOption()
    task.options.ascii = False
    task.options.level_of_detail = lods
    assert unreal.Exporter.run_asset_export_task(task)


if os.environ.get('ASSETHIVE_LINK_PHASE') != 'apply':
    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset('M_LinkProof', '/Game/LinkProof', unreal.Material, unreal.MaterialFactoryNew())
    assert material and eal.save_loaded_asset(material, False)
    roots = {'suit': '/Game/Characters/Hero/Asset_PJZ/LinkProofSuit',
             'face': '/Game/Characters/Hero/Asset_PJZ/LinkProofFace',
             'hair': '/Game/Characters/Hero/Asset_PJZ/LinkProofHair',
             'monster': '/Game/Characters/Monster/LinkProofMonster',
             'npc': '/Game/Characters/NPC/LinkProofNPC',
             'weapon': '/Game/Characters/Weapon_PJZ/LinkProofWeapon',
             'item': '/Game/Characters/GameplayItem/LinkProofItem'}
    suits = []
    for category, root in roots.items():
        mesh = eal.duplicate_asset('/Engine/BasicShapes/Cube', root + '/SM_Existing')
        mesh.set_material(0, material)
        assert eal.save_loaded_asset(mesh, False)
        source = folder / (category + '.fbx')
        export(mesh, source)
        slots = mesh.get_editor_property('static_materials')
        slot = material.get_name()  # FBX exporter uses the bound material name.
        entry = {'key': category + ':staticMesh:main', 'kind': 'staticMesh', 'name': 'SM_Existing',
                 'targetPath': root + '/SM_Existing', 'sourcePath': str(source), 'sourceHash': linker.digest_file(source),
                 'config': {'family': 'suit' if category in ('hair', 'face') else category},
                 'materials': {}, 'externalMaterials': {slot: material.get_path_name()}, 'unresolvedMaterials': []}
        entry['fingerprint'] = importer.digest(entry)
        suits.append({'assetId': category, 'name': category, 'family': entry['config']['family'], 'targetRoot': root, 'entries': [entry]})
    job = {'suits': suits}
    write('job.json', job)
    result = linker.prepare(job)
    write('prepare.json', result)
    assert all(row['status'] == 'checking' for row in result['entries']), result
else:
    job = json.loads((folder / 'job.json').read_text(encoding='utf-8'))
    verified = json.loads((folder / 'verified.json').read_text(encoding='utf-8'))
    before = json.loads((folder / 'prepare.json').read_text(encoding='utf-8'))
    result = linker.apply(verified)
    assert all(row['status'] == 'success' for row in result['entries']), result
    for row in before['entries']:
        obj = eal.load_asset(row['path'])
        assert linker.snapshot(obj) == row['state']
        assert linker.dependencies(obj) == row['dependencies']
    proof = {'linked': len(result['entries']), 'packagesUnchanged': True, 'families': {}}
    inspected = importer.inspect(copy.deepcopy(job))
    assert all(row['action'] == 'unchanged' and row['status'] == 'ready' for row in inspected['entries']), inspected
    unchanged = send(copy.deepcopy(job))
    assert all(row['status'] == 'unchanged' for row in unchanged['entries']), unchanged
    again = importer.inspect(copy.deepcopy(job))
    assert all(row['action'] == 'unchanged' for row in again['entries']), again
    # Verify the no-op send did not lose sidecar identity or mutate packages.
    for row in before['entries']:
        assert linker.snapshot(eal.load_asset(row['path'])) == row['state']
    for suit in job['suits']:
        entry = suit['entries'][0]
        old = eal.load_asset(entry['targetPath'])
        source_mesh = eal.duplicate_asset('/Engine/BasicShapes/Sphere', '/Game/LinkProof/Source_' + suit['assetId'])
        source_mesh.set_material(0, eal.load_asset('/Game/LinkProof/M_LinkProof'))
        export(source_mesh, Path(entry['sourcePath']))
        entry['sourceHash'] = linker.digest_file(entry['sourcePath'])
        entry['fingerprint'] = importer.digest({key: value for key, value in entry.items() if key != 'fingerprint'})
        single = {'suits': [copy.deepcopy(suit)]}
        checked = importer.inspect(single)
        assert checked['entries'][0]['action'] == 'updated', checked
        updated = send(single)
        assert updated['entries'][0]['status'] == 'success', updated
        assert eal.load_asset(entry['targetPath']) == old, 'Updated object identity changed'
        added = copy.deepcopy(entry)
        added.update(key=suit['assetId'] + ':staticMesh:additional', name='SM_Added', targetPath=suit['targetRoot'] + '/SM_Added')
        added['fingerprint'] = importer.digest(added)
        suit['entries'].append(added)
        extended = {'suits': [copy.deepcopy(suit)]}
        checked = importer.inspect(extended)
        assert [r['action'] for r in checked['entries']] == ['unchanged', 'added'], checked
        applied = send(extended)
        assert [r['status'] for r in applied['entries']] == ['unchanged', 'success'], applied
        repeated = importer.inspect({'suits': [copy.deepcopy(suit)]})
        assert all(r['action'] == 'unchanged' for r in repeated['entries']), repeated
        proof['families'][suit['assetId']] = {'updatedSameObject': True, 'added': True, 'repeatUnchanged': True}
    write('proof.json', proof)
