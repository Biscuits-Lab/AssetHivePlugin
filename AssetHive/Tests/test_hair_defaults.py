import unittest, types
from unittest.mock import patch
from test_suit_import import module

class HairDefaultsTests(unittest.TestCase):
    def invoke(self, textures, previous=None, scalar_names=(), vector_names=()):
        def props(name):
            names=scalar_names if name=='scalar_parameter_values' else vector_names
            return [types.SimpleNamespace(parameter_info=types.SimpleNamespace(name=n)) for n in names]
        obj=types.SimpleNamespace(get_editor_property=props)
        entry={'config':{'materialRole':'hero_hair','hairDefaultsVersion':1},'textures':textures}
        with patch.object(module.MEL,'set_material_instance_scalar_parameter_value',create=True) as s, patch.object(module.MEL,'set_material_instance_vector_parameter_value',create=True) as v, patch.object(module.unreal,'LinearColor',side_effect=lambda *args:args,create=True):
            module.configure_hair_instance(obj,entry,previous or {})
            return [(call.args[1],call.args[2]) for call in s.call_args_list],[(call.args[1],call.args[2]) for call in v.call_args_list]
    def test_data_maps_without_diffuse_get_colour_fallback(self):
        scalars,vectors=self.invoke({'Opacity':'a','Root':'r','ID':'i','Height':'h','Tangent':'t'})
        self.assertIn(('Diffuse Tex Blend Intensity',0),scalars)
        self.assertIn(('ISEdgeMask',0),scalars)
        self.assertEqual(dict(vectors)['Root Color'],(.02,.02,.02,1))
    def test_resend_keeps_artist_changes(self):
        self.assertEqual(self.invoke({}, {'config':{'hairDefaultsVersion':1}}),([],[]))
    def test_legacy_material_explicit_overrides_survive_migration(self):
        self.assertEqual(self.invoke({},scalar_names=['Diffuse Tex Blend Intensity','ISEdgeMask'],vector_names=['Root Color','Tip Color']),([],[]))
    def test_diffuse_material_keeps_colour_input(self):
        scalars,vectors=self.invoke({'D':'colour'})
        self.assertEqual(scalars,[('ISEdgeMask',0)])
        self.assertEqual(vectors,[])
