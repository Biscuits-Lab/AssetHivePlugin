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
class SlowTask:
    instances = []
    def __init__(self, work, desc, enabled=True):
        self.work = work
        self.opened = 0
        self.closed = False
        self.frames = []
        self.instances.append(self)
    def __enter__(self):
        return self
    def __exit__(self, *args):
        self.closed = True
    def make_dialog(self, cancel):
        self.opened += 1
    def enter_progress_frame(self, work, desc):
        self.frames.append(work)

unreal_stub.ScopedSlowTask = SlowTask
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
        task = SlowTask.instances[-1]
        self.assertEqual(task.opened, 1)
        self.assertTrue(task.closed)
        self.assertEqual(sum(task.frames), task.work)

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


    def test_weapon_reuses_existing_master_and_non_virtual_parameters(self):
        parent = types.SimpleNamespace()
        entry = {'config': {'family': 'weapon', 'parentPath': module.WEAPON_TEMPLATE}, 'textures': {'D': 'color', 'N': 'normal', 'ORM': 'orm'}}
        with patch.object(module, 'load', return_value=parent), patch.object(module.unreal, 'Material', types.SimpleNamespace, create=True), patch.object(module.MEL, 'get_texture_parameter_names', return_value=['Diffuse', 'Normal', 'ORM', 'OpacityMasked', 'VT_Diffuse'], create=True), patch.object(module.MEL, 'get_static_switch_parameter_names', return_value=['Use VT', 'Use OpacityMasked'], create=True):
            self.assertIs(module.weapon_parent(entry), parent)
            self.assertEqual(module.material_parameters(parent, entry), {'D': 'Diffuse', 'N': 'Normal', 'ORM': 'ORM', 'A': 'OpacityMasked'})
        with patch.object(module, 'load', return_value=None), patch.object(module.unreal, 'Material', types.SimpleNamespace, create=True):
            with self.assertRaises(RuntimeError):
                module.weapon_parent(entry)

    def test_weapon_instance_switches_reparent_and_preserve_later_ue_edits(self):
        values = {'Use VT': True, 'Use OpacityMasked': False}
        def setter(obj, name, value):
            values[name] = value
            return False  # Current engine writes successfully but always returns False.
        entry = {'config': {'family': 'weapon', 'parentPath': module.WEAPON_TEMPLATE, 'virtualTexture': False, 'useOpacityMasked': True}, 'textures': {'A': 'mask'}}
        with patch.object(module.MEL, 'set_material_instance_static_switch_parameter_value', side_effect=setter, create=True) as write, patch.object(module.MEL, 'get_material_instance_static_switch_parameter_value', side_effect=lambda obj, name: values[name], create=True):
            module.configure_weapon_instance(object(), entry, {'config': {'parentPath': '/old/generated'}})
            self.assertEqual(values, {'Use VT': False, 'Use OpacityMasked': True})
            write.reset_mock()
            module.configure_weapon_instance(object(), entry, entry)
            write.assert_not_called()


    def test_weapon_physics_reuses_assigned_asset_and_does_not_change_suits(self):
        entry = {'key': 'weapon:mesh', 'kind': 'skeletalMesh', 'config': {'family': 'weapon', 'createPhysicsAsset': True, 'physicsAssetPath': '/Game/Weapon/SK_Test_Physics'}}
        existing = Asset('user-owned', '/Game/Shared/Physics.Physics', {})
        mesh = types.SimpleNamespace(get_editor_property=lambda name: existing)
        with patch.object(module, 'load') as loader, patch.object(module, 'save') as writer, patch.object(module.unreal, 'PhysicsAsset', Asset, create=True), patch.object(module.EAL, 'rename_asset', create=True) as rename:
            self.assertTrue(module.weapon_physics_enabled(entry))
            self.assertIs(module.weapon_physics_for_import(entry, mesh), existing)
            module.save_weapon_physics(entry, mesh, existing)
            loader.assert_not_called()
            writer.assert_not_called()
            rename.assert_not_called()
        self.assertFalse(module.weapon_physics_enabled({'kind': 'skeletalMesh', 'config': {'createPhysicsAsset': True}}))

    def test_new_weapon_physics_is_named_and_missing_physics_is_reported(self):
        entry = {'key': 'weapon:mesh', 'kind': 'skeletalMesh', 'config': {'family': 'weapon', 'createPhysicsAsset': True, 'physicsAssetPath': '/Game/Weapon/SK_Test_Physics'}}
        created = Asset('', '/Game/Weapon/SK_Test_PhysicsAsset.SK_Test_PhysicsAsset', {})
        mesh = types.SimpleNamespace(get_editor_property=lambda name: created)
        with patch.object(module, 'save') as writer, patch.object(module.unreal, 'PhysicsAsset', Asset, create=True), patch.object(module.EAL, 'rename_asset', return_value=True, create=True) as rename:
            module.save_weapon_physics(entry, mesh, None)
            rename.assert_called_once_with(created.path, entry['config']['physicsAssetPath'])
            writer.assert_called_once_with(created, {'key': 'weapon:mesh:physics', 'kind': 'weaponPhysics'})
            with patch.object(module, 'load', return_value=None), patch.object(module.unreal, 'SkeletalMeshEditorSubsystem', types.SimpleNamespace(create_physics_asset=lambda *args: None), create=True), self.assertRaises(RuntimeError):
                module.save_weapon_physics(entry, types.SimpleNamespace(get_editor_property=lambda name: None, get_path_name=lambda: '/Game/Weapon/SK_Test.SK_Test'), None)


if __name__ == '__main__':
    unittest.main()
