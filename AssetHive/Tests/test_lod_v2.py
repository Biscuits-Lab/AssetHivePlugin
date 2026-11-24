import copy
import hashlib
import json
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from test_suit_import import module


class LodV2Tests(unittest.TestCase):
    def test_skin_transfer_uses_verified_material_paths_including_existing_sm_receipt(self):
        calls = []
        library = types.SimpleNamespace(complete_skinned_lods_from_static=lambda *args: calls.append(args) or json.dumps({'pending': []}))
        entry = {'key': 'asset:skeletalMesh:head', 'config': {'lodPolicy': 'lod1-reduction-v2',
                 'skinLodSource': {'sourcePath': 'head.fbx', 'sourceHash': 'hash', 'sourceLod': 0}}}
        records = {'asset:staticMesh:head': {'externalMaterials': {'Old_SM_Slot': '/Game/Material/Head'}}}
        with patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True), patch.object(module, 'read_registry', return_value=records):
            module.lod_policy_report(object(), entry)
            self.assertEqual(json.loads(calls[-1][-1]), {'Old_SM_Slot': '/Game/Material/Head'})
            entry['config']['skinLodSource']['sourceMaterials'] = {'New_SM_Slot': '/Game/Material/NewHead'}
            module.lod_policy_report(object(), entry)
            self.assertEqual(json.loads(calls[-1][-1]), {'New_SM_Slot': '/Game/Material/NewHead'})

    def test_static_skin_source_is_validated_and_does_not_reimport_base(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'SM_LOD1.fbx'
            source.write_bytes(b'static-geometry')
            entry = {'kind': 'skeletalMesh', 'config': {'lodPolicy': 'lod1-reduction-v2', 'skinLodSource': {
                'sourcePath': str(source), 'sourceHash': hashlib.sha256(source.read_bytes()).hexdigest(), 'sourceLod': 0, 'version': 1}}}
            events = []
            def derive(*args):
                result = self.report()
                result['levels'][1]['source'] = 'existing'
                if args[-1]: events.append('derive')
                else: result['pending'] = [1, 2, 3, 4, 5]
                return json.dumps(result)
            library = types.SimpleNamespace(complete_skinned_lods_from_static=derive)
            api = types.SimpleNamespace(import_lod=lambda *args: self.fail('SM must not be imported as a skeletal FBX'), get_lod_count=lambda mesh: 6)
            with patch.object(module.unreal, 'SkeletalMeshEditorSubsystem', object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True), patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True):
                report = module.apply_model_lods(object(), entry, entry)
                self.assertEqual(events, ['derive'])
                self.assertFalse(report['pending'])
                source.write_bytes(b'changed-after-check')
                with self.assertRaisesRegex(RuntimeError, 'SM LOD1 文件已变化'):
                    module.apply_model_lods(object(), entry, entry)

    def test_shared_skin_source_path_is_not_identity(self):
        a = {'key': 'x:skeletalMesh:arm', 'kind': 'skeletalMesh', 'sourceHash': 'base', 'config': {'skinLodSource': {
            'sourcePath': 'Z:/library/SM_LOD1.fbx', 'sourceHash': 'geometry', 'sourceLod': 0, 'version': 1}}}
        b = copy.deepcopy(a)
        b['config']['skinLodSource']['sourcePath'] = '//fileshare/share/library/SM_LOD1.fbx'
        self.assertTrue(module.same_entry_content(a, b))
        b['config']['skinLodSource']['sourceHash'] = 'changed'
        self.assertFalse(module.same_entry_content(a, b))

    def report(self):
        return {'policy': 'lod1-reduction-v2', 'qualityVersion': 1, 'count': 6, 'missingManual': [], 'missing': [], 'pending': [],
                'levels': [{'index': i, 'triangles': 1024 if i == 0 else 512 // 2**(i-1),
                            'source': 'imported' if i < 2 else 'generated', 'structureVerified': i >= 2} for i in range(6)]}

    def test_both_mesh_kinds_reimport_artist_lod_even_when_previous_count_matches(self):
        for kind, subsystem in [('staticMesh', 'StaticMeshEditorSubsystem'), ('skeletalMesh', 'SkeletalMeshEditorSubsystem')]:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as folder:
                file = Path(folder) / 'LOD1.fbx'
                file.write_bytes(b'artist')
                entry = {'kind': kind, 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
                    {'index': 1, 'sourcePath': str(file), 'sourceHash': hashlib.sha256(b'artist').hexdigest()}]}}
                events = []
                api = types.SimpleNamespace(import_lod=lambda mesh, index, path: events.append(index) or index, get_lod_count=lambda mesh: 6)
                def report(*args):
                    result = self.report()
                    if not args[2]: result['missingManual'] = [1]
                    return json.dumps(result)
                library = types.SimpleNamespace(protect_imported_lod=lambda *a: None, complete_character_lods=report)
                with patch.object(module.unreal, subsystem, object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True), patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True):
                    result = module.apply_model_lods(object(), entry, entry)
                    self.assertEqual(events, [1])
                    self.assertEqual(result['count'], 6)

    def test_unchanged_artist_lod_is_not_imported_again_for_either_mesh_kind(self):
        for kind, subsystem in [('staticMesh', 'StaticMeshEditorSubsystem'), ('skeletalMesh', 'SkeletalMeshEditorSubsystem')]:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as folder:
                file = Path(folder) / 'LOD1.fbx'
                file.write_bytes(b'artist')
                entry = {'kind': kind, 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
                    {'index': 1, 'sourcePath': str(file), 'sourceHash': hashlib.sha256(b'artist').hexdigest()}]}}
                events = []
                api = types.SimpleNamespace(import_lod=lambda *a: events.append('import'), get_lod_count=lambda mesh: 6)
                def report(*args):
                    if args[2]: events.append('regenerate')
                    return json.dumps(self.report())
                library = types.SimpleNamespace(protect_imported_lod=lambda *a: events.append('protect'), complete_character_lods=report)
                with patch.object(module.unreal, subsystem, object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True), patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True):
                    module.apply_model_lods(object(), entry, entry)
                    self.assertEqual(events, [])

    def test_missing_artist_mesh_or_failed_reduction_cannot_succeed(self):
        entry = {'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [{'index': 1}]}}
        for change in ('missing', 'empty', 'generated_instead_of_artist', 'absent_report', 'broken_structure', 'old_plugin'):
            report = copy.deepcopy(self.report())
            if change == 'missing': report['missing'] = [5]
            if change == 'empty': report['levels'][1]['triangles'] = 0
            if change == 'generated_instead_of_artist': report['levels'][1]['source'] = 'generated'
            if change == 'absent_report': report = None
            if change == 'broken_structure': report['levels'][2]['structureVerified'] = False
            if change == 'old_plugin': del report['qualityVersion']
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                module.verify_lod_report(entry, report)

    def test_old_binary_cannot_claim_v2_support(self):
        with patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', types.SimpleNamespace(complete_configured_lods=lambda *a: '{}'), create=True):
            with self.assertRaises(RuntimeError):
                module.lod_policy_report(object(), {'config': {'lodPolicy': 'lod1-reduction-v2'}})
