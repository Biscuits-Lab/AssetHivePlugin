import json
import types
import unittest
from unittest.mock import patch
from test_suit_import import module


class WaitingRigTests(unittest.TestCase):
    def test_wait_is_persistent_and_new_rig_retries(self):
        obj = types.SimpleNamespace(tags={}, count=6, rig='old')
        entry = {'kind': 'skeletalMesh', 'sourceHash': 'old-file', 'config': {
            'lodPolicy': 'lod1-reduction-v2', 'skinLodSource': {
                'sourcePath': 'lod1.fbx', 'sourceHash': 'new-static', 'sourceLod': 0}}}
        builds = []
        def native(mesh, path, stamp, level, build, materials):
            if build:
                builds.append(mesh.rig)
                if mesh.rig == 'old':
                    return json.dumps({'error': 'SM LOD1 is not aligned with SK LOD0; skin transfer rejected (example)'})
                mesh.count = 6
            return json.dumps({'rigHash': mesh.rig, 'count': mesh.count,
                               'pending': [1, 2, 3, 4, 5], 'levels': [{'index': i} for i in range(mesh.count)]})
        library = types.SimpleNamespace(complete_skinned_lods_from_static=native)
        with patch.object(module.unreal, 'AssetHiveModelCatalogLibrary', library, create=True), \
             patch.object(module.EAL, 'set_metadata_tag', lambda o,k,v: o.tags.__setitem__(k,v), create=True), \
             patch.object(module.EAL, 'remove_metadata_tag', lambda o,k: o.tags.pop(k,None), create=True), \
             patch.object(module, 'reset_model_lods', lambda o,e: setattr(o,'count',1)):
            report = module.lod_policy_report(obj, entry, True)
            self.assertTrue(report['waitingForRig'])
            module.verify_lod_report(entry, report)
            for regenerate in [False, True]:
                self.assertTrue(module.lod_policy_report(obj, entry, regenerate)['waitingForRig'])
            self.assertEqual(builds, ['old'])
            obj.rig = 'new'
            entry['sourceHash'] = 'new-file'
            report = module.lod_policy_report(obj, entry, True)
            self.assertNotIn('waitingForRig', report)
            self.assertEqual(builds, ['old','new'])
            self.assertEqual(obj.tags, {})

    def test_wait_cannot_hide_residual_lods(self):
        entry = {'kind':'skeletalMesh', 'config':{'skinLodSource':{'sourceHash':'new'}}}
        with self.assertRaises(RuntimeError):
            module.verify_lod_report(entry, {'waitingForRig':True,'count':6,'levels':[{'index':0}]})


if __name__ == '__main__':
    unittest.main()
