import json
import hashlib
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from test_suit_import import module, Asset

class AdaptiveLodTests(unittest.TestCase):
    def test_full_artist_chain_and_incremental_update_preserve_manual_levels(self):
        with tempfile.TemporaryDirectory() as folder:
            sources=[]
            for i in range(1,6):
                source=Path(folder)/('model_LOD%d.fbx'%i);source.write_bytes(bytes([i]))
                sources.append({'index':i,'sourcePath':str(source),'sourceHash':hashlib.sha256(source.read_bytes()).hexdigest()})
            entry={'kind':'skeletalMesh','config':{'lodPolicy':'existing-settings-v1','lodSources':sources}}
            events=[]
            api=types.SimpleNamespace(import_lod=lambda mesh,i,path:events.append(('import',i)) or i,get_lod_count=lambda mesh:6)
            library=types.SimpleNamespace(protect_imported_lod=lambda mesh,i:events.append(('protect',i)),complete_configured_lods=lambda mesh,manual,regen:json.dumps({'manual':list(manual),'regenerate':regen}))
            with patch.object(module.unreal,'AssetHiveModelCatalogLibrary',library,create=True),patch.object(module.unreal,'SkeletalMeshEditorSubsystem',object,create=True),patch.object(module.unreal,'get_editor_subsystem',return_value=api,create=True):
                result=module.apply_model_lods(object(),entry)
                self.assertEqual(result['manual'],list(range(6)))
                self.assertTrue(result['regenerate'])
                self.assertEqual([e[1] for e in events if e[0]=='import'],[1,2,3,4,5])
                events.clear();module.apply_model_lods(object(),entry,entry)
                self.assertFalse(any(e[0]=='import' for e in events))
                prior={'config':{'lodSources':[dict(s) for s in sources]}}
                prior['config']['lodSources'][0]['sourceHash']='old'
                events.clear();module.apply_model_lods(object(),entry,prior)
                self.assertEqual([e for e in events if e[0]=='import'],[('import',1)])

    def test_no_source_lods_still_reports_missing_settings(self):
        api=types.SimpleNamespace(get_lod_count=lambda mesh:1)
        library=types.SimpleNamespace(complete_configured_lods=lambda mesh,manual,regen:json.dumps({'missing':[2,3,4,5]}))
        with patch.object(module.unreal,'AssetHiveModelCatalogLibrary',library,create=True),patch.object(module.unreal,'StaticMeshEditorSubsystem',object,create=True),patch.object(module.unreal,'get_editor_subsystem',return_value=api,create=True):
            self.assertEqual(module.apply_model_lods(object(),{'kind':'staticMesh','config':{'lodPolicy':'existing-settings-v1'}})['missing'],[2,3,4,5])

    def test_old_plugin_reports_upgrade_instead_of_silently_skipping(self):
        with patch.object(module.unreal,'AssetHiveModelCatalogLibrary',None,create=True):
            with self.assertRaises(RuntimeError): module.lod_policy_report(object(),{'config':{'lodPolicy':'existing-settings-v1'}})

    def test_adding_ue_settings_marks_unchanged_mesh_as_updatable(self):
        entry={'key':'owner:staticMesh:main','name':'SM_Test','kind':'staticMesh','targetPath':module.ROOT+'Test/SM_Test','fingerprint':'same','config':{'lodPolicy':'existing-settings-v1'}}
        obj=Asset(entry['key'],entry['targetPath'],entry)
        job={'suits':[{'assetId':'owner','selectedSlot':'','entries':[entry]}]}
        with patch.object(module,'read_registry',return_value={entry['key']:{**entry,'assetId':'owner','path':entry['targetPath']}}),patch.object(module,'load',return_value=obj),patch.object(module,'owned_objects',return_value={}),patch.object(module,'validate_entry'),patch.object(module,'lod_policy_report',return_value={'pending':[2,3,4,5],'missing':[]}):
            row=module.inspect(job)['entries'][0]
            self.assertEqual(row['action'],'updated')
            self.assertEqual(row['lodReport']['pending'],[2,3,4,5])
