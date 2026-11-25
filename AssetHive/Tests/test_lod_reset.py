import types
import unittest
from unittest.mock import patch
from test_suit_import import module


class LodResetTests(unittest.TestCase):
    def test_completed_token_does_not_hide_residual_lods(self):
        for kind, cls in [('staticMesh', 'StaticMeshEditorSubsystem'), ('skeletalMesh', 'SkeletalMeshEditorSubsystem')]:
            entry = {'kind': kind, 'config': {'lodReset': 'new', 'lodResetBaseCount': 1}}
            mesh = types.SimpleNamespace(tags={'AssetHive.LodReset': 'new'})
            api = types.SimpleNamespace(get_lod_count=lambda obj: 6)
            with patch.object(module.unreal, cls, object, create=True), patch.object(module.unreal, 'get_editor_subsystem', return_value=api, create=True):
                self.assertTrue(module.lod_reset_pending(mesh, entry))
                api.get_lod_count = lambda obj: 1
                self.assertFalse(module.lod_reset_pending(mesh, entry))
                api.get_lod_count = lambda obj: 6
                entry['config']['lodSources'] = [{'index': 1}]
                self.assertFalse(module.lod_reset_pending(mesh, entry))
                del entry['config']['lodSources']
                entry['config']['lodResetBaseCount'] = 2
                self.assertFalse(module.lod_reset_pending(mesh, entry))

    def test_final_verification_rejects_residual_lods(self):
        entry = {'config': {'lodReset': 'new', 'lodResetBaseCount': 1}}
        for report in [None, {'count': 6, 'levels': [{'index': 0}, {'index': 1}]}]:
            with self.assertRaisesRegex(RuntimeError, '旧 LOD 清理未完成'):
                module.verify_lod_report(entry, report)


if __name__ == '__main__':
    unittest.main()
