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

    def test_face_scatter_binds_verified_sss_vt_parameter(self):
        with patch.object(module.MEL, 'get_texture_parameter_names', return_value=['SSS_VT'], create=True):
            self.assertEqual(module.material_parameters(object(), {'config': {'materialRole': 'hero_face'}}), {'Scatter': 'SSS_VT'})

    def test_item_opacity_binds_separate_masked_template(self):
        import types
        parent = types.SimpleNamespace(get_editor_property=lambda key: 'masked')
        entry = {'config': {'family': 'item', 'parentPath': module.ITEM_MASKED_TEMPLATE, 'virtualTexture': True}, 'textures': {'A': 'a'}}
        with patch.object(module, 'load', return_value=parent), patch.object(module.unreal, 'Material', type(parent), create=True), patch.object(module.unreal, 'BlendMode', types.SimpleNamespace(BLEND_MASKED='masked'), create=True), patch.object(module.MEL, 'get_texture_parameter_names', return_value=['OpacityMasked_VT'], create=True):
            self.assertIs(module.item_parent(entry), parent)
            self.assertEqual(module.material_parameters(parent, entry), {'A': 'OpacityMasked_VT'})
            parent.get_editor_property = lambda key: 'opaque'
            with self.assertRaisesRegex(RuntimeError, 'Masked'):
                module.item_parent(entry)
