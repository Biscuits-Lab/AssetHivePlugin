"""Run without Unreal: python -m unittest discover -s Tests -p test_suit_import.py."""
import importlib.util
import pathlib
import sys
import types
import unittest
from unittest.mock import patch

unreal_stub = types.ModuleType('unreal')
unreal_stub.EditorAssetLibrary = types.SimpleNamespace(get_metadata_tag=lambda obj, key: obj.tags.get(key, ''))
unreal_stub.MaterialEditingLibrary = types.SimpleNamespace()
unreal_stub.log = lambda text: None
unreal_stub.log_error = lambda text: None
sys.modules.setdefault('unreal', unreal_stub)
script = pathlib.Path(__file__).resolve().parents[1] / 'Content/Python/assethive_suit_import.py'
if not script.exists():
    script = pathlib.Path(__file__).with_name('assethive_suit_import.py')
spec = importlib.util.spec_from_file_location('suit_import', script)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class Asset:
    def __init__(self, key, path, stamp):
        import json
        self.path = path
        self.tags = {module.KEY: key, module.STAMP: json.dumps(stamp)}

    def get_path_name(self):
        return self.path


class SuitImportTests(unittest.TestCase):
    def setUp(self):
        self.entry = {'key': 'owner:texture:head:D', 'name': 'T_Test_Head_D', 'kind': 'texture', 'targetPath': module.ROOT + 'Female_01/Mesh/Test/Textures/T_Test_Head_D', 'fingerprint': 'original'}
        self.job = {'suits': [{'assetId': 'owner', 'selectedSlot': '', 'entries': [self.entry]}]}
        self.records = {self.entry['key']: {**self.entry, 'assetId': 'owner', 'path': self.entry['targetPath']}}

    def inspect(self, objects, owned=None):
        with patch.object(module, 'read_registry', return_value=self.records), patch.object(module, 'load', side_effect=objects.get), patch.object(module, 'owned_objects', return_value=owned or {}), patch.object(module, 'validate_entry'):
            return module.inspect(self.job)

    def test_missing_is_not_unchanged_and_skip_does_not_recreate(self):
        result = self.inspect({})
        self.assertEqual(result['entries'][0]['status'], 'missing')
        self.job['inspectToken'] = result['inspectToken']
        with patch.object(module, 'inspect', return_value=result), patch.object(module, 'read_registry', return_value=self.records), patch.object(module, 'import_file') as importer:
            applied = module.apply_job(self.job)
        importer.assert_not_called()
        self.assertEqual(applied['entries'][0]['status'], 'skipped')

    def test_manually_moved_asset_is_found_without_recreating_it(self):
        moved = Asset(self.entry['key'], '/Game/Test/Moved.Moved', self.entry)
        row = self.inspect({moved.path: moved}, {self.entry['key']: [moved]})['entries'][0]
        self.assertEqual(row['action'], 'unchanged')
        self.assertEqual(row['path'], moved.path)

    def test_duplicate_identity_and_destination_collision_are_blocked(self):
        original = Asset(self.entry['key'], self.entry['targetPath'], self.entry)
        duplicate = Asset(self.entry['key'], '/Game/Test/Copy.Copy', self.entry)
        row = self.inspect({original.path: original}, {self.entry['key']: [original, duplicate]})['entries'][0]
        self.assertEqual(row['status'], 'blocked')
        old_path = original.path
        self.entry['targetPath'] += '_Renamed'
        collision = Asset('someone-else', self.entry['targetPath'], {})
        row = self.inspect({old_path: original, collision.path: collision})['entries'][0]
        self.assertEqual(row['status'], 'blocked')

    def test_library_removal_keeps_ue_object(self):
        self.job['suits'][0]['entries'] = []
        row = self.inspect({})['entries'][0]
        self.assertEqual(row['action'], 'retained')

    def test_reinspection_invalidates_old_confirmation(self):
        self.job['inspectToken'] = 'stale'
        with patch.object(module, 'inspect', return_value={'inspectToken': 'new'}):
            with self.assertRaisesRegex(RuntimeError, '重新检查'):
                module.apply_job(self.job)

    def test_only_overlapping_material_edits_conflict(self):
        saved = {'config': {'parentPath': 'parent'}, 'textures': {'D': 'old-d', 'N': 'normal'}, 'appliedMaterial': {'parent': 'parent', 'textures': {'D': '/old', 'N': '/normal'}}}
        current = {'parent': 'parent', 'textures': {'D': '/hand-tuned', 'N': '/normal', 'A': ''}}
        entry = {'config': {'parentPath': 'parent'}, 'textures': {'D': 'old-d', 'N': 'normal', 'A': 'new-mask'}}
        self.assertEqual(module.material_conflicts(current, saved, entry), [])
        entry['textures']['D'] = 'new-d'
        self.assertEqual(module.material_conflicts(current, saved, entry), ['D'])

    def test_bone_names_follow_fbx_importer_not_three_animation_sanitizing(self):
        self.assertEqual(module.imported_bone_name('namespace:Bip001 L.Hand'), 'bip001-l_hand')
        self.assertEqual(module.imported_bone_name(''), '')

    def test_status_observes_delete_and_restore_without_import_or_save(self):
        original = Asset(self.entry['key'], self.entry['targetPath'], self.entry)
        for exists in (True, False, True):
            with patch.object(module, 'read_registry', return_value=self.records), patch.object(module, 'load', return_value=original if exists else None), patch.object(module, 'owned_objects', return_value={}), patch.object(module, 'import_file') as importer, patch.object(module, 'write_registry') as writer:
                row = module.suit_status(self.job)['entries'][0]
                self.assertEqual(row['present'], exists)
                self.assertEqual(bool(row['path']), exists)
                importer.assert_not_called()
                writer.assert_not_called()


if __name__ == '__main__':
    unittest.main()
