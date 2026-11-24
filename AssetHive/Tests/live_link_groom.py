"""Groom content association and changed-curve rejection in a disposable project."""
import json
import os
from pathlib import Path
import unreal
import assethive_link_only as linker
import assethive_suit_import as importer
assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
fixtures = Path(os.environ['ASSETHIVE_GROOM_FIXTURES'])
source = fixtures / 'source.abc'
changed = fixtures / 'changed.abc'
root = '/Game/Characters/Hero/Asset_PJZ/Male_01/Mesh/Hair/Groom'
entry = {'key':'groom:groom:main', 'kind':'groom', 'name':'Groom_LinkProof', 'targetPath':root + '/Groom_LinkProof',
         'sourcePath':str(source), 'sourceHash':linker.digest_file(source), 'config':{'family':'suit','heroPart':'Hair'},
         'materials':{}, 'externalMaterials':{}, 'unresolvedMaterials':[]}
entry['fingerprint'] = importer.digest(entry)
model = importer.import_file(entry, entry['targetPath'])
assert unreal.EditorAssetLibrary.save_loaded_asset(model, False)
before = linker.snapshot(model)
assert unreal.AssetHiveModelCatalogLibrary.verify_groom_source(model, str(source)), 'Same groom failed content comparison'
assert not unreal.AssetHiveModelCatalogLibrary.verify_groom_source(model, str(changed)), 'Changed curve incorrectly matched'
job = {'suits':[{'assetId':'groom', 'name':'Groom_LinkProof', 'targetRoot':root, 'entries':[entry]}]}
checked = linker.prepare(job)
assert checked['entries'][0]['status'] == 'checking', checked
linked = linker.apply({'inspectToken':checked['inspectToken'],'verifiedKeys':[entry['key']]})
assert linked['entries'][0]['status'] == 'success', linked
assert linker.snapshot(model) == before
checked = importer.inspect(job)
assert checked['entries'][0]['action'] == 'unchanged', checked
entry['sourcePath'] = str(changed)
entry['sourceHash'] = linker.digest_file(changed)
entry['fingerprint'] = importer.digest(entry)
job['inspectToken'] = importer.inspect(job)['inspectToken']
updated = importer.apply_job(job)
assert updated['entries'][0]['status'] == 'success', updated
assert unreal.EditorAssetLibrary.load_asset(entry['targetPath']) == model
assert unreal.AssetHiveModelCatalogLibrary.verify_groom_source(model, str(changed))
assert importer.inspect(job)['entries'][0]['action'] == 'unchanged'
folder = Path(unreal.Paths.project_saved_dir()) / 'CharacterLinkAcceptance'
folder.mkdir(parents=True, exist_ok=True)
(folder/'groom-proof.json').write_text(json.dumps({'linkedUnchanged':True,'changedCurveRejected':True,'updateSameObject':True,'repeatUnchanged':True}), encoding='utf-8')
