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
    def test_root_container_requires_the_source_ancestor_chain(self):
        entry = {'key': 'owner:skeletalMesh:main', 'kind': 'skeletalMesh', 'sourcePath': str(script),
                 'targetPath': '/Game/Characters/Monster/Test/SK_Test', 'skeletonPath': '/Game/Characters/Monster/Test/SKEL_Test',
                 'config': {'family': 'monster', 'ownSkeleton': True},
                 'bones': [{'name': 'root', 'parent': '', 'rootParents': ['Rig']}],
                 'targetBones': [{'name': 'Rig', 'parent': ''}, {'name': 'root', 'parent': 'Rig'}]}
        with patch.object(module, 'validate_weapon_helper', return_value=object()), patch.object(module.unreal, 'Skeleton', object, create=True):
            module.validate_entry(entry)
            entry['bones'][0]['rootParents'] = ['OtherRig']
            with self.assertRaises(RuntimeError):
                module.validate_entry(entry)
            entry['bones'][0]['rootParents'] = []
            with self.assertRaises(RuntimeError):
                module.validate_entry(entry)

    def test_fbx_slot_dot_sanitization_is_unique(self):
        entry = {'materials': {'Body.003': 'body'}}
        self.assertEqual(module.mapped_slot_name('Body_003', entry), 'Body.003')
        self.assertEqual(module.mapped_slot_name('Other', entry), 'Other')
        ambiguous = {'materials': {'A.B_C': 'one', 'A_B.C': 'two'}}
        self.assertEqual(module.mapped_slot_name('A_B_C', ambiguous), 'A_B_C')
        entry['materials']['Body_003'] = 'exact'
        self.assertEqual(module.mapped_slot_name('Body_003', entry), 'Body_003')

    def test_same_name_in_another_character_directory_is_not_a_legacy_owner(self):
        entry = {'key':'new:staticMesh:main','kind':'staticMesh','name':'SM_Main','targetPath':'/Game/Characters/NPC/A/SM_Main'}
        saved = {**entry, 'targetPath':'/Game/Characters/Monster/B/SM_Main'}
        self.assertFalse(module.legacy_export_matches(entry, 'old:staticMesh:main', saved))
        self.assertTrue(module.legacy_export_matches(entry, 'old:staticMesh:main', {**saved, 'targetPath':entry['targetPath']}))

    def test_linked_sidecar_baseline_requires_exact_object_and_preserves_native_receipt(self):
        obj = Asset('', '/Game/Existing.SM_Existing', {})
        sidecar = {'path':'/Game/Existing.SM_Existing', 'linkOnly':True, 'fingerprint':'verified'}
        self.assertEqual(module.receipt(obj, sidecar), sidecar)
        self.assertEqual(module.receipt(obj, {**sidecar, 'path':'/Game/Other.SM_Other'}), {})
        self.assertEqual(module.receipt(obj, {**sidecar, 'linkOnly':False}), {})
        obj = Asset('owner', obj.path, {'fingerprint':'new-import'})
        self.assertEqual(module.receipt(obj, sidecar), {'fingerprint':'new-import'})

    def test_buqiang_case_only_destination_keeps_existing_material(self):
        source = '/Game/Suit/Materials/MI_Zhou_Manhunt_head.MI_Zhou_Manhunt_head'
        target = '/Game/Suit/Materials/MI_Zhou_Manhunt_Head'
        obj = Asset('', source, {})
        with patch.object(module.EAL, 'rename_asset', create=True) as rename, patch.object(module, 'load') as load:
            self.assertEqual(module.relocate_resource(obj, source, target), (source, obj))
            rename.assert_not_called()
            load.assert_not_called()
        self.assertFalse(module.same_ue_package(source, target + '02'))

    def test_real_relocation_still_rejects_occupied_destination(self):
        obj = Asset('owner:material:head', '/Game/Old/MI_Head', {})
        with patch.object(module, 'load', return_value=object()), patch.object(module.EAL, 'rename_asset', create=True) as rename:
            with self.assertRaises(RuntimeError):
                module.relocate_resource(obj, obj.path, '/Game/New/MI_Head')
            rename.assert_not_called()

    def test_status_checks_library_paths_without_local_registry_and_preserves_foreign_owner(self):
        owned = Asset('owner:material:head', '/Game/Suit/MI_Head', {})
        foreign = Asset('other:staticMesh:head', '/Game/Suit/SM_Head', {})
        objects = {owned.path: owned, foreign.path: foreign}
        job = {'suits': [{'assetId': 'owner', 'entries': [
            {'key': 'owner:material:head', 'path': owned.path},
            {'key': 'owner:staticMesh:head', 'path': foreign.path},
            {'key': 'owner:skeletalMesh:head', 'path': '/Game/Deleted/SK_Head'},
            {'key': 'other:material:head', 'path': owned.path}]}]}
        with patch.object(module, 'read_registry', return_value={}), patch.object(module, 'load', side_effect=objects.get), patch.object(module, 'owned_objects', return_value={}):
            rows = {row['key']: row for row in module.suit_status(job)['entries']}
        self.assertEqual(len(rows), 3)
        self.assertTrue(rows['owner:material:head']['present'])
        self.assertFalse(rows['owner:material:head']['associationPending'])
        self.assertTrue(rows['owner:staticMesh:head']['associationPending'])
        self.assertFalse(rows['owner:skeletalMesh:head']['present'])
        self.assertEqual(foreign.tags[module.KEY], 'other:staticMesh:head')

    def test_item_destination_accepts_current_character_root_and_rejects_old_root(self):
        entry = {'kind': 'texture', 'channel': 'D', 'sourcePath': str(script),
                 'targetPath': '/Game/Characters/GameplayItem/Chest/Test/Texture/T_Test_D',
                 'config': {'family': 'item'}}
        module.validate_entry(entry)
        for destination in ['/Game/GameplayItems/Chest/Test/Texture/T_Test_D',
                            '/Game/Characters/GameplayItem/../Other/T_Test_D']:
            entry['targetPath'] = destination
            with self.assertRaisesRegex(RuntimeError, 'Invalid Suit destination'):
                module.validate_entry(entry)

    def test_item_master_uses_exact_vt_parameters_and_rejects_unsupported_channels(self):
        parent = types.SimpleNamespace()
        entry = {'kind': 'material', 'config': {'family': 'item', 'parentPath': module.ITEM_TEMPLATE, 'virtualTexture': True}, 'textures': {'D': 'd', 'N': 'n', 'ORM': 'orm', 'E': 'e'}}
        with patch.object(module, 'load', return_value=parent), patch.object(module.unreal, 'Material', type(parent), create=True), \
             patch.object(module.MEL, 'get_texture_parameter_names', return_value=['Diffuse_VT', 'Normal_VT', 'ORM_VT', 'Emissive_Tex_VT'], create=True), \
             patch.object(module.MEL, 'get_static_switch_parameter_names', return_value=['UseEmissive'], create=True):
            self.assertIs(module.item_parent(entry), parent)
            self.assertEqual(module.material_parameters(parent, entry)['E'], 'Emissive_Tex_VT')
            entry['textures']['SS'] = 'ss'
            with self.assertRaisesRegex(RuntimeError, '不支持'):
                module.item_parent(entry)
            del entry['textures']['SS']
            entry['config']['virtualTexture'] = False
            with self.assertRaisesRegex(RuntimeError, 'VT'):
                module.item_parent(entry)

    def test_item_emissive_uses_its_switch_and_preserves_later_artist_edits(self):
        entry = {'config': {'family': 'item', 'parentPath': module.ITEM_TEMPLATE}, 'textures': {'E': 'e'}}
        values = {}
        with patch.object(module.MEL, 'set_material_instance_static_switch_parameter_value', side_effect=lambda obj, key, value: values.update({key: value}), create=True) as writer, \
             patch.object(module.MEL, 'get_material_instance_static_switch_parameter_value', side_effect=lambda obj, key: values.get(key), create=True):
            module.configure_emissive_instance(object(), entry, {})
            self.assertEqual(values, {'UseEmissive': True})
            values['UseEmissive'] = False
            writer.reset_mock()
            module.configure_emissive_instance(object(), entry, entry)
            writer.assert_not_called()

    def test_item_upgrade_requires_choice_for_custom_material_but_not_engine_default(self):
        entry = {'kind': 'staticMesh', 'config': {'family': 'item', 'itemMaterialVersion': 1}, 'materials': {'Mat': 'managed-mi'}}
        custom = types.SimpleNamespace(get_path_name=lambda: '/Game/Custom/MI_Artist.MI_Artist')
        slot = types.SimpleNamespace(material_slot_name='Mat', material_interface=custom, get_editor_property=lambda name: 'Mat')
        obj = types.SimpleNamespace(get_editor_property=lambda name: [slot])
        with patch.object(module, 'receipt', return_value={'config': {'deferMaterials': True}}):
            self.assertEqual(module.deferred_item_material_conflicts(obj, entry), ['Mat'])
            slot.material_interface = None
            self.assertEqual(module.deferred_item_material_conflicts(obj, entry), [])

    def test_disconnected_source_control_stops_before_mutation_but_disabled_provider_is_allowed(self):
        control = types.SimpleNamespace(is_enabled=lambda: True, is_available=lambda: False)
        with patch.object(unreal_stub, 'SourceControl', control, create=True):
            self.assertIn('未开始修改资源', module.source_control_issue())
            control.is_enabled = lambda: False
            self.assertEqual(module.source_control_issue(), '')

    def test_pure_rename_moves_only_owned_skeleton_and_physics(self):
        entry = {'key': 'owner:skeletalMesh:main', 'kind': 'skeletalMesh',
                 'skeletonPath': '/Game/New/SKEL_Test',
                 'config': {'family': 'weapon', 'ownSkeleton': True, 'createPhysicsAsset': True,
                            'physicsAssetPath': '/Game/New/SK_Test_Physics'}}
        skeleton = Asset(entry['key'] + ':skeleton', '/Game/Old/SKEL_Test.SKEL_Test', {})
        physics = Asset(entry['key'] + ':physics', '/Game/Old/SK_Test_Physics.SK_Test_Physics', {})
        mesh = types.SimpleNamespace(get_editor_property=lambda prop: {'skeleton': skeleton, 'physics_asset': physics}[prop])
        with patch.object(module, 'load', return_value=None), \
             patch.object(module.EAL, 'rename_asset', return_value=True, create=True) as rename, \
             patch.object(module.EAL, 'save_loaded_asset', return_value=True, create=True):
            module.synchronize_owned_helpers(entry, mesh)
            self.assertEqual(rename.call_count, 2)
            physics.tags[module.KEY] = 'artist-owned'
            entry['config']['ownSkeleton'] = False
            rename.reset_mock()
            module.synchronize_owned_helpers(entry, mesh)
            rename.assert_not_called()

    def test_suit_explicit_new_skeleton_does_not_enable_physics(self):
        entry = {'kind': 'skeletalMesh', 'config': {'family': 'suit', 'ownSkeleton': True,
                 'skeletonOverride': 'create', 'createPhysicsAsset': False}}
        self.assertTrue(module.owned_skeleton_entry(entry))
        self.assertFalse(module.weapon_physics_enabled(entry))

    def test_npc_destination_and_material_contract(self):
        entry = {'key': 'npc:texture:D', 'kind': 'texture', 'channel': 'D', 'sourcePath': __file__,
                 'targetPath': module.NPC_ROOT + 'Merchant/Merchant_FireKeeper/Texture/T_FireKeeper_01_Body_D',
                 'config': {'family': 'npc'}}
        module.validate_entry(entry)
        entry['targetPath'] = module.MONSTER_ROOT + 'Merchant/T_Test'
        with self.assertRaises(RuntimeError):
            module.validate_entry(entry)
        entry['config'].update(ownSkeleton=True, materialRole='monsterSkin')
        self.assertTrue(module.owned_skeleton_entry(entry))
        self.assertTrue(module.monster_material_entry(entry))
        entry['config']['materialRole'] = 'monsterWeapon'
        self.assertTrue(module.weapon_material_entry(entry))

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

    def test_all_material_channels_survive_final_update_and_later_user_edits_are_preserved(self):
        channels = ['A', 'D', 'E', 'N', 'ORM']
        entry = {'config': {'parentPath': 'parent'}, 'textures': {c: 'key-' + c for c in channels}}
        textures = {key: Asset('', '/Game/' + key, {}) for key in entry['textures'].values()}
        values = {'UserDetail': Asset('', '/Game/Keep', {})}
        def setter(obj, name, texture):
            values[name] = texture
        with patch.object(module, 'material_parameters', return_value={c: 'VT_' + c for c in channels}), \
             patch.object(module.MEL, 'set_material_instance_texture_parameter_value', side_effect=setter, create=True) as write, \
             patch.object(module.MEL, 'get_material_instance_texture_parameter_value', side_effect=lambda obj, name: values.get(name), create=True), \
             patch.object(module.MEL, 'update_material_instance', create=True):
            module.apply_material_textures(object(), entry, {}, textures)
            self.assertEqual(write.call_count, 5)
            for channel, key in entry['textures'].items():
                self.assertIs(values['VT_' + channel], textures[key])
            self.assertIn('UserDetail', values)
            values['VT_D'] = Asset('', '/Game/ArtistOverride', {})
            write.reset_mock()
            module.apply_material_textures(object(), entry, entry, textures)
            write.assert_not_called()

    def test_material_batch_detects_lost_earlier_channels_instead_of_reporting_success(self):
        entry = {'config': {'parentPath': 'parent'}, 'textures': {'D': 'color', 'ORM': 'orm'}}
        textures = {key: Asset('', '/Game/' + key, {}) for key in entry['textures'].values()}
        values = {}
        def lossy_setter(obj, name, texture):
            values.clear(); values[name] = texture
        with patch.object(module, 'material_parameters', return_value={'D': 'Diffuse', 'ORM': 'ORM'}), \
             patch.object(module.MEL, 'set_material_instance_texture_parameter_value', side_effect=lossy_setter, create=True), \
             patch.object(module.MEL, 'get_material_instance_texture_parameter_value', side_effect=lambda obj, name: values.get(name), create=True), \
             patch.object(module.MEL, 'update_material_instance', create=True), self.assertRaisesRegex(RuntimeError, 'Diffuse'):
            module.apply_material_textures(object(), entry, {}, textures)

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
        entry = {'config': {'family': 'weapon', 'parentPath': module.WEAPON_TEMPLATE}, 'textures': {'D': 'color', 'N': 'normal', 'ORM': 'orm', 'E': 'emissive'}}
        with patch.object(module, 'load', return_value=parent), patch.object(module.unreal, 'Material', types.SimpleNamespace, create=True), patch.object(module.MEL, 'get_texture_parameter_names', return_value=['Diffuse', 'Normal', 'ORM', 'OpacityMasked', 'Emissive', 'VT_Diffuse'], create=True), patch.object(module.MEL, 'get_static_switch_parameter_names', return_value=['Use VT', 'Use OpacityMasked', 'Use Emissive'], create=True):
            self.assertIs(module.weapon_parent(entry), parent)
            self.assertEqual(module.material_parameters(parent, entry), {'D': 'Diffuse', 'N': 'Normal', 'ORM': 'ORM', 'A': 'OpacityMasked', 'E': 'Emissive'})
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

    def test_emissive_mapping_texture_settings_and_switch_preserve_later_ue_edits(self):
        parent = types.SimpleNamespace()
        body = {'config': {'family': 'monster', 'materialRole': 'monsterCloth', 'parentPath': module.MONSTER_CLOTH_TEMPLATE}, 'textures': {'E': 'emissive'}}
        weapon = {'config': {'family': 'monster', 'materialRole': 'monsterWeapon', 'parentPath': module.MONSTER_WEAPON_TEMPLATE}, 'textures': {'E': 'emissive'}}
        with patch.object(module.MEL, 'get_texture_parameter_names', return_value=['VT_Emissive', 'Emissive'], create=True):
            self.assertEqual(module.material_parameters(parent, body)['E'], 'VT_Emissive')
            self.assertEqual(module.material_parameters(parent, weapon)['E'], 'Emissive')

        class Texture:
            def __init__(self):
                self.values = {}
            def get_editor_property(self, name):
                return self.values.get(name)
            def set_editor_properties(self, values):
                self.values.update(values)
        texture = Texture()
        with patch.object(module.unreal, 'TextureCompressionSettings', types.SimpleNamespace(TC_NORMALMAP='normal', TC_DEFAULT='default', TC_MASKS='masks'), create=True), \
             patch.object(module.unreal, 'TextureGroup', types.SimpleNamespace(TEXTUREGROUP_CHARACTER_NORMAL_MAP='normal-group', TEXTUREGROUP_CHARACTER_SPECULAR='specular-group', TEXTUREGROUP_CHARACTER='character-group'), create=True), \
             patch.object(module.unreal, 'TextureMipGenSettings', types.SimpleNamespace(TMGS_FROM_TEXTURE_GROUP='mips'), create=True), \
             patch.object(module.unreal, 'TextureLossyCompressionAmount', types.SimpleNamespace(TLCA_DEFAULT='lossy'), create=True), \
             patch.object(module.unreal, 'TextureAddress', types.SimpleNamespace(TA_WRAP='wrap'), create=True):
            module.apply_texture(texture, {'channel': 'E', 'config': {'virtualTexture': True}})
        self.assertEqual(texture.values['compression_settings'], 'default')
        self.assertTrue(texture.values['srgb'])

        values = {'Use Emissive': False}
        def setter(obj, name, value):
            values[name] = value
        with patch.object(module.MEL, 'set_material_instance_static_switch_parameter_value', side_effect=setter, create=True) as write, \
             patch.object(module.MEL, 'get_material_instance_static_switch_parameter_value', side_effect=lambda obj, name: values[name], create=True):
            module.configure_emissive_instance(object(), body, {'config': body['config'], 'textures': {}})
            self.assertTrue(values['Use Emissive'])
            values['Use Emissive'] = False  # Artist disables it later in UE.
            write.reset_mock()
            module.configure_emissive_instance(object(), body, body)
            write.assert_not_called()

    def test_monster_cloth_and_skin_use_verified_shared_masters(self):
        parent = types.SimpleNamespace()
        cloth = {'config': {'family': 'monster', 'materialRole': 'monsterCloth', 'parentPath': module.MONSTER_CLOTH_TEMPLATE,
                            'virtualTexture': True, 'useOpacityMasked': False}, 'textures': {'D': 'd', 'E': 'e'}}
        skin = {'config': {'family': 'monster', 'materialRole': 'monsterSkin', 'parentPath': module.MONSTER_SKIN_TEMPLATE,
                           'virtualTexture': True, 'useOpacityMasked': True}, 'textures': {'D': 'd', 'A': 'a', 'E': 'e'}}
        names = ['VT_Diffuse', 'VT_Emissive', 'Diffuse_VT', 'OpacityMasked_VT', 'Emissive_VT']
        switches = ['Use VT', 'Use OpacityMasked', 'Use Emissive']
        with patch.object(module, 'load', return_value=parent), patch.object(module.unreal, 'Material', types.SimpleNamespace, create=True), \
             patch.object(module.MEL, 'get_texture_parameter_names', return_value=names, create=True), \
             patch.object(module.MEL, 'get_static_switch_parameter_names', return_value=switches, create=True):
            self.assertIs(module.monster_parent(cloth), parent)
            self.assertIs(module.monster_parent(skin), parent)
            self.assertEqual(module.material_parameters(parent, cloth), {'D': 'VT_Diffuse', 'E': 'VT_Emissive'})
            self.assertEqual(module.material_parameters(parent, skin), {'D': 'Diffuse_VT', 'A': 'OpacityMasked_VT', 'E': 'Emissive_VT'})

        values = {'Use VT': False, 'Use OpacityMasked': False}
        def setter(obj, name, value):
            values[name] = value
        with patch.object(module.MEL, 'set_material_instance_static_switch_parameter_value', side_effect=setter, create=True) as write, \
             patch.object(module.MEL, 'get_material_instance_static_switch_parameter_value', side_effect=lambda obj, name: values[name], create=True):
            module.configure_monster_instance(object(), skin, {'config': {'parentPath': '/old'}})
            self.assertEqual(values, {'Use VT': True, 'Use OpacityMasked': True})
            values['Use OpacityMasked'] = False
            write.reset_mock()
            module.configure_monster_instance(object(), skin, skin)
            write.assert_not_called()

    def test_old_assethive_owner_can_be_confirmed_without_deleting_ue_object(self):
        old_key = 'deleted-owner:texture:head:D'
        old_entry = {**self.entry, 'key': old_key, 'assetId': 'deleted-owner', 'path': '/Game/Old/T_Test_Head_D'}
        old_object = Asset(old_key, old_entry['path'], old_entry)
        self.records = {old_key: old_entry}
        row = self.inspect({old_object.path: old_object}, {old_key: [old_object]})['entries'][0]
        self.assertEqual(row['status'], 'conflict')
        self.assertEqual(row['action'], 'adopt')
        self.assertEqual(row['oldKey'], old_key)
        self.assertEqual(row['path'], old_object.path)

    def test_confirmed_deleted_owner_reassociates_without_reimport_or_move(self):
        old_key = 'deleted-owner:texture:head:D'
        old_entry = {**self.entry, 'key': old_key, 'assetId': 'deleted-owner'}
        moved = Asset(old_key, '/Game/Artists/MovedTexture', old_entry)
        self.records = {old_key: {**old_entry, 'path': moved.path}}
        self.job['deletedOwnerIds'] = ['deleted-owner']
        checked = self.inspect({moved.path: moved}, {old_key: [moved]})
        row = checked['entries'][0]
        self.assertEqual((row['status'], row['action']), ('ready', 'relinked'))
        self.assertEqual(row['path'], moved.path)
        self.job['inspectToken'] = checked['inspectToken']
        with patch.object(module, 'inspect', return_value=checked), patch.object(module, 'read_registry', return_value=self.records), \
             patch.object(module, 'load', return_value=moved), patch.object(module, 'save') as save, \
             patch.object(module, 'write_registry'), patch.object(module, 'import_file') as importer, \
             patch.object(module.EAL, 'rename_asset', create=True) as rename:
            result = module.apply_job(self.job)
        self.assertEqual(result['entries'][0]['status'], 'success')
        importer.assert_not_called()
        rename.assert_not_called()
        save.assert_called_once()
        self.assertNotIn(old_key, self.records)
        self.assertIn(self.entry['key'], self.records)

    def test_deleted_owner_proof_does_not_override_a_different_resource_or_ambiguity(self):
        self.job['deletedOwnerIds'] = ['old']
        wrong = Asset('old:texture:body:D', self.entry['targetPath'], {**self.entry, 'name': 'Other'})
        self.assertEqual(self.inspect({wrong.path: wrong})['entries'][0]['status'], 'blocked')
        key = 'old:texture:head:D'
        old = {**self.entry, 'key': key, 'assetId': 'old'}
        self.records = {key: old}
        copies = [Asset(key, '/Game/Copy1', old), Asset(key, '/Game/Copy2', old)]
        self.assertEqual(self.inspect({}, {key: copies})['entries'][0]['status'], 'blocked')


    def test_monster_weapon_uses_dedicated_master_and_regular_parameters(self):
        monster = {'config': {'family': 'monster', 'materialRole': 'monsterWeapon', 'parentPath': module.MONSTER_WEAPON_TEMPLATE}, 'textures': {'D': 'd', 'N': 'n', 'ORM': 'orm'}}
        player = {'config': {'family': 'weapon', 'parentPath': module.WEAPON_TEMPLATE}, 'textures': {'D': 'd'}}
        parent = Asset('', '/Game/Existing', {})
        with patch.object(module.unreal, 'Material', Asset, create=True), patch.object(module, 'load', return_value=parent), patch.object(module.MEL, 'get_texture_parameter_names', return_value=['Diffuse', 'Normal', 'ORM', 'VT_Diffuse'], create=True), patch.object(module.MEL, 'get_static_switch_parameter_names', return_value=['Use VT', 'Use OpacityMasked'], create=True):
            self.assertIs(module.weapon_parent(monster), parent)
            self.assertIs(module.weapon_parent(player), parent)
            self.assertEqual(module.material_parameters(parent, monster)['D'], 'Diffuse')
            self.assertEqual(module.material_parameters(parent, {'config': {'family': 'monster'}})['D'], 'VT_Diffuse')
            monster['config']['parentPath'] = module.WEAPON_TEMPLATE
            with self.assertRaises(RuntimeError):
                module.weapon_parent(monster)
            player['config']['parentPath'] = module.MONSTER_WEAPON_TEMPLATE
            with self.assertRaises(RuntimeError):
                module.weapon_parent(player)

    def test_weapon_physics_reuses_assigned_asset_and_does_not_change_suits(self):
        entry = {'key': 'weapon:mesh', 'kind': 'skeletalMesh', 'config': {'family': 'weapon', 'ownSkeleton': True, 'createPhysicsAsset': True, 'physicsAssetPath': '/Game/Weapon/SK_Test_Physics'}}
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
        entry = {'key': 'weapon:mesh', 'kind': 'skeletalMesh', 'config': {'family': 'weapon', 'ownSkeleton': True, 'createPhysicsAsset': True, 'physicsAssetPath': '/Game/Weapon/SK_Test_Physics'}}
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
