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
    def report(self):
        return {'policy': 'lod1-reduction-v2', 'count': 6, 'missingManual': [], 'missing': [], 'pending': [],
                'levels': [{'index': i, 'triangles': 1024 if i == 0 else 512 // 2**(i-1),
                            'source': 'imported' if i < 2 else 'generated'} for i in range(6)]}

    def test_both_mesh_kinds_reimport_artist_lod_even_when_previous_count_matches(self):
        for kind, subsystem in [('staticMesh', 'StaticMeshEditorSubsystem'), ('skeletalMesh', 'SkeletalMeshEditorSubsystem')]:
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as folder:
                file = Path(folder) / 'LOD1.fbx'
                file.write_bytes(b'artist')
                entry = {'kind': kind, 'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [
                    {'index': 1, 'sourcePath': str(file), 'sourceHash': hashlib.sha256(b'artist').hexdigest()}]}}
                events = []
                api = types.SimpleNamespace(import_lod=lambda mesh, index, path: events.append(index) or index, get_lod_count=lambda mesh: 6)
                library = types.SimpleNamespace(protect_imported_lod=lambda *a: None, complete_character_lods=lambda *a: json.dumps(self.report()))
                with patch.object(module.unreal, subsystem, object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True), patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True):
                    result = module.apply_model_lods(object(), entry, entry)
                    self.assertEqual(events, [1])
                    self.assertEqual(result['count'], 6)

    def test_missing_artist_mesh_or_failed_reduction_cannot_succeed(self):
        entry = {'config': {'lodPolicy': 'lod1-reduction-v2', 'lodSources': [{'index': 1}]}}
        for change in ('missing', 'empty', 'generated_instead_of_artist', 'absent_report'):
            report = copy.deepcopy(self.report())
            if change == 'missing': report['missing'] = [5]
            if change == 'empty': report['levels'][1]['triangles'] = 0
            if change == 'generated_instead_of_artist': report['levels'][1]['source'] = 'generated'
            if change == 'absent_report': report = None
            with self.subTest(change=change), self.assertRaises(RuntimeError):
                module.verify_lod_report(entry, report)

    def test_old_binary_cannot_claim_v2_support(self):
        with patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', types.SimpleNamespace(complete_configured_lods=lambda *a: '{}'), create=True):
            with self.assertRaises(RuntimeError):
                module.lod_policy_report(object(), {'config': {'lodPolicy': 'lod1-reduction-v2'}})
