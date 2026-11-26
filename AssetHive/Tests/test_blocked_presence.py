import unittest
from unittest.mock import patch
from test_suit_import import module, Asset


class BlockedPresenceTests(unittest.TestCase):
    def test_linked_skeleton_failure_still_reports_existing_mesh(self):
        entry = {'key': 'owner:skeletalMesh:head', 'name': 'SK_Head', 'kind': 'skeletalMesh',
                 'targetPath': '/Game/Test/SK_Head', 'skeletonPath': '/Game/Test/SKEL_Head',
                 'config': {'ownSkeleton': True}}
        saved = {**entry, 'path': entry['targetPath'], 'linkedSkeletonPath': '/Game/Shared/SKEL'}
        obj = Asset(entry['key'], entry['targetPath'], saved)
        with patch.object(module, 'read_registry', return_value={entry['key']: saved}), \
                patch.object(module, 'load', return_value=obj), \
                patch.object(module, 'owned_objects', return_value={}):
            row = module.inspect({'suits': [{'assetId': 'owner', 'entries': [entry]}]})['entries'][0]
            self.assertEqual(row['status'], 'blocked')
            self.assertEqual(row['action'], 'updated')

    def test_validation_failure_preserves_existing_missing_and_new_distinction(self):
        entry = {'key': 'owner:skeletalMesh:head', 'name': 'SK_Head', 'kind': 'skeletalMesh',
                 'targetPath': '/Game/Test/SK_Head', 'config': {}}
        obj = Asset(entry['key'], entry['targetPath'], entry)
        job = {'suits': [{'assetId': 'owner', 'entries': [entry]}]}
        for existing, saved, expected in [(obj, True, 'updated'), (None, True, 'restore'), (None, False, 'added')]:
            with self.subTest(expected=expected), \
                    patch.object(module, 'read_registry', return_value={entry['key']: {'path': entry['targetPath']}} if saved else {}), \
                    patch.object(module, 'load', return_value=existing), \
                    patch.object(module, 'owned_objects', return_value={}), \
                    patch.object(module, 'validate_entry', side_effect=RuntimeError('material or skeleton blocked')):
                row = module.inspect(job)['entries'][0]
                self.assertEqual(row['status'], 'blocked')
                self.assertEqual(row['action'], expected)
                if existing:
                    self.assertEqual(row['path'], entry['targetPath'])
