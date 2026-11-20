import hashlib
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch
from test_suit_import import module

class CharacterLodTests(unittest.TestCase):
    def test_levels_import_in_order_and_validate_content(self):
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'SM_Model_LOD1.fbx'
            source.write_bytes(b'level1')
            entry = {'kind': 'staticMesh', 'config': {'lodSources': [{'index': 1, 'sourcePath': str(source), 'sourceHash': hashlib.sha256(b'level1').hexdigest()}]}}
            calls = []
            api = types.SimpleNamespace(import_lod=lambda mesh, index, filename: calls.append(index) or index, get_lod_count=lambda mesh: 2)
            with patch.object(module.unreal, 'StaticMeshEditorSubsystem', object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True):
                module.apply_model_lods(object(), entry)
                self.assertEqual(calls, [1])
                source.write_bytes(b'changed')
                with self.assertRaises(RuntimeError):
                    module.apply_model_lods(object(), entry)
                self.assertEqual(calls, [1])
    def test_reject_gap_before_ue_import(self):
        with self.assertRaises(RuntimeError):
            module.validate_lod_sources({'config': {'lodSources': [{'index': 2, 'sourcePath': 'x.fbx'}]}})
    def test_no_levels_does_not_touch_existing_mesh(self):
        module.apply_model_lods(object(), {'kind': 'skeletalMesh', 'config': {}})
