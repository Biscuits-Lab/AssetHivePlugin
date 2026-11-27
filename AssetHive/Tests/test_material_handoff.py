import copy
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'Content/Python'))
from assethive_material_handoff import prepare


class MaterialHandoffTests(unittest.TestCase):
    def test_existing_desktop_blockout_identity_and_colour_survive_inspection(self):
        sk, sm, job = self.fixture()
        material = {'key': 'desktop-stable-key', 'kind': 'material', 'name': 'MI_Desktop',
                    'config': {'blockoutMaterial': {'color': [0.1, 0.2, 0.3]}}}
        sk['materials']['lambert2'] = material['key']
        job['suits'][0]['entries'].append(material)
        for _ in range(2):
            prepare(job, lambda key, target: sm if ':staticMesh:' in key else None,
                    lambda key: '/Game/ExistingMI', lambda entry, static: {})
        self.assertEqual(sk['materials']['lambert2'], material['key'])
        self.assertEqual([item for item in job['suits'][0]['entries'] if item['key'] == material['key']], [material])

    def test_explicit_blockout_with_old_formal_sm_allows_unmatched_slots(self):
        sk, sm, job = self.fixture()
        sk['config']['materialMode'] = 'blockout'
        sm['config'] = {}
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None,
                lambda key: '/Game/FormalMI', lambda entry, static: {})
        self.assertEqual(sk['unresolvedMaterials'], [])
        self.assertEqual(len(sk['materials']), 2)
        self.assertEqual(sk['config']['materialMode'], 'blockout')

    def test_explicit_blockout_does_not_restore_stale_formal_failure(self):
        sk, sm, job = self.fixture()
        saved = copy.deepcopy(sk)
        sk['config']['materialMode'] = 'blockout'
        sm['config'] = {}
        prepare(job, lambda key, target: saved if ':skeletalMesh:' in key else sm,
                lambda key: '/Game/FormalMI', lambda entry, static: {})
        self.assertEqual(sk['unresolvedMaterials'], [])
        self.assertEqual(len(sk['materials']), 2)

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

    def test_sm_upgrade_updates_unchanged_sk_appearance(self):
        sk, sm, job = self.fixture()
        saved = copy.deepcopy(sk)
        saved['materials'] = {'MI_Rig_head_01': 'old-mi'}
        saved['unresolvedMaterials'] = []
        saved['config']['materialMode'] = 'blockout'
        sm['config'] = {}
        prepare(job, lambda key, target: saved if ':skeletalMesh:' in key else sm, lambda key: '/Game/FormalMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['materials'], {})
        self.assertNotIn('materialMode', sk['config'])
        self.assertTrue(sk['adoptStaticMaterials'])
        self.assertEqual(sk['sourceHash'], saved['sourceHash'])
        self.assertEqual(sk['externalMaterials'], {'MI_Rig_head_01': '/Game/FormalMI'})
        self.assertEqual(sk['unresolvedMaterials'], ['lambert2'])

    def test_same_send_uses_new_sm_mapping_and_tracks_its_dependencies(self):
        sk, sm, job = self.fixture()
        sm['sourceHash'] = 'geometry'
        planned = {**copy.deepcopy(sm), 'kind': 'staticMesh', 'config': {},
                   'materials': {'MI_Art_head_01': 'new-mi'}}
        job['suits'][0]['entries'].insert(0, planned)
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None,
                lambda key: '/Game/' + key, lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['materials'], {'MI_Rig_head_01': 'new-mi'})
        self.assertNotIn('materialMode', sk['config'])
        self.assertEqual(sm['materials'], {'MI_Art_head_01': 'existing-mi'})

    def test_first_send_uses_verified_source_geometry_without_existing_sm(self):
        sk, sm, job = self.fixture()
        sm.update(kind='staticMesh', sourcePath='/source/SM.fbx', sourceHash='new')
        sm['config'] = {}
        job['suits'][0]['entries'].insert(0, sm)
        seen = []
        def geometry(entry, reference):
            seen.append(reference)
            return {'MI_Rig_head_01': ['MI_Art_head_01']}
        prepare(job, lambda key, target: None, lambda key: '/Game/MI_New', geometry)
        self.assertEqual(seen, [sm])
        self.assertEqual(sk['materials'], {'MI_Rig_head_01': 'existing-mi'})
        self.assertEqual(sk['unresolvedMaterials'], ['lambert2'])

    def test_formal_requires_matching_slot_not_arbitrary_order(self):
        sk, sm, job = self.fixture()
        sm['config'] = {}
        prepare(job, lambda key, target: sm if ':staticMesh:' in key else None, lambda key: '/Game/FormalMI', lambda entry, static: {'MI_Rig_head_01': ['MI_Art_head_01']})
        self.assertEqual(sk['externalMaterials']['MI_Rig_head_01'], '/Game/FormalMI')
        self.assertEqual(sk['unresolvedMaterials'], ['lambert2'])
        self.assertEqual(len(job['suits'][0]['entries']), 1)

    def test_verified_desktop_material_mapping_is_not_erased_by_static_geometry_mismatch(self):
        sk, sm, job = self.fixture()
        sm['config'] = {}
        sk['materials'] = {'MI_Rig_head_01': 'explicit-material'}
        sk['externalMaterials'] = {'lambert2': '/Game/Shared/Body'}
        prepare(job, lambda key, target: sm, lambda key: '/Game/FormalMI', lambda entry, static: {})
        self.assertEqual(sk['materials'], {'MI_Rig_head_01': 'explicit-material'})
        self.assertEqual(sk['externalMaterials'], {'lambert2': '/Game/Shared/Body'})
        self.assertEqual(sk['unresolvedMaterials'], [])

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
