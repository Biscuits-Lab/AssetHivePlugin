import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from test_suit_import import Asset

spec = importlib.util.spec_from_file_location('link_only', Path(__file__).resolve().parents[1] / 'Content/Python/assethive_link_only.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class LinkOnlyTests(unittest.TestCase):
    def test_repeated_material_binding_uses_exact_export_slot_aliases(self):
        class Material:
            def get_name(self): return 'Shared'
            def get_path_name(self): return '/Game/Shared.Shared'
        class Slot:
            def get_editor_property(self, name): return Material()
        class Mesh:
            def get_editor_property(self, name): return [Slot(), Slot(), Slot()]
        with patch.object(module.unreal, 'StaticMesh', Mesh, create=True):
            source, exported = module.material_evidence(Mesh(), {'kind':'staticMesh', 'externalMaterials':{'OldSlot':'/Game/Shared'}}, {})
        self.assertEqual(source, {'OldSlot':'/Game/Shared'})
        self.assertEqual(exported, {'Shared':'/Game/Shared','Shared_1':'/Game/Shared','Shared_2':'/Game/Shared'})

    def test_changed_package_or_dependencies_cannot_be_linked_and_writes_nothing(self):
        row = {'key':'test:staticMesh:main','assetId':'test','status':'checking','kind':'staticMesh','path':'/Game/Test.SM',
               'sourceHash':'source', 'sourcePath':'source.fbx','exportedPath':'export.fbx','exportedHash':'export',
               'state':{'hash':'original'}, 'dependencies':{'material':'original'}}
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            (folder/'check.json').write_text(json.dumps({'entries':[row],'projectPath':'D:/Test.uproject'}),encoding='utf-8')
            obj = Asset('', row['path'], {})
            for state, deps in [({'hash':'changed'},row['dependencies']), (row['state'],{'material':'changed'})]:
                with patch.object(module, 'check_dir', return_value=folder), patch.object(module, 'read_links', return_value={}), \
                     patch.object(module.unreal.EditorAssetLibrary, 'load_asset', return_value=obj, create=True), \
                     patch.object(module, 'snapshot', return_value=state), patch.object(module, 'dependencies', return_value=deps), \
                     patch.object(module, 'atomic_json') as write:
                    result = module.apply({'inspectToken':'token','verifiedKeys':[row['key']]})
                    self.assertEqual(result['entries'][0]['status'],'failed')
                    write.assert_not_called()

    def test_token_cannot_escape_saved_check_directory(self):
        with self.assertRaises(RuntimeError):
            module.check_dir('../other/check')
