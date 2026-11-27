import importlib.util
import pathlib
import sys
import types
import unittest

spec = importlib.util.spec_from_file_location('association_sync_tested', pathlib.Path(__file__).parents[1] / 'Content/Python/assethive_association_sync.py')
module = importlib.util.module_from_spec(spec)
sys.modules.setdefault('unreal', types.ModuleType('unreal'))
spec.loader.exec_module(module)


class AssociationSyncTests(unittest.TestCase):
    def prepare(self, entries, objects, records=None):
        rows = [types.SimpleNamespace(asset_class_path=types.SimpleNamespace(asset_name='Texture2D'),
                                      asset_name=path.rsplit('/', 1)[-1], package_name=path,
                                      get_tag_value=lambda key, obj=obj: obj.owner) for path, obj in objects.items()]
        module.unreal.AssetRegistryHelpers = types.SimpleNamespace(get_asset_registry=lambda: types.SimpleNamespace(get_assets_by_path=lambda *args, **kwargs: rows))
        api = types.SimpleNamespace(read_registry=lambda: records or {}, KEY='owner', load=objects.get,
                                    EAL=types.SimpleNamespace(get_metadata_tag=lambda obj, key: obj.owner),
                                    legacy_blockout_key_matches=lambda *args: False, receipt=lambda obj: {})
        job = {'associationSync': True, 'suits': [{'entries': entries}]}
        module.prepare(job, api)
        return job

    def entry(self, key='one:D', name='T_Current_D'):
        return {'key': key, 'kind': 'texture', 'name': name, 'targetPath': '/Game/Characters/New/' + name, 'config': {}}

    def obj(self, owner=''):
        return types.SimpleNamespace(owner=owner, get_class=lambda: types.SimpleNamespace(get_name=lambda: 'Texture2D'))

    def test_old_name_and_directory_found_without_content_equality(self):
        entry = self.entry()
        entry['associationNames'] = ['T_Old_D.png']
        self.prepare([entry], {'/Game/Old/T_Old_D': self.obj()})
        self.assertEqual(entry['targetPath'], '/Game/Old/T_Old_D')
        self.assertEqual(entry['associationCanonicalPath'], '/Game/Characters/New/T_Current_D')

    def test_ambiguous_names_do_not_choose_arbitrary_asset(self):
        entry = self.entry()
        self.prepare([entry], {'/Game/A/T_Current_D': self.obj(), '/Game/B/T_Current_D': self.obj()})
        self.assertIn('多个', entry['validationError'])

    def test_registered_identity_wins_over_unrelated_same_name(self):
        entry = self.entry()
        self.prepare([entry], {'/Game/A/T_Current_D': self.obj('one:D'), '/Game/B/T_Current_D': self.obj()})
        self.assertEqual(entry['targetPath'], '/Game/A/T_Current_D')

    def test_other_owner_not_overwritten(self):
        entry = self.entry()
        self.prepare([entry], {'/Game/A/T_Current_D': self.obj('another:D')})
        self.assertIn('另一个', entry['validationError'])

    def test_two_independent_sources_cannot_adopt_same_object(self):
        first, second = self.entry(), self.entry('two:D')
        self.prepare([first, second], {'/Game/A/T_Current_D': self.obj()})
        self.assertIn('共享关系', first['validationError'])
        self.assertIn('共享关系', second['validationError'])

    def test_unknown_object_is_new_and_no_fuzzy_name_match(self):
        entry = self.entry()
        self.prepare([entry], {'/Game/A/T_Current_D_Other': self.obj()})
        self.assertNotIn('associationExistingPath', entry)

    def test_only_explicit_shared_identities_are_merged(self):
        entry = self.entry()
        entry['associationReferenceKeys'] = ['one:D', 'two:D']
        self.prepare([entry], {'/Game/A/T_Current_D': self.obj('one:D'), '/Game/B/T_Old_D': self.obj('two:D'), '/Game/C/T_Current_D': self.obj()})
        self.assertEqual(entry['targetPath'], '/Game/A/T_Current_D')
        self.assertEqual(entry['associationMergePaths'], ['/Game/B/T_Old_D'])

    def test_foreign_shared_receipt_is_not_merged(self):
        entry = self.entry()
        entry['associationReferenceKeys'] = ['one:D', 'two:D']
        self.prepare([entry], {'/Game/A/T_Current_D': self.obj('one:D'), '/Game/B/T_Old_D': self.obj('foreign:D')}, {'two:D': {'path':'/Game/B/T_Old_D'}})
        self.assertIn('归属', entry['validationError'])


if __name__ == '__main__':
    unittest.main()
