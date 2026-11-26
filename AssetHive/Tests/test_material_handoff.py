import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'Content/Python'))
from assethive_material_handoff import prepare


class MaterialHandoffTests(unittest.TestCase):
    def fixture(self):
        sk = {'key': 'owner:skeletalMesh:head', 'kind': 'skeletalMesh', 'name': 'SK_Head',
              'sourceHash': 'new', 'targetPath': '/Game/Test/SK_Head', 'config': {'family': 'suit'},
              'materials': {}, 'unresolvedMaterials': ['MI_Rig_head_01', 'lambert2'],
              'materialHandoff': {'slots': ['MI_Rig_head_01', 'lambert2'],
                                  'blockoutRoot': '/Game/Test/Material/Blockout',
                                  'appearances': {'lambert2': {'color': [1, 0, 0]}}}}
        sm = {'key': 'owner:staticMesh:head', 'config': {'materialMode': 'blockout'},
              'materials': {'MI_Art_head_01': 'existing-mi'}}
        return sk, sm, {'suits': [{'entries': [sk]}]}

    def test_new_rig_consumes_sent_blockout_not_global_formal_mode(self):
        sk, sm, job = self.fixture()
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None, lambda key: '/Game/ExistingMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['externalMaterials'], {'MI_Rig_head_01': '/Game/ExistingMI'})
        self.assertEqual(sk['unresolvedMaterials'], [])
        self.assertEqual(sk['config']['materialMode'], 'blockout')
        self.assertEqual(job['suits'][0]['entries'][0]['config']['blockoutMaterial']['color'], [1, 0, 0])

    def test_sm_upgrade_never_changes_unchanged_sk(self):
        sk, sm, job = self.fixture()
        saved = copy.deepcopy(sk)
        saved['materials'] = {'MI_Rig_head_01': 'old-mi'}
        saved['unresolvedMaterials'] = []
        saved['config']['materialMode'] = 'blockout'
        sm['config'] = {}
        prepare(job, lambda key, target: saved if ':skeletalMesh:' in key else sm, lambda key: '/Game/FormalMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['materials'], saved['materials'])
        self.assertEqual(sk['config']['materialMode'], 'blockout')
        self.assertFalse(sk['adoptStaticMaterials'])

    def test_formal_requires_matching_slot_not_arbitrary_order(self):
        sk, sm, job = self.fixture()
        sm['config'] = {}
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None, lambda key: '/Game/FormalMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['externalMaterials']['MI_Rig_head_01'], '/Game/FormalMI')
        self.assertEqual(sk['unresolvedMaterials'], ['lambert2'])
        self.assertEqual(len(job['suits'][0]['entries']), 1)

    def test_inspection_is_repeatable_and_never_mutates_receipt(self):
        sk, sm, job = self.fixture()
        before = copy.deepcopy(sm)
        for _ in range(2):
            prepare(job, lambda key, target: sm if ':staticMesh:' in key else None, lambda key: '/Game/ExistingMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sm, before)
        self.assertEqual(len(job['suits'][0]['entries']), 2)

    def test_matching_names_without_matching_geometry_do_not_borrow_materials(self):
        sk, sm, job = self.fixture()
        sm['config'] = {}
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None,
                lambda key: '/Game/FormalMI', lambda entry, static: {})
        self.assertEqual(sk['externalMaterials'], {})
        self.assertEqual(sk['unresolvedMaterials'], sk['materialHandoff']['slots'])

    def test_blockout_geometry_mismatch_keeps_own_fbx_colours(self):
        sk, sm, job = self.fixture()
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None,
                lambda key: '/Game/ExistingMI', lambda entry, static: {})
        self.assertEqual(sk['externalMaterials'], {})
        self.assertEqual(len(sk['materials']), 2)
        self.assertEqual(sk['unresolvedMaterials'], [])
