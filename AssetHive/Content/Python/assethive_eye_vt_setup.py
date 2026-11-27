"""为 AssetHive 建立独立眼睛 VT 母材质，保留原虹膜视差函数。"""
import unreal

SOURCE = '/Game/MaterialLibrary/Character/Common/M_Char_Eye_CustomIris'
TARGET = SOURCE + '_VT'
OWNER = 'AssetHive.EyeVTSetup'


def build_eye_vt_parent():
    """复制眼睛颜色/法线默认图并转 VT，不改动项目已有母材质。"""
    eal = unreal.EditorAssetLibrary
    mel = unreal.MaterialEditingLibrary
    parent = eal.load_asset(SOURCE)
    if not parent:
        raise RuntimeError('缺少项目眼睛母材质，请联系 TA')
    existed = eal.does_asset_exist(TARGET)
    target = eal.load_asset(TARGET) if existed else eal.duplicate_asset(SOURCE, TARGET)
    if not target:
        raise RuntimeError('无法创建眼睛 VT 母材质')
    owner = str(eal.get_metadata_tag(target, OWNER))
    if existed and owner != '1':
        raise RuntimeError('眼睛 VT 母材质版本不一致，请联系 TA')
    eal.set_metadata_tag(target, OWNER, '1')
    parameters = {'IrisColorTex': unreal.MaterialSamplerType.SAMPLERTYPE_VIRTUAL_COLOR,
                  'IrisEmissiveColorTex': unreal.MaterialSamplerType.SAMPLERTYPE_VIRTUAL_COLOR,
                  'ScleraNormal': unreal.MaterialSamplerType.SAMPLERTYPE_VIRTUAL_NORMAL}
    changed = []
    for expression in mel.get_material_expressions(target):
        if not isinstance(expression, unreal.MaterialExpressionTextureSampleParameter2D):
            continue
        name = str(expression.get_editor_property('parameter_name'))
        if name not in parameters:
            continue
        source = expression.get_editor_property('texture')
        if not source:
            raise RuntimeError('眼睛母材质缺少默认贴图：' + name)
        destination = '/Game/MaterialLibrary/Character/Common/Shared/EYE/AssetHiveVT/T_' + name + '_VT'
        texture_existed = eal.does_asset_exist(destination)
        texture = eal.load_asset(destination) if texture_existed else eal.duplicate_asset(source.get_path_name(), destination)
        if not texture:
            raise RuntimeError('无法创建眼睛 VT 默认贴图：' + name)
        if texture_existed and str(eal.get_metadata_tag(texture, OWNER)) != '1':
            raise RuntimeError('眼睛默认贴图位置已被其他资源占用，请联系 TA：' + destination)
        texture.set_editor_property('virtual_texture_streaming', True)
        eal.set_metadata_tag(texture, OWNER, '1')
        if not eal.save_loaded_asset(texture, False):
            raise RuntimeError('眼睛 VT 默认贴图保存失败：' + name)
        expression.set_editor_property('texture', texture)
        expression.set_editor_property('sampler_type', parameters[name])
        changed.append(name)
    if set(changed) != set(parameters):
        raise RuntimeError('项目眼睛母材质结构已变化，请联系 TA 核对参数')
    errors = mel.recompile_material(target)
    if errors:
        raise RuntimeError('眼睛 VT 母材质编译失败：' + '; '.join(str(error) for error in errors))
    stats = mel.get_statistics(target)
    if stats.num_virtual_texture_samples < 2:
        raise RuntimeError('眼睛 VT 采样未生效，未保存母材质')
    eal.set_metadata_tag(target, OWNER, '1')
    if not eal.save_loaded_asset(target, False):
        raise RuntimeError('眼睛 VT 母材质保存失败')
    return {'parent': TARGET, 'parameters': changed, 'virtualSamples': stats.num_virtual_texture_samples,
            'pixelInstructions': stats.num_pixel_shader_instructions, 'customHeightSupported': False}
