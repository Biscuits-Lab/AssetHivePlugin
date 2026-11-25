"""Mesh-owned Blockout instances using the project's shared Color master."""
import math
import unreal

PARENT_PATH = '/Game/MaterialLibrary/Character/Common/M_Blockout'


def parent():
    obj = unreal.load_asset(PARENT_PATH)
    if not isinstance(obj, unreal.Material):
        raise RuntimeError('Blockout 母材质不存在或类型不正确：' + PARENT_PATH)
    if 'Color' not in [str(n) for n in unreal.MaterialEditingLibrary.get_vector_parameter_names(obj)]:
        raise RuntimeError('Blockout 母材质缺少 Color 向量参数：' + PARENT_PATH)
    return obj


def values(obj):
    if not isinstance(obj, unreal.MaterialInstanceConstant):
        return {'legacyMaterial': True}
    color = unreal.MaterialEditingLibrary.get_material_instance_vector_parameter_value(obj, 'Color')
    p = obj.get_editor_property('parent')
    return {'parent': p.get_path_name() if p else '', 'color': [round(color.r, 6), round(color.g, 6), round(color.b, 6)]}


def validate(appearance):
    color = appearance.get('color', [1, 1, 1])
    if not isinstance(color, list) or len(color) != 3 or not all(isinstance(v, (int, float)) and math.isfinite(v) and v >= 0 for v in color):
        raise RuntimeError('Invalid FBX blockout color')
    parent()


def apply(obj, destination, appearance):
    validate(appearance)
    master = parent()
    if obj is None:
        directory, name = destination.rsplit('/', 1)
        obj = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, directory, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
    if not isinstance(obj, unreal.MaterialInstanceConstant):
        raise RuntimeError('Blockout material destination has an incompatible type')
    mel = unreal.MaterialEditingLibrary
    mel.set_material_instance_parent(obj, master)
    expected = appearance.get('color', [1, 1, 1])
    mel.set_material_instance_vector_parameter_value(obj, 'Color', unreal.LinearColor(*expected, 1))
    mel.update_material_instance(obj)
    actual = mel.get_material_instance_vector_parameter_value(obj, 'Color')
    if any(abs(a - b) > 1e-5 * max(1, abs(b)) for a, b in zip((actual.r, actual.g, actual.b), expected)):
        raise RuntimeError('Blockout Color 参数回读不一致')
    return obj
