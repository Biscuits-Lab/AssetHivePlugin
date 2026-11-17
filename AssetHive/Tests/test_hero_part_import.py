import unittest
from unittest.mock import patch
from test_suit_import import module

class HeroMaterialTests(unittest.TestCase):
    def test_hair_channels_use_verified_parameters(self):
        names = ['VT_Alpha','VT_Root','VT_Unique_Hair_ID','VT_Depth','VT_FlowMap','VT_DiffuseColor']
        with patch.object(module.MEL, 'get_texture_parameter_names', return_value=names, create=True):
            mapping = module.material_parameters(object(), {'config':{'materialRole':'hero_hair'}})
        self.assertEqual(mapping, {'D':'VT_DiffuseColor','Opacity':'VT_Alpha','Root':'VT_Root','ID':'VT_Unique_Hair_ID','Height':'VT_Depth','Tangent':'VT_FlowMap'})

    def test_face_and_eye_do_not_inherit_suit_mapping(self):
        with patch.object(module.MEL, 'get_texture_parameter_names', return_value=['Diffuse_VT','Normal_VT','ORM_VT','IrisColorTex'], create=True):
            self.assertEqual(module.material_parameters(object(), {'config':{'materialRole':'hero_face'}}), {'D':'Diffuse_VT','N':'Normal_VT','ORM':'ORM_VT'})
            self.assertEqual(module.material_parameters(object(), {'config':{'materialRole':'hero_eye'}}), {'D':'IrisColorTex'})
