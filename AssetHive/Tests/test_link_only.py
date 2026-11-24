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
    def test_gray_export_preserves_u8_and_does_not_downconvert_sixteen_bit(self):
        import types
        with tempfile.TemporaryDirectory() as temp:
            png = Path(temp) / 'texture.png'
            def header(depth, color):
                return b'\x89PNG\r\n\x1a\n' + b'\x00\x00\x00\x0dIHDR' + b'\x00\x00\x00\x01' * 2 + bytes([depth, color])
            calls = []
            def export(task):
                calls.append(task.filename)
                self.assertIsNone(task.exporter)
                Path(task.filename).write_bytes(b'tga')
                return True
            task = types.SimpleNamespace(filename=str(png), exporter=object())
            with patch.object(module.unreal, 'Exporter', types.SimpleNamespace(run_asset_export_task=export), create=True):
                for depth, color in [(16, 0), (16, 6), (8, 6)]:
                    png.write_bytes(header(depth, color))
                    self.assertEqual(module.preserve_texture_export_pixels(task, png), png)
                self.assertEqual(calls, [])
                png.write_bytes(header(8, 0))
                self.assertEqual(module.preserve_texture_export_pixels(task, png), png.with_suffix('.tga'))
                self.assertEqual(len(calls), 1)
            with patch.object(module.unreal, 'Exporter', types.SimpleNamespace(run_asset_export_task=lambda task: False), create=True):
                with self.assertRaisesRegex(RuntimeError, '无法无损'):
                    module.preserve_texture_export_pixels(task, png)

    def test_python_source_control_checks_out_readonly_registry(self):
        import types
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'Links.json'
            file.write_text('{"old":{}}')
            file.chmod(0o444)
            calls = []
            def checkout(path, silent):
                calls.append(path)
                Path(path).chmod(0o666)
                return True
            control = types.SimpleNamespace(is_enabled=lambda: True, is_available=lambda: True,
                                            check_out_or_add_file=checkout)
            try:
                with patch.object(module, 'store_path', return_value=file), patch.object(module.unreal, 'SourceControl', control, create=True):
                    module.write_links({'new': {}})
                self.assertEqual(calls, [str(file)])
                self.assertEqual(json.loads(file.read_text()), {'new': {}})
            finally:
                file.chmod(0o666)

    def test_readonly_registry_without_provider_preserves_original(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'Links.json'
            file.write_text('{"old":{}}')
            file.chmod(0o444)
            try:
                with patch.object(module, 'store_path', return_value=file), patch.object(module.unreal, 'SourceControl', None, create=True):
                    with self.assertRaisesRegex(RuntimeError, '只读'):
                        module.write_links({'new': {}})
                self.assertEqual(json.loads(file.read_text()), {'old': {}})
                self.assertEqual(list(Path(temp).glob('*.tmp')), [])
            finally:
                file.chmod(0o666)

    def test_shared_link_store_checks_out_existing_file_before_replacing(self):
        import types
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp)/'Links.json'
            file.write_text('{"old":{}}')
            control = types.SimpleNamespace(is_enabled=lambda:True, is_available=lambda:True,
                                            check_out_or_add_file=lambda *args:False)
            with patch.object(module, 'store_path', return_value=file), patch.object(module.unreal, 'SourceControl', control, create=True):
                with self.assertRaises(RuntimeError): module.write_links({'new':{}})
            self.assertEqual(json.loads(file.read_text()), {'old':{}})

    def test_failed_source_control_add_rolls_back_only_new_link_file(self):
        import types
        with tempfile.TemporaryDirectory() as temp:
            file=Path(temp)/'Links.json'
            control=types.SimpleNamespace(is_enabled=lambda:True,is_available=lambda:True,
                                           check_out_or_add_file=lambda *args:False)
            with patch.object(module,'store_path',return_value=file),patch.object(module.unreal,'SourceControl',control,create=True):
                with self.assertRaises(RuntimeError):module.write_links({'new':{}})
            self.assertFalse(file.exists())

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
