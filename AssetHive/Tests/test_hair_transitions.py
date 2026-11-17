import unittest,types
from unittest.mock import patch
from test_suit_import import module

class HairTransitionTests(unittest.TestCase):
    def test_new_diffuse_replaces_automatic_fallback_without_resetting_custom_colour(self):
        colours={'Root Color':types.SimpleNamespace(r=.02,g=.02,b=.02,a=1),'Tip Color':types.SimpleNamespace(r=.1,g=.2,b=.3,a=1)}
        with patch.object(module.MEL,'get_material_instance_scalar_parameter_value',return_value=0,create=True), patch.object(module.MEL,'get_material_instance_vector_parameter_value',side_effect=lambda obj,n:colours[n],create=True), patch.object(module.MEL,'set_material_instance_scalar_parameter_value',create=True) as scalar, patch.object(module.MEL,'set_material_instance_vector_parameter_value',create=True) as vector, patch.object(module.unreal,'LinearColor',side_effect=lambda *v:v,create=True):
            module.configure_hair_instance(None,{'config':{'materialRole':'hero_hair','hairDefaultsVersion':1},'textures':{'D':'colour'}},{'config':{'hairDefaultsVersion':1},'textures':{}})
            scalar.assert_called_once_with(None,'Diffuse Tex Blend Intensity',1)
            vector.assert_called_once_with(None,'Root Color',(1,1,1,1))
    def test_hand_tuned_blend_is_retained_when_colour_map_changes(self):
        with patch.object(module.MEL,'get_material_instance_scalar_parameter_value',return_value=.4,create=True), patch.object(module.MEL,'set_material_instance_scalar_parameter_value',create=True) as scalar:
            module.configure_hair_instance(None,{'config':{'materialRole':'hero_hair','hairDefaultsVersion':1},'textures':{'D':'colour'}},{'config':{'hairDefaultsVersion':1},'textures':{}})
            scalar.assert_not_called()
