"""Editor-only Suit import. Invoked by AssetHive's project-checked TCP dispatcher."""
import hashlib
import json
import os
import re
import traceback
import time
import unreal

EAL = unreal.EditorAssetLibrary
MEL = unreal.MaterialEditingLibrary
KEY = 'AssetHive.SuitKey'
STAMP = 'AssetHive.SuitReceipt'
ROOT = '/Game/Characters/Hero/Asset_PJZ/'
WEAPON_ROOT = '/Game/Characters/Weapon_PJZ/'
MONSTER_ROOT = '/Game/Characters/Monster/'
NPC_ROOT = '/Game/Characters/NPC/'
ITEM_ROOT = '/Game/Characters/GameplayItem/'
ITEM_TEMPLATE = '/Game/MaterialLibrary/Character/Common/M_GamePlayItem_Base_VT'
ITEM_MASKED_TEMPLATE = '/Game/MaterialLibrary/Character/Common/M_GamePlayItem_Base_Masked_VT'
MONSTER_CLOTH_TEMPLATE = '/Game/MaterialLibrary/Character/Common/M_Char_Monster_ComplexShading'
MONSTER_SKIN_TEMPLATE = '/Game/MaterialLibrary/Character/Common/M_Char_Monster_skin'
TEMPLATE = '/Game/MaterialLibrary/Character/MI_Suit_ComplexShading/'
WEAPON_TEMPLATE = '/Game/MaterialLibrary/Weapon/Common/M_Weapon_Slab'
MONSTER_WEAPON_TEMPLATE = '/Game/MaterialLibrary/Weapon/Common/M_Weapon_Monster'


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest()


def load(path):
    return EAL.load_asset(path) if path and EAL.does_asset_exist(path) else None


def object_path(obj):
    return obj.get_path_name() if obj else ''


def slot_name(slot):
    # ImportedMaterialSlotName is editor-only, not a Python attribute on StaticMaterial.
    imported = slot.get_editor_property('imported_material_slot_name')
    return str(imported) if imported and str(imported) != 'None' else str(slot.material_slot_name)


def mapped_slot_name(name, entry):
    """Resolve UE FBX dot sanitization only when the source slot is unique."""
    names = set(entry.get('materials', {})) | set(entry.get('externalMaterials', {}))
    if name in names:
        return name
    matches = [source for source in names if source.replace('.', '_') == name]
    return matches[0] if len(matches) == 1 else name


def receipt(obj, previous=None):
    try:
        saved = json.loads(EAL.get_metadata_tag(obj, STAMP) or '{}')
    except ValueError:
        saved = {}
    if previous and previous.get('linkOnly') and same_ue_package(previous.get('path'), object_path(obj)):
        # The asset carries the consumed link revision across workspaces. A
        # fresh checkout has no Saved registry and must not resurrect that link.
        if previous.get('linkRevision') and saved.get('linkRevision') == previous['linkRevision']:
            return saved
        return previous
    return saved


def same_entry_content(saved, entry):
    """Local source locations are transport details, never resource identity."""
    if not saved or saved.get('key') != entry.get('key') or saved.get('kind') != entry.get('kind'):
        return False
    def config(value):
        result = dict(value or {})
        if 'lodSources' in result:
            result['lodSources'] = [{k: v for k, v in lod.items() if k != 'sourcePath' or not lod.get('sourceHash')}
                                    for lod in result['lodSources']]
        if result.get('skinLodSource', {}).get('sourceHash'):
            result['skinLodSource'] = {k: v for k, v in result['skinLodSource'].items() if k != 'sourcePath'}
        return result
    return (bool(saved.get('sourceHash') or saved.get('fingerprint'))
            and config(saved.get('config')) == config(entry.get('config'))
            and all(saved.get(field) == entry.get(field) for field in
                    ('sourceHash', 'skeletonPath', 'materials', 'externalMaterials', 'textures', 'heroSharedRoles')))


def legacy_export_matches(entry, old_key, saved):
    """Match only the same exported resource after its AssetHive owner ID changed."""
    if not old_key or ':' not in old_key or ':' not in entry.get('key', ''):
        return False
    return (old_key.split(':', 1)[1] == entry['key'].split(':', 1)[1]
            and saved.get('kind') == entry.get('kind')
            and saved.get('name') == entry.get('name')
            and bool(saved.get('targetPath'))
            and same_ue_package(saved['targetPath'], entry.get('targetPath')))


def reassociated_receipt(saved, old_key, new_key):
    """Only replace owner identities, preserving UE's last applied values."""
    previous = old_key.split(':', 1)[0] + ':'
    current = new_key.split(':', 1)[0] + ':'
    result = dict(saved)
    for field in ('textures', 'materials'):
        if field in saved:
            result[field] = {slot: current + key[len(previous):] if key.startswith(previous) else key
                             for slot, key in saved[field].items()}
    return result


def save(obj, entry):
    EAL.set_metadata_tag(obj, KEY, entry['key'])
    EAL.set_metadata_tag(obj, STAMP, json.dumps(entry, ensure_ascii=False))
    if not EAL.save_loaded_asset(obj, only_if_is_dirty=False):
        raise RuntimeError('UE asset save failed: ' + object_path(obj))


def registry_path():
    return os.path.join(unreal.Paths.project_saved_dir(), 'AssetHive', 'SuitImports.json')


def read_registry():
    path = registry_path()
    records = {}
    links = os.path.join(unreal.Paths.project_dir(), 'Config', 'AssetHive', 'Links.json')
    if os.path.isfile(path):
        with open(path, encoding='utf-8') as source:
            records.update(json.load(source))
    if os.path.isfile(links):
        with open(links, encoding='utf-8') as source:
            for key, linked in json.load(source).items():
                revision = linked.get('linkRevision') or digest(linked)
                # A newly verified association supersedes old imports/UObject
                # stamps. A later send consumes that revision and becomes current.
                if records.get(key, {}).get('linkRevision') != revision:
                    records[key] = {**linked, 'linkRevision': revision}
    # Bone arrays belong to this inspection, not the persistent identity receipt.
    # Old versions stored both full hierarchies on every mesh and rewrote them
    # after each imported resource. Drop only these derived validation fields.
    for record in records.values():
        record.pop('bones', None)
        record.pop('targetBones', None)
    return records


def write_registry(records):
    path = registry_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + '.tmp', 'w', encoding='utf-8') as output:
        json.dump(records, output, ensure_ascii=False)
    os.replace(path + '.tmp', path)


def owned_objects(keys):
    """Read registry identity tags; only load matches, including moved/duplicated objects."""
    result = {key: [] for key in keys}
    if not keys:
        return result
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for data in registry.get_assets_by_path('/Game', recursive=True):
        key = str(data.get_tag_value(KEY) or '')
        if key in result:
            result[key].append(data.get_asset())
    return result


def material_parameters(parent, entry=None):
    names = [str(name) for name in MEL.get_texture_parameter_names(parent)]
    if entry and item_material_entry(entry):
        mapping = {'D': 'Diffuse_VT', 'N': 'Normal_VT', 'ORM': 'ORM_VT', 'E': 'Emissive_Tex_VT', 'A': 'OpacityMasked_VT'}
        return {channel: name for channel, name in mapping.items() if name in names}
    if entry and entry.get('config', {}).get('materialRole', '').startswith('hero_'):
        role = entry['config']['materialRole']
        mapping = ({'D': 'VT_DiffuseColor', 'Opacity': 'VT_Alpha', 'Root': 'VT_Root', 'ID': 'VT_Unique_Hair_ID', 'Height': 'VT_Depth', 'Tangent': 'VT_FlowMap'}
                   if role in ('hero_hair', 'hero_eyebrow') else
                   {'D': 'IrisColorTex', 'N': 'ScleraNormal', 'Height': 'IrisHeight', 'E': 'IrisEmissiveColorTex'} if role == 'hero_eye' else
                   {'D': 'Diffuse_VT', 'N': 'Normal_VT', 'ORM': 'ORM_VT', 'Opacity': 'OpacityMasked_VT', 'E': 'Emissive_VT', 'Scatter': 'SSS_VT'})
        return {channel: name for channel, name in mapping.items() if name in names}
    # Verified against Zion M_Char_Suit_ComplexShading via the editor, not guessed from texture order.
    role = entry.get('config', {}).get('materialRole') if entry else ''
    mapping = ({'D': 'Diffuse', 'N': 'Normal', 'ORM': 'ORM', 'A': 'OpacityMasked', 'E': 'Emissive'}
               if entry and weapon_material_entry(entry) else
               {'D': 'Diffuse_VT', 'N': 'Normal_VT', 'ORM': 'ORM_VT', 'A': 'OpacityMasked_VT', 'E': 'Emissive_VT'}
               if role == 'monsterSkin' else
               {'D': 'VT_Diffuse', 'N': 'VT_Normal', 'ORM': 'VT_ORM', 'A': 'VT_OpacityMasked', 'E': 'VT_Emissive'})
    return {channel: name if name in names else channel for channel, name in mapping.items() if name in names or channel in names}


def weapon_entry(entry):
    return entry.get('config', {}).get('family') == 'weapon'


def item_material_entry(entry):
    return entry.get('config', {}).get('family') == 'item'


def item_parent(entry):
    config = entry.get('config', {})
    parent = load(config.get('parentPath', ''))
    expected = ITEM_MASKED_TEMPLATE if 'A' in entry.get('textures', {}) else ITEM_TEMPLATE
    if config.get('parentPath', '').split('.')[0] != expected or not isinstance(parent, unreal.Material):
        raise RuntimeError('Gameplay Item 母材质缺失或路径不正确：' + expected)
    if 'A' in entry.get('textures', {}) and parent.get_editor_property('blend_mode') != unreal.BlendMode.BLEND_MASKED:
        raise RuntimeError('Gameplay Item 透贴母材质必须使用 Masked 模式')
    missing = set(entry.get('textures', {})) - set(material_parameters(parent, entry))
    if missing:
        raise RuntimeError('当前 Gameplay Item 母材质不支持这些贴图通道：' + ', '.join(sorted(missing)))
    if not config.get('virtualTexture'):
        raise RuntimeError('Gameplay Item 母材质使用 VT 采样，贴图必须启用虚拟纹理')
    if 'E' in entry.get('textures', {}) and 'UseEmissive' not in {str(name) for name in MEL.get_static_switch_parameter_names(parent)}:
        raise RuntimeError('Gameplay Item 母材质缺少 UseEmissive 开关')
    return parent


def is_default_mesh_material(material):
    return not material or object_path(material).split('.')[0] in (
        '/Engine/EngineMaterials/DefaultMaterial', '/Engine/EngineMaterials/WorldGridMaterial')


def deferred_item_material_conflicts(obj, entry):
    if entry['kind'] not in ('staticMesh', 'skeletalMesh') or entry.get('config', {}).get('family') != 'item':
        return []
    if not receipt(obj).get('config', {}).get('deferMaterials') or not entry['config'].get('itemMaterialVersion'):
        return []
    slots = obj.get_editor_property('static_materials' if entry['kind'] == 'staticMesh' else 'materials')
    return [slot_name(slot) for slot in slots if (slot_name(slot) in entry.get('materials', {}) or slot_name(slot) in entry.get('externalMaterials', {})) and not is_default_mesh_material(slot.material_interface)]


def weapon_material_entry(entry):
    config = entry.get('config', {})
    return weapon_entry(entry) or (config.get('family') in ('monster', 'npc') and config.get('materialRole') == 'monsterWeapon')


def monster_material_entry(entry):
    config = entry.get('config', {})
    return config.get('family') in ('monster', 'npc') and config.get('materialRole') in ('monsterCloth', 'monsterSkin')


def owned_skeleton_entry(entry):
    config = entry.get('config', {})
    return config.get('ownSkeleton', False) and (config.get('family') in ('weapon', 'monster', 'npc', 'item') or config.get('skeletonOverride') == 'create')


def validate_weapon_helper(entry, path, key, expected_type, deleted_owners=()):
    root = entry['targetPath'].split('/Mesh/')[0].split('/Material/')[0]
    if not path.startswith(root + '/') or any(p in ('', '.', '..') for p in path[1:].split('/')):
        raise RuntimeError('武器依赖路径不属于当前武器：' + path)
    obj = load(path)
    if obj:
        owner = str(EAL.get_metadata_tag(obj, KEY))
        reassociate = (':' in owner and owner.split(':', 1)[0] in deleted_owners
                       and owner.split(':', 1)[1] == key.split(':', 1)[1])
        if not isinstance(obj, expected_type) or (owner != key and not reassociate):
            raise RuntimeError('武器依赖名称已被占用：' + path)
    return obj


def weapon_physics_enabled(entry):
    config = entry.get('config', {})
    return entry['kind'] == 'skeletalMesh' and config.get('family') in ('weapon', 'monster', 'npc', 'item') and config.get('createPhysicsAsset', False)


def weapon_physics_for_import(entry, mesh):
    # Keep existing, possibly hand-tuned or shared physics exactly as assigned.
    assigned = mesh.get_editor_property('physics_asset') if mesh else None
    return assigned or load(entry['config'].get('physicsAssetPath', ''))


def save_weapon_physics(entry, mesh, previous_physics):
    physics = mesh.get_editor_property('physics_asset')
    if previous_physics and physics != previous_physics:
        mesh.set_editor_property('physics_asset', previous_physics)
        physics = previous_physics
    if not physics:
        # Interchange reimport may ignore FbxImportUI.create_physics_asset.
        # The editor subsystem creates bodies without a modal factory dialog.
        default_path = object_path(mesh).split('.')[0] + '_PhysicsAsset'
        if load(default_path):
            raise RuntimeError('Physics asset generation path already exists: ' + default_path)
        physics = unreal.SkeletalMeshEditorSubsystem.create_physics_asset(mesh, True, 0)
    if not isinstance(physics, unreal.PhysicsAsset):
        raise RuntimeError('武器物理资产未生成，请检查模型骨骼和尺寸')
    key = entry['key'] + ':physics'
    owned = str(EAL.get_metadata_tag(physics, KEY)) == key
    if previous_physics and not owned:
        return  # Never rename or overwrite an existing user-assigned physics asset.
    target = entry['config']['physicsAssetPath']
    if object_path(physics).split('.')[0] != target:
        if not EAL.rename_asset(object_path(physics), target):
            raise RuntimeError('武器物理资产规范命名失败')
    if not previous_physics or owned:
        save(physics, {'key': key, 'kind': 'weaponPhysics'})


def synchronize_owned_helpers(entry, mesh):
    """Pure mesh renames must also move owned helpers; never move shared/user assets."""
    if entry.get('kind') != 'skeletalMesh':
        return
    helpers = []
    if owned_skeleton_entry(entry):
        helpers.append(('skeleton', entry.get('skeletonPath'), ':skeleton'))
    if weapon_physics_enabled(entry):
        helpers.append(('physics_asset', entry.get('config', {}).get('physicsAssetPath'), ':physics'))
    for prop, target, suffix in helpers:
        helper = mesh.get_editor_property(prop)
        if not helper or not target or str(EAL.get_metadata_tag(helper, KEY)) != entry['key'] + suffix:
            continue
        if object_path(helper).split('.')[0] == target:
            continue
        collision = load(target)
        if collision and collision != helper:
            raise RuntimeError('骨架或物理资产改名目标已被占用：' + target)
        if not EAL.rename_asset(object_path(helper), target):
            raise RuntimeError('骨架或物理资产改名失败：' + target)
        if not EAL.save_loaded_asset(helper, only_if_is_dirty=False):
            raise RuntimeError('骨架或物理资产改名保存失败：' + target)


def weapon_parent(entry):
    """Reuse the project's shared master; never create or modify a base material."""
    path = entry.get('config', {}).get('parentPath', '')
    parent = load(path)
    expected = MONSTER_WEAPON_TEMPLATE if entry.get('config', {}).get('materialRole') == 'monsterWeapon' else WEAPON_TEMPLATE
    if path.split('.')[0] != expected or not isinstance(parent, unreal.Material):
        raise RuntimeError('武器母材质缺失：' + expected)
    parameters = material_parameters(parent, entry)
    for channel in entry.get('textures', {}):
        if channel not in parameters:
            raise RuntimeError('武器母材质缺少贴图参数：' + channel)
    switches = {str(name) for name in MEL.get_static_switch_parameter_names(parent)}
    required_switches = {'Use VT', 'Use OpacityMasked'} | ({'Use Emissive'} if 'E' in entry.get('textures', {}) else set())
    if not required_switches.issubset(switches):
        raise RuntimeError('武器母材质缺少开关：' + ', '.join(sorted(required_switches - switches)))
    return parent


def monster_parent(entry):
    """Use the two verified shared monster masters; never modify either master."""
    config = entry.get('config', {})
    expected = MONSTER_SKIN_TEMPLATE if config.get('materialRole') == 'monsterSkin' else MONSTER_CLOTH_TEMPLATE
    path = config.get('parentPath', '')
    parent = load(path)
    if path.split('.')[0] != expected or not isinstance(parent, unreal.Material):
        raise RuntimeError('怪物母材质缺失或类型不正确：' + expected)
    parameters = material_parameters(parent, entry)
    for channel in entry.get('textures', {}):
        if channel not in parameters:
            raise RuntimeError('怪物母材质缺少贴图参数：' + channel)
    switches = {str(name) for name in MEL.get_static_switch_parameter_names(parent)}
    required = {'Use VT', 'Use OpacityMasked'} | ({'Use Emissive'} if 'E' in entry.get('textures', {}) else set())
    if not required.issubset(switches):
        raise RuntimeError('怪物母材质缺少开关：' + ', '.join(sorted(required - switches)))
    return parent


def configure_weapon_instance(obj, entry, previous):
    # Change only import-owned switches when new/reparented or their policy changed.
    config = entry['config']
    old = previous.get('config', {})
    reparented = old.get('parentPath') != config['parentPath']
    for field, parameter, value in [('virtualTexture', 'Use VT', False),
                                     ('useOpacityMasked', 'Use OpacityMasked', bool(entry.get('textures', {}).get('A')))]:
        if reparented or old.get(field) != value:
            # This engine's setter returns False even after a successful write; verify the stored value.
            MEL.set_material_instance_static_switch_parameter_value(obj, parameter, value)
            if MEL.get_material_instance_static_switch_parameter_value(obj, parameter) != value:
                raise RuntimeError('武器材质实例参数设置失败：' + parameter)


def configure_monster_instance(obj, entry, previous):
    """Set importer-owned switches only for a new/reparented or changed policy."""
    config = entry['config']
    old = previous.get('config', {})
    reparented = old.get('parentPath') != config['parentPath']
    for field, parameter, value in [('virtualTexture', 'Use VT', True),
                                     ('useOpacityMasked', 'Use OpacityMasked', bool(entry.get('textures', {}).get('A')))]:
        if reparented or old.get(field) != value:
            MEL.set_material_instance_static_switch_parameter_value(obj, parameter, value)
            if MEL.get_material_instance_static_switch_parameter_value(obj, parameter) != value:
                raise RuntimeError('怪物材质实例参数设置失败：' + parameter)


def configure_emissive_instance(obj, entry, previous):
    """Enable a newly managed emissive channel without overwriting later UE edits."""
    config = entry['config']
    reparented = previous.get('config', {}).get('parentPath') != config['parentPath']
    has_emissive = bool(entry.get('textures', {}).get('E'))
    had_emissive = bool(previous.get('textures', {}).get('E'))
    if not reparented and (not has_emissive or had_emissive):
        return
    parameter = 'UseEmissive' if item_material_entry(entry) else 'Use Emissive'
    MEL.set_material_instance_static_switch_parameter_value(obj, parameter, has_emissive)
    if MEL.get_material_instance_static_switch_parameter_value(obj, parameter) != has_emissive:
        raise RuntimeError('材质实例参数设置失败：' + parameter)


def current_material_values(obj, entry):
    if not isinstance(obj, unreal.MaterialInstanceConstant):
        return {}
    names = material_parameters(obj, entry)
    return {'parent': object_path(obj.get_editor_property('parent')),
            'textures': {channel: object_path(MEL.get_material_instance_texture_parameter_value(obj, param))
                         for channel, param in names.items() if channel in entry.get('textures', {})}}


def material_conflicts(current, saved, entry):
    applied = saved.get('appliedMaterial', {})
    if not applied:
        return []
    conflicts = []
    old_config = saved.get('config', {})
    if entry['config'].get('parentPath') != old_config.get('parentPath') and current['parent'] != applied.get('parent'):
        conflicts.append('parent')
    old_mapping = saved.get('textures', {})
    for channel, key in entry['textures'].items():
        if old_mapping.get(channel) != key and channel in applied.get('textures', {}) and current['textures'].get(channel) != applied['textures'][channel]:
            conflicts.append(channel)
    return conflicts


def apply_material_textures(obj, entry, previous, resolved, decision='', repair=()):
    parameters = material_parameters(obj, entry)
    parent_changed = previous.get('config', {}).get('parentPath') != entry['config']['parentPath']
    saved_mapping = previous.get('textures', {})
    written = {}
    for channel, texture_key in entry['textures'].items():
        if channel not in parameters:
            raise RuntimeError('UE 手调母材质缺少参数：' + channel)
        if parent_changed or saved_mapping.get(channel) != texture_key or decision == 'applyMapping' or not previous or channel in repair:
            texture = resolved[texture_key]
            MEL.set_material_instance_texture_parameter_value(obj, parameters[channel], texture)
            written[channel] = texture
    MEL.update_material_instance(obj)
    # Verify after the entire batch: a successful API call alone is not proof
    # that all earlier channels survived the final material update.
    for channel, texture in written.items():
        actual = MEL.get_material_instance_texture_parameter_value(obj, parameters[channel])
        if object_path(actual) != object_path(texture):
            raise RuntimeError('材质贴图绑定未生效：' + parameters[channel])


def imported_bone_name(name):
    # Match this engine's FFbxImporter::MakeName + FSkeletalMeshImportData::FixupBoneName.
    return re.sub(r'[.,/`%]', '_', name.rsplit(':', 1)[-1]).strip().replace(' ', '-').lower()


def resolve_hero_shared_materials(entry):
    config = entry.get('config', {})
    if config.get('heroPart'):
        gender, body = config.get('gender'), config.get('bodyCode')
        for slot, role in entry.get('heroSharedRoles', {}).items():
            if gender not in ('Male', 'Female') or not re.fullmatch(r'\d{2}', str(body)):
                raise RuntimeError('共享角色材质缺少性别/体型')
            base = ROOT + gender + '_' + body + '/Mesh/SK_' + gender + body + '/Materials/'
            paths = {'oral': base + 'MI_' + gender + body + '_oral', 'hair': base + 'MI_' + gender + body + '_hair01',
                     'eyebrow': base + 'MI_' + (gender + body if gender == 'Male' else gender) + '_eyebrow01',
                     'skin': base + 'MI_' + gender + body + '_body',
                     'eye': '/Game/MaterialLibrary/Character/MI_Eye/MI_Char_ProceduralIris',
                     'lacrimal': '/Game/MaterialLibrary/Character/MI_Eye/MI_Char_LacriMIalfluid',
                     'occlusion': base + 'MI_' + gender + body + '_EyeOcclusion' if gender == 'Male' else '/Game/MaterialLibrary/Character/MI_Eye/MI_Char_EyeOcclusion'}
            if role not in paths:
                raise RuntimeError('无法确定共享材质用途：' + slot)
            entry.setdefault('externalMaterials', {})[slot] = paths[role]


def validate_entry(entry, deleted_owners=()):
    config = entry.get('config', {})
    if entry['kind'] in ('staticMesh', 'skeletalMesh'):
        validate_lod_sources(entry)
        if config.get('lodPolicy') in ('existing-settings-v1', 'lod1-reduction-v2'):
            library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
            method = 'complete_character_lods' if config['lodPolicy'] == 'lod1-reduction-v2' else 'complete_configured_lods'
            if not library or not hasattr(library, method) or not hasattr(library, 'protect_imported_lod'):
                raise RuntimeError('请更新 AssetHive UE 插件并重启编辑器，以使用自适应 LOD')
            if config['lodPolicy'] == 'lod1-reduction-v2' and (not hasattr(library, 'get_character_lod_quality_version') or library.get_character_lod_quality_version() != 1):
                raise RuntimeError('请更新并重启 UE 插件，以启用 LOD 结构保护；尚未导入任何资源')
    resolve_hero_shared_materials(entry)
    if entry.get('validationError'):
        raise RuntimeError(entry['validationError'])
    path = entry.get('targetPath', '')
    root = ITEM_ROOT if entry.get('config', {}).get('family') == 'item' else NPC_ROOT if entry.get('config', {}).get('family') == 'npc' else MONSTER_ROOT if entry.get('config', {}).get('family') == 'monster' else WEAPON_ROOT if weapon_entry(entry) else ROOT
    if not path.startswith(root) or any(segment in ('', '.', '..') for segment in path[1:].split('/')):
        raise RuntimeError('Invalid Suit destination: ' + path)
    if entry['kind'] not in ('texture', 'material', 'staticMesh', 'skeletalMesh', 'groom'):
        raise RuntimeError('Unsupported Suit resource')
    if entry.get('unresolvedMaterials'):
        raise RuntimeError('材质槽待指定：' + ', '.join(entry['unresolvedMaterials']))
    for slot, path in entry.get('externalMaterials', {}).items():
        if slot in entry.get('sharedMaterialSlots', []) and not load(path):
            # Some existing public MIs omit the body number (e.g. MI_Female_eyebrow01).
            # Match only this suit's own gender/body directory, never another character family.
            directory, expected = path.rsplit('/', 1)
            suffix = re.sub(r'^MI_(?:Male|Female)\d*_', '', expected, flags=re.I).lower()
            matches = []
            for candidate in EAL.list_assets(directory, recursive=False, include_folder=False):
                name = candidate.rsplit('/', 1)[-1].split('.')[0]
                if re.sub(r'^MI_(?:Male|Female)\d*_', '', name, flags=re.I).lower() == suffix:
                    obj = load(candidate)
                    if isinstance(obj, unreal.MaterialInstanceConstant):
                        matches.append(object_path(obj))
            if len(matches) == 1:
                path = matches[0]
                entry['externalMaterials'][slot] = path
            elif len(matches) > 1:
                raise RuntimeError('公共材质存在多个匹配：' + slot + '；请整理 ' + directory)
        if not path.startswith(('/Game/', '/Engine/')) or not isinstance(load(path), unreal.MaterialInterface):
            raise RuntimeError('指定的共享材质不存在或类型不正确：' + path)
    if entry['kind'] != 'material' and not os.path.isfile(entry.get('sourcePath', '')):
        raise RuntimeError('Managed source missing: ' + entry.get('sourcePath', ''))
    if entry['kind'] == 'skeletalMesh':
        owned = owned_skeleton_entry(entry)
        skeleton = validate_weapon_helper(entry, entry.get('skeletonPath', ''), entry['key'] + ':skeleton', unreal.Skeleton, deleted_owners) if owned else load(entry.get('skeletonPath'))
        if not owned and not isinstance(skeleton, unreal.Skeleton):
            raise RuntimeError('请指定有效的已有 Skeleton；此 SK 暂不导入')
        if weapon_physics_enabled(entry):
            validate_weapon_helper(entry, entry['config'].get('physicsAssetPath', ''), entry['key'] + ':physics', unreal.PhysicsAsset, deleted_owners)
        target = {bone['name'].lower(): bone['parent'].lower() for bone in entry.get('targetBones', [])}
        incoming = [{'name': imported_bone_name(bone['name']), 'parent': imported_bone_name(bone['parent']),
                     'rootParents': [imported_bone_name(name) for name in bone.get('rootParents', [])]} for bone in entry.get('bones', [])]
        if len({bone['name'] for bone in incoming}) != len(incoming):
            raise RuntimeError('骨骼名称规范化后重名；请由绑定人员整理')
        def parent_matches(bone):
            if bone['name'] not in target:
                return False
            parent = target[bone['name']]
            if parent == bone['parent']:
                return True
            # FBX Null rig containers can become bones in UE. Only accept the
            # complete ancestor chain actually present in the source FBX.
            if bone['parent'] or not bone['rootParents']:
                return False
            for source_parent in bone['rootParents']:
                if parent != source_parent or parent not in target:
                    return False
                parent = target[parent]
            return not parent
        incompatible = [bone['name'] for bone in incoming if not parent_matches(bone)]
        if not incoming or ((not owned or skeleton) and (not target or incompatible)):
            raise RuntimeError('FBX 骨骼名称/父子层级与目标 Skeleton 不兼容：' + ', '.join(incompatible[:8]) + '；请由绑定人员确认，不修改共享骨架')
    if entry['kind'] == 'texture' and entry['channel'] == 'N' and entry['config'].get('normalFormat', '').lower() not in ('directx', 'opengl', 'dx', 'gl'):
        raise RuntimeError('请确认法线格式 DirectX / OpenGL')
    if entry['kind'] == 'material':
        if item_material_entry(entry):
            item_parent(entry)
            return
        if config.get('materialRole', '').startswith('hero_'):
            expected = {'hero_face': 'M_Char_Face', 'hero_hair': 'M_Char_Hair', 'hero_eyebrow': 'M_Char_EyeBrow', 'hero_eye': 'M_Char_Eye_CustomIris'}.get(config['materialRole'])
            if config.get('parentPath') != '/Game/MaterialLibrary/Character/Common/' + str(expected):
                raise RuntimeError('脸部/头发母材质映射不正确')
            parent = load(config['parentPath'])
            if not parent:
                raise RuntimeError('脸部/头发母材质缺失')
            missing = set(entry.get('textures', {})) - set(material_parameters(parent, entry))
            if missing:
                raise RuntimeError('母材质缺少贴图参数：' + ', '.join(sorted(missing)))
            return
        if weapon_material_entry(entry):
            weapon_parent(entry)
            return
        if monster_material_entry(entry):
            monster_parent(entry)
            return
        parent = load(entry['config'].get('parentPath'))
        if not parent or not entry['config']['parentPath'].startswith(TEMPLATE):
            raise RuntimeError('套装母材质模板缺失')
        parameters = material_parameters(parent)
        if not MEL.get_material_instance_static_switch_parameter_value(parent, 'Use VT'):
            raise RuntimeError('套装模板未启用 Use VT；请核对工程模板')
        if 'A' in entry['textures'] and not MEL.get_material_instance_static_switch_parameter_value(parent, 'Use OpacityMasked'):
            raise RuntimeError('透明模板未启用 Use OpacityMasked；请核对工程模板')
        if 'E' in entry['textures'] and 'Use Emissive' not in {str(name) for name in MEL.get_static_switch_parameter_names(parent)}:
            raise RuntimeError('母材质缺少 Use Emissive 开关')
        for channel in entry['textures']:
            if channel not in parameters:
                raise RuntimeError('母材质贴图参数待指定：' + channel)


def same_ue_package(left, right):
    # UE package/FName identity is case-insensitive; object suffixes are not
    # another package. A case-only rename would attempt to move an object onto itself.
    return str(left or '').split('.', 1)[0].rstrip('/').casefold() == str(right or '').split('.', 1)[0].rstrip('/').casefold()


def relocate_resource(obj, source, target):
    if same_ue_package(source, target):
        return source, obj
    collision = load(target)
    if collision and collision != obj:
        raise RuntimeError('接管目标已被其他 UE 对象占用')
    if not obj or not EAL.rename_asset(source, target):
        raise RuntimeError('UE 资源迁移失败；请检查目标冲突、写入权限和版本控制连接')
    moved = load(target)
    if not moved:
        raise RuntimeError('迁移后无法读取目标资源')
    return target, moved


def inspect(job):
    records = read_registry()
    deleted_owners = set(job.get('deletedOwnerIds', [])) - {suit['assetId'] for suit in job['suits']}
    resources = [(suit, entry) for suit in job['suits'] for entry in suit['entries']]
    legacy_keys = {entry['key']: [key for key, saved in records.items()
                                  if key != entry['key'] and legacy_export_matches(entry, key, saved)]
                   for _, entry in resources}
    unresolved = []
    located = {}
    for _, entry in resources:
        previous = records.get(entry['key'], {})
        obj = load(previous.get('path'))
        if obj and (str(EAL.get_metadata_tag(obj, KEY)) == entry['key'] or previous.get('linkOnly') and not str(EAL.get_metadata_tag(obj, KEY))):
            located[entry['key']] = [obj]
        elif previous:
            unresolved.append(entry['key'])
    # Query every identity, so copying an owned asset also surfaces an ambiguity.
    indexed = owned_objects(list({entry['key'] for _, entry in resources}
                                 | {key for keys in legacy_keys.values() for key in keys}))
    for key, matches in indexed.items():
        if matches:
            located[key] = matches
    rows = []
    for suit, entry in resources:
        row = {'assetId': suit['assetId'], 'key': entry['key'], 'name': entry['name'], 'kind': entry['kind'],
               'partSlot': entry.get('partSlot', ''), 'targetPath': entry['targetPath'], 'status': 'ready', 'action': 'added', 'message': ''}
        try:
            previous = records.get(entry['key'], {})
            if (previous.get('linkedSkeletonPath') and entry.get('config', {}).get('ownSkeleton')
                    and not entry.get('config', {}).get('skeletonOverride')
                    and same_ue_package(previous.get('targetPath'), entry.get('targetPath'))
                    and same_ue_package(previous.get('linkedCanonicalSkeletonPath', previous.get('skeletonPath')), entry.get('skeletonPath'))):
                entry['linkedSkeletonPath'] = previous['linkedSkeletonPath']
                entry['linkedCanonicalSkeletonPath'] = previous.get('linkedCanonicalSkeletonPath', previous.get('skeletonPath'))
                entry['skeletonPath'] = previous['linkedSkeletonPath']
                entry['config'] = {**entry['config'], 'ownSkeleton': False, 'createPhysicsAsset': False}
                catalog = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
                if not catalog or not hasattr(catalog, 'get_link_skeleton_bones'):
                    raise RuntimeError('请更新正式插件，以验证已关联模型的骨架')
                entry['targetBones'] = json.loads(catalog.get_link_skeleton_bones(load(entry['skeletonPath'])))
            validate_entry(entry, deleted_owners)
            matches = located.get(entry['key'], [])
            if len(matches) > 1:
                raise RuntimeError('多个 UE 对象具有相同来源身份，请先明确目标')
            target_obj = load(entry['targetPath'])
            legacy_matches = []
            if not matches and not target_obj:
                for old_key in legacy_keys.get(entry['key'], []):
                    candidates = indexed.get(old_key, [])
                    if not candidates:
                        candidate = load(records.get(old_key, {}).get('path'))
                        candidates = [candidate] if candidate else []
                    for candidate in candidates:
                        if candidate not in legacy_matches:
                            legacy_matches.append(candidate)
                if len(legacy_matches) > 1:
                    raise RuntimeError('多个旧 AssetHive 归属对象符合当前资源，不能自动选择')
            obj = matches[0] if matches else target_obj or (legacy_matches[0] if legacy_matches else None)
            previous = records.get(entry['key'], {})
            if obj:
                key = str(EAL.get_metadata_tag(obj, KEY))
                if not key and previous.get('linkOnly') and same_ue_package(previous.get('path'), object_path(obj)):
                    key = entry['key']
                row['path'] = object_path(obj)
                if key != entry['key']:
                    old = receipt(obj) or records.get(key, {})
                    if key and not legacy_export_matches(entry, key, old):
                        raise RuntimeError('此目标属于另一个库资产，不能覆盖')
                    if key and key.split(':', 1)[0] in deleted_owners:
                        saved = reassociated_receipt(old, key, entry['key'])
                        same = all(saved.get(field) == entry.get(field) for field in
                                   ('sourceHash', 'config', 'skeletonPath', 'textures', 'materials', 'externalMaterials'))
                        row.update(action='relinked' if same else 'updated', oldKey=key,
                                   message='旧库资产已删除，已自动匹配；确认发送后重新关联')
                        if entry['kind'] == 'material':
                            current = current_material_values(obj, entry)
                            row['currentMaterial'] = current
                            if material_conflicts(current, saved, entry):
                                row.update(status='conflict', message='UE 材质已手调：选择保留 UE 或采用本次映射')
                    else:
                        row.update(status='conflict', action='adopt', oldKey=key,
                                   message=('检测到同一规范资源的旧 AssetHive 归属；确认接管后更新，UE 对象不会被删除'
                                            if key else '同名 UE 资产尚未关联；确认关联后更新，UE 对象不会被删除'))
                else:
                    saved = receipt(obj, previous)
                    row['action'] = 'unchanged' if saved.get('fingerprint') == entry['fingerprint'] or same_entry_content(saved, entry) else 'updated'
                    if entry['kind'] in ('staticMesh', 'skeletalMesh'):
                        reset_pending = lod_reset_pending(obj, entry)
                        lod_report = None if reset_pending else lod_policy_report(obj, entry)
                        if reset_pending:
                            row.update(action='updated', message='LOD0 已替换：清理旧 LOD1 及后续等级，仅使用本版本新提供的 LOD')
                        if lod_report:
                            row['lodReport'] = lod_report
                            if (lod_report.get('pending') or lod_report.get('missingManual')) and row['action'] == 'unchanged':
                                row.update(action='updated', message='将重新验证美术 LOD，并补齐自动减面等级')
                            elif lod_report.get('missing'):
                                row['message'] = ('请补充美术 LOD1 后自动生成 LOD2–5；本次仍可发送已有模型' if entry.get('config', {}).get('lodPolicy') == 'lod1-reduction-v2'
                                                  else '未配置减面设置：' + '、'.join('LOD' + str(index) for index in lod_report['missing']) + '；本次仍可发送美术模型')

                    # A manually moved object stays where the user put it unless the canonical name changed.
                    if previous.get('targetPath') and not same_ue_package(previous['targetPath'], entry['targetPath']):
                        collision = load(entry['targetPath'])
                        if collision and collision != obj:
                            raise RuntimeError('改名目标已被其他 UE 对象占用')
                        row['action'] = 'renamed' if row['action'] == 'unchanged' else 'updatedRenamed'
                    if entry['kind'] == 'material' and row['action'] not in ('unchanged', 'renamed'):
                        current = current_material_values(obj, entry)
                        row['currentMaterial'] = current
                        if material_conflicts(current, saved, entry):
                            row.update(status='conflict', message='UE 材质已手调：选择保留 UE 或采用本次映射')
                if row['status'] == 'ready' and deferred_item_material_conflicts(obj, entry):
                    row.update(status='conflict', message='此玩法物件已有手动材质：选择保留 UE 材质，或采用新母材质映射')
            elif previous:
                row.update(status='missing', action='restore', message='UE 中已缺失：选择补回或跳过本次')
        except Exception as error:
            row.update(status='blocked', message=str(error))
        rows.append(row)
    missing = {row['key'] for row in rows if row['status'] == 'missing'}
    row_by_key = {row['key']: row for row in rows}
    for _, entry in resources:
        row = row_by_key[entry['key']]
        if row['action'] != 'unchanged' or row['status'] != 'ready':
            continue
        obj = load(row.get('path'))
        repair = []
        if entry['kind'] == 'material':
            parameters = material_parameters(obj, entry)
            overrides = {str(value.parameter_info.name): value.parameter_value for value in obj.get_editor_property('texture_parameter_values')}
            # A deleted override resolves to the parent's fallback texture via MEL.
            # Inspect the actual override so restoration can reconnect it without touching other values.
            repair = [channel for channel, key in entry['textures'].items()
                      if key in missing and parameters.get(channel) in overrides and not overrides[parameters[channel]]]
            saved = receipt(obj, records.get(entry['key']))
            if not saved.get('materialBindingVersion'):
                current = current_material_values(obj, entry)
                applied = saved.get('appliedMaterial', {}).get('textures', {})
                repair += [channel for channel in entry['textures'] if parameters.get(channel) not in overrides
                           and current['textures'].get(channel) == applied.get(channel)]
        elif entry['kind'] in ('staticMesh', 'skeletalMesh'):
            slots = obj.get_editor_property('static_materials' if entry['kind'] == 'staticMesh' else 'materials')
            repair = [slot_name(slot) for slot in slots
                          if not slot.material_interface and entry.get('materials', {}).get(slot_name(slot)) in missing]
        if weapon_physics_enabled(entry) and not obj.get_editor_property('physics_asset'):
            row.update(action='updated', message='补充武器物理资产')
        elif repair:
            row.update(action='repair', repair=repair, message='补齐已管理的贴图或材质引用')
    current_keys = {entry['key'] for _, entry in resources}
    for suit in job['suits']:
        if suit.get('selectedSlot'):
            continue
        for key, previous in records.items():
            if previous.get('assetId') == suit['assetId'] and key not in current_keys:
                rows.append({'assetId': suit['assetId'], 'key': key, 'name': previous.get('name', key),
                             'action': 'retained', 'status': 'unchanged', 'path': previous.get('path', ''),
                             'message': '库中已移除，UE 保留'})
    return {'entries': rows, 'inspectToken': digest(rows)}


def validate_lod_sources(entry):
    skin_source = entry.get('config', {}).get('skinLodSource')
    if skin_source:
        if entry['kind'] != 'skeletalMesh' or skin_source.get('version') != 1 or skin_source.get('sourceLod') not in (0, 1):
            raise RuntimeError('SM → SK LOD 来源配置无效')
        library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
        if not library or not hasattr(library, 'complete_skinned_lods_from_static'):
            raise RuntimeError('请更新并重启 UE 插件，以从 SM LOD1 生成蒙皮 LOD')
        if not skin_source.get('sourcePath', '').lower().endswith('.fbx'):
            raise RuntimeError('SM LOD1 来源必须为 FBX')
        with open(skin_source['sourcePath'], 'rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != skin_source.get('sourceHash'):
                raise RuntimeError('SM LOD1 文件已变化，请重新检查')
    levels = entry.get('config', {}).get('lodSources', [])
    for index, source in enumerate(levels, 1):
        if source.get('index') != index or not source.get('sourcePath', '').lower().endswith('.fbx'):
            raise RuntimeError('LOD 等级必须连续且来源必须为 FBX')
        with open(source['sourcePath'], 'rb') as stream:
            actual = hashlib.file_digest(stream, 'sha256').hexdigest()
        if actual != source.get('sourceHash'):
            raise RuntimeError('LOD 文件已变化，请重新检查：' + source['sourcePath'])


def lod_policy_report(obj, entry, regenerate=False, source_changed=True):
    policy = entry.get('config', {}).get('lodPolicy')
    if policy not in ('existing-settings-v1', 'lod1-reduction-v2'):
        return None
    library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
    skin_source = entry.get('config', {}).get('skinLodSource')
    if skin_source:
        if not library or not hasattr(library, 'complete_skinned_lods_from_static'):
            raise RuntimeError('请更新并重启 UE 插件，以从 SM LOD1 生成蒙皮 LOD')
        source_materials = skin_source.get('sourceMaterials', {})
        if not source_materials and entry.get('key'):
            records = read_registry()
            static_record = records.get(entry['key'].replace(':skeletalMesh:', ':staticMesh:'), {})
            source_materials = dict(static_record.get('externalMaterials', {}))
            for slot, key in static_record.get('materials', {}).items():
                if records.get(key, {}).get('path'):
                    source_materials[slot] = records[key]['path']
        args = (obj, skin_source['sourcePath'], skin_source['sourceHash'], skin_source['sourceLod'], regenerate)
        report = json.loads(library.complete_skinned_lods_from_static(*args,
                            json.dumps(source_materials, sort_keys=True)) if source_materials
                            else library.complete_skinned_lods_from_static(*args))
        if report.get('error'):
            raise RuntimeError('蒙皮 LOD 生成失败：' + report['error'])
        return report
    method = 'complete_character_lods' if policy == 'lod1-reduction-v2' else 'complete_configured_lods'
    if not library or not hasattr(library, method):
        raise RuntimeError('请更新 AssetHive UE 插件并重启编辑器，以使用自适应 LOD')
    manual = [0] + [source['index'] for source in entry.get('config', {}).get('lodSources', [])]
    args = (obj, manual, regenerate, source_changed) if policy == 'lod1-reduction-v2' and hasattr(library, 'get_character_lod_quality_version') else (obj, manual, regenerate)
    return json.loads(getattr(library, method)(*args))


def verify_lod_report(entry, report):
    if entry.get('config', {}).get('lodPolicy') != 'lod1-reduction-v2':
        return
    if not report or report.get('policy') != 'lod1-reduction-v2':
        raise RuntimeError('UE 未返回有效 LOD 验证结果，请更新插件并重试')
    if report.get('qualityVersion') != 1:
        raise RuntimeError('请更新并重启 UE 插件，以启用 LOD 结构保护')
    levels = {level['index']: level for level in report.get('levels', [])}
    manual = [0] + [s['index'] for s in entry.get('config', {}).get('lodSources', [])]
    if report.get('missingManual') or any(levels.get(i, {}).get('triangles', 0) <= 0 or levels[i].get('source') != 'imported' for i in manual):
        raise RuntimeError('UE 美术 LOD 未正确挂入或网格为空，请重新发送')
    if entry.get('config', {}).get('skinLodSource') or 1 in levels and levels[1].get('source') in ('imported', 'existing'):
        if report.get('missing') or report.get('pending') or any(levels.get(i, {}).get('triangles', 0) <= 0 or (levels[i].get('source') == 'generated' and not levels[i].get('structureVerified')) for i in range(2, 6)):
            raise RuntimeError('UE LOD2–5 自动减面未完成，请检查减面模块并重试')


def lod_reset_pending(obj, entry):
    token = entry.get('config', {}).get('lodReset')
    return bool(token and EAL.get_metadata_tag(obj, 'AssetHive.LodReset') != token)


def reset_model_lods(obj, entry):
    """Explicit base revision invalidation; only called during confirmed send."""
    kind = unreal.SkeletalMeshEditorSubsystem if entry['kind'] == 'skeletalMesh' else unreal.StaticMeshEditorSubsystem
    subsystem = unreal.get_editor_subsystem(kind) or unreal.get_default_object(kind)
    count = subsystem.get_lod_count(obj)
    if count > 1:
        ok = subsystem.remove_lods(obj, list(range(1, count))) if entry['kind'] == 'skeletalMesh' else subsystem.remove_lods(obj)
        if not ok or subsystem.get_lod_count(obj) != 1:
            raise RuntimeError('旧 LOD 清理失败，未记录本次更新成功')
    for index in range(1, max(count, 6)):
        for prefix in ('AssetHive.ComponentLOD.', 'AssetHive.LodSource.', 'AssetHive.SkinLOD.'):
            EAL.remove_metadata_tag(obj, prefix + str(index))


def apply_model_lods(obj, entry, previous=None):
    levels = entry.get('config', {}).get('lodSources', [])
    adaptive = entry.get('config', {}).get('lodPolicy') in ('existing-settings-v1', 'lod1-reduction-v2')
    if not levels and not adaptive:
        return
    validate_lod_sources(entry)
    subsystem_type = unreal.SkeletalMeshEditorSubsystem if entry['kind'] == 'skeletalMesh' else unreal.StaticMeshEditorSubsystem
    subsystem = unreal.get_editor_subsystem(subsystem_type) or unreal.get_default_object(subsystem_type)
    library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
    old_levels = {source['index']: source.get('sourceHash') for source in (previous or {}).get('config', {}).get('lodSources', [])}
    before = lod_policy_report(obj, entry) if adaptive else None
    missing_manual = set((before or {}).get('missingManual', []))
    changed = []
    for source in levels:
        if old_levels.get(source['index']) == source.get('sourceHash') and source['index'] not in missing_manual and subsystem.get_lod_count(obj) > source['index']:
            continue
        if adaptive:
            library.protect_imported_lod(obj, source['index'])
        imported = subsystem.import_lod(obj, source['index'], source['sourcePath'])
        if imported != source['index']:
            raise RuntimeError('UE 导入 LOD 失败：LOD' + str(source['index']) + ' ' + source['sourcePath'])
        changed.append(source['index'])
        if adaptive:
            library.protect_imported_lod(obj, source['index'])
    if levels and subsystem.get_lod_count(obj) < levels[-1]['index'] + 1:
        raise RuntimeError('UE LOD 数量回读不一致')
    if previous and not changed and before and not any(before.get(field) for field in ('missing', 'pending', 'missingManual')):
        verify_lod_report(entry, before)
        return before
    report = lod_policy_report(obj, entry, True, source_changed=not previous or 1 in changed)
    verify_lod_report(entry, report)
    return report


def import_file(entry, destination):
    directory, name = destination.rsplit('/', 1)
    task = unreal.AssetImportTask()
    task.filename = entry['sourcePath']
    task.destination_path = directory
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.replace_existing_settings = False
    task.save = False
    previous_model = load(destination)
    previous_skeleton = previous_model.get_editor_property('skeleton') if entry['kind'] == 'skeletalMesh' and previous_model else None
    previous_physics = weapon_physics_for_import(entry, load(destination)) if weapon_physics_enabled(entry) else None
    if entry['kind'] == 'texture':
        task.factory = unreal.TextureFactory()
    if entry['kind'] == 'groom':
        if not entry['sourcePath'].lower().endswith('.abc') or not hasattr(unreal, 'GroomImportOptions'):
            raise RuntimeError('Groom 需要 ABC 以及启用的 Groom/Alembic Groom Importer 插件')
        task.factory = unreal.HairStrandsFactory()
        task.options = unreal.GroomImportOptions()
        if previous_model:
            library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
            if not library or not hasattr(library, 'copy_groom_import_options'):
                raise RuntimeError('请更新正式插件，以保留 Groom 导入设置')
            previous_options = library.copy_groom_import_options(previous_model)
            if not previous_options:
                raise RuntimeError('Groom 缺少原始导入设置，未覆盖现有资源')
            task.options = previous_options
    if entry['kind'] in ('staticMesh', 'skeletalMesh'):
        options = unreal.FbxImportUI()
        skeletal = entry['kind'] == 'skeletalMesh'
        options.set_editor_property('automated_import_should_detect_type', False)
        options.import_as_skeletal = skeletal
        options.mesh_type_to_import = unreal.FBXImportType.FBXIT_SKELETAL_MESH if skeletal else unreal.FBXImportType.FBXIT_STATIC_MESH
        options.import_mesh = True
        options.import_materials = False
        options.import_textures = False
        options.import_animations = False
        options.create_physics_asset = bool(weapon_physics_enabled(entry) and not previous_physics)
        if previous_physics:
            options.physics_asset = previous_physics
        if skeletal:
            options.skeleton = load(entry['skeletonPath'])
            options.skeletal_mesh_import_data.set_editor_property('update_skeleton_reference_pose', False)
            options.skeletal_mesh_import_data.set_editor_property('use_t0_as_ref_pose', False)
            options.skeletal_mesh_import_data.set_editor_property('import_morph_targets', True)
        else:
            options.static_mesh_import_data.combine_meshes = True
            options.static_mesh_import_data.auto_generate_collision = False
        # Normalize each FBX source axis system, independent of editor import presets.
        mesh_import_data = options.skeletal_mesh_import_data if skeletal else options.static_mesh_import_data
        mesh_import_data.set_editor_property('import_mesh_lods', True)
        mesh_import_data.set_editor_property('convert_scene', True)
        mesh_import_data.set_editor_property('force_front_x_axis', False)
        task.options = options
        task.factory = unreal.FbxFactory()
    reimported = False
    if previous_model and entry['kind'] in ('staticMesh', 'skeletalMesh'):
        library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
        if not library or not hasattr(library, 'reimport_linked_model'):
            raise RuntimeError('请更新 AssetHive 插件，以自动更新已关联模型')
        if not library.reimport_linked_model(previous_model, entry['sourcePath'], task.options):
            raise RuntimeError('模型自动重新导入失败，未要求手动选择源文件')
        reimported = True
    else:
        unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    obj = load(destination)
    expected = getattr(unreal, 'GroomAsset', None) if entry['kind'] == 'groom' else {'texture': unreal.Texture2D, 'staticMesh': unreal.StaticMesh, 'skeletalMesh': unreal.SkeletalMesh}[entry['kind']]
    if not isinstance(obj, expected) or (not reimported and not task.imported_object_paths):
        raise RuntimeError('UE 未返回预期类型的导入资产：' + entry['name'])
    if entry['kind'] == 'skeletalMesh':
        skeleton = obj.get_editor_property('skeleton')
        if owned_skeleton_entry(entry) and not load(entry['skeletonPath']):
            if skeleton == previous_skeleton:
                # Reimport can retain the old assignment even with an empty import Skeleton.
                # Create from the updated mesh; never rename an existing/shared skeleton.
                skeleton = unreal.AssetHiveModelCatalogLibrary.create_skeleton_for_mesh(obj, entry['skeletonPath'])
                if not skeleton:
                    raise RuntimeError('创建新骨架失败，原骨架已保留')
                if not unreal.AssetHiveModelCatalogLibrary.assign_existing_skeleton(obj, skeleton):
                    raise RuntimeError('新骨架与模型不兼容，未替换原骨架')
            elif not skeleton or not EAL.rename_asset(object_path(skeleton), entry['skeletonPath']):
                raise RuntimeError('骨架规范命名失败')
            save(skeleton, {'key': entry['key'] + ':skeleton', 'kind': 'weaponSkeleton'})
        if skeleton != load(entry['skeletonPath']):
            # Compatibility was checked before apply; preserve the shared skeleton's tree.
            skeleton = load(entry['skeletonPath'])
            if not isinstance(skeleton, unreal.Skeleton):
                raise RuntimeError('指定的已有骨架不可用')
            if not unreal.AssetHiveModelCatalogLibrary.assign_existing_skeleton(obj, skeleton):
                raise RuntimeError('已有骨架与模型不兼容，未修改共享骨架')
        if skeleton != load(entry['skeletonPath']):
            raise RuntimeError('导入后的 Skeleton 不符合目标')
    if weapon_physics_enabled(entry):
        save_weapon_physics(entry, obj, previous_physics)
    return obj


def apply_texture(obj, entry):
    channel = entry['channel']
    properties = {
        'compression_settings': unreal.TextureCompressionSettings.TC_NORMALMAP if channel == 'N'
            else unreal.TextureCompressionSettings.TC_DEFAULT if channel in ('D', 'E') else unreal.TextureCompressionSettings.TC_MASKS,
        'srgb': channel in ('D', 'E'),
        'lod_group': unreal.TextureGroup.TEXTUREGROUP_CHARACTER_NORMAL_MAP if channel == 'N'
            else unreal.TextureGroup.TEXTUREGROUP_CHARACTER_SPECULAR if channel == 'ORM' else unreal.TextureGroup.TEXTUREGROUP_CHARACTER,
        'virtual_texture_streaming': entry.get('config', {}).get('virtualTexture', True), 'max_texture_size': 0, 'lod_bias': 0,
        'mip_gen_settings': unreal.TextureMipGenSettings.TMGS_FROM_TEXTURE_GROUP,
        'compression_no_alpha': False,
        'lossy_compression_amount': unreal.TextureLossyCompressionAmount.TLCA_DEFAULT,
        'address_x': unreal.TextureAddress.TA_WRAP, 'address_y': unreal.TextureAddress.TA_WRAP,
    }
    if channel == 'N':
        properties['flip_green_channel'] = entry['config']['normalFormat'].lower() in ('gl', 'opengl')
    if entry.get('config', {}).get('family') == 'item' and entry['config'].get('itemMaterialVersion'):
        properties.update(virtual_texture_streaming=True, max_texture_size=2048,
                          lod_group=unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP if channel == 'N' else unreal.TextureGroup.TEXTUREGROUP_WORLD,
                          lossy_compression_amount=unreal.TextureLossyCompressionAmount.TLCA_LOW if channel in ('D', 'N') else unreal.TextureLossyCompressionAmount.TLCA_MEDIUM)
    changed = {name: value for name, value in properties.items() if obj.get_editor_property(name) != value}
    # UObject.set_editor_properties issues one Pre/PostEditChange for the batch.
    # Individual setters rebuild texture resources repeatedly, even for unchanged values.
    if changed:
        obj.set_editor_properties(changed)


def configure_hair_instance(obj, entry, previous):
    """Initialize texture-only cards once; never reset artist overrides on resend."""
    if entry.get('config', {}).get('materialRole') != 'hero_hair':
        return
    if previous.get('config', {}).get('hairDefaultsVersion') == entry['config'].get('hairDefaultsVersion'):
        had_diffuse = 'D' in previous.get('textures', {})
        has_diffuse = 'D' in entry.get('textures', {})
        if had_diffuse != has_diffuse:
            old_blend = 1.0 if had_diffuse else 0.0
            # Switch only our automatic defaults when adding/removing a colour map.
            if abs(MEL.get_material_instance_scalar_parameter_value(obj, 'Diffuse Tex Blend Intensity') - old_blend) < 0.00001:
                MEL.set_material_instance_scalar_parameter_value(obj, 'Diffuse Tex Blend Intensity', 1.0 if has_diffuse else 0.0)
                for name, fallback in [('Root Color', 0.02), ('Tip Color', 0.04)]:
                    colour = MEL.get_material_instance_vector_parameter_value(obj, name)
                    expected = 1.0 if had_diffuse else fallback
                    if all(abs(value - expected) < 0.00001 for value in (colour.r, colour.g, colour.b)):
                        level = 1.0 if has_diffuse else fallback
                        MEL.set_material_instance_vector_parameter_value(obj, name, unreal.LinearColor(level, level, level, colour.a))
        return
    scalars = {str(v.parameter_info.name) for v in obj.get_editor_property('scalar_parameter_values')}
    vectors = {str(v.parameter_info.name) for v in obj.get_editor_property('vector_parameter_values')}
    # These five data maps carry no colour. Do not sample the mother's placeholder diffuse.
    if 'D' not in entry.get('textures', {}):
        if 'Diffuse Tex Blend Intensity' not in scalars:
            MEL.set_material_instance_scalar_parameter_value(obj, 'Diffuse Tex Blend Intensity', 0.0)
        for name, level in [('Root Color', 0.02), ('Tip Color', 0.04)]:
            if name not in vectors:
                MEL.set_material_instance_vector_parameter_value(obj, name, unreal.LinearColor(level, level, level, 1.0))
    # Match the common hair instance: its view-angle edge mask is disabled by default.
    if 'ISEdgeMask' not in scalars:
        MEL.set_material_instance_scalar_parameter_value(obj, 'ISEdgeMask', 0.0)


def source_control_issue():
    control = getattr(unreal, 'SourceControl', None)
    if control and control.is_enabled() and not control.is_available():
        return 'UE 版本控制未连接或登录已过期，请重新登录后发送；未开始修改资源'
    return ''


def apply_job(job):
    started = time.perf_counter()
    checked = inspect(job)
    timings = {'inspectMs': round((time.perf_counter() - started) * 1000), 'hashMs': 0, 'importMs': 0, 'textureSettingsMs': 0, 'saveMs': 0, 'registryMs': 0}
    if checked['inspectToken'] != job.get('inspectToken'):
        raise RuntimeError('UE 状态在检查后变化，请重新检查')
    if any(row['action'] not in ('unchanged', 'retained') and job.get('decisions', {}).get(row['key']) != 'skip'
           and row['status'] != 'blocked' for row in checked['entries']):
        issue = source_control_issue()
        if issue:
            raise RuntimeError(issue)
    # Reuse the fixed branches; create missing directories only.
    for suit in job['suits']:
        root = suit.get('targetRoot', '')
        if re.fullmatch(r'/Game/Characters/Hero/Asset_PJZ/(Male|Female)_\d{2}/Mesh/Hair', root):
            for branch in ('HairCard', 'Groom'):
                directory = root + '/' + branch
                if not EAL.does_directory_exist(directory):
                    if not EAL.make_directory(directory):
                        raise RuntimeError('Cannot create hair directory: ' + directory)
    records = read_registry()
    decisions = job.get('decisions', {})
    by_key = {entry['key']: entry for suit in job['suits'] for entry in suit['entries']}
    suit_by_key = {entry['key']: suit for suit in job['suits'] for entry in suit['entries']}
    resolved = {}
    pending_count = sum(row['action'] not in ('unchanged', 'retained') for row in checked['entries'])
    families = {asset.get('family', 'suit') for asset in job['suits']}
    labels = {'suit': '套装', 'weapon': '武器', 'monster': '怪物', 'npc': 'NPC', 'hair': '头发', 'face': '脸部', 'item': '玩法物件'}
    category = labels.get(next(iter(families)), '角色资产') if len(families) == 1 else '角色资产'
    progress_label = 'AssetHive：正在导入' + category
    # One parent slow-task window owns the entire batch. UE child import/save
    # tasks reuse it instead of repeatedly creating and destroying dialogs.
    with unreal.ScopedSlowTask(max(1, pending_count), progress_label, pending_count > 0) as progress:
        if pending_count:
            progress.make_dialog(False)
        current_item = 0
        for index, row in enumerate(checked['entries']):
            unreal.log('AssetHiveSuitProgress:%d|%d/%d · %s' % (int(index * 100 / max(1, len(checked['entries']))), index + 1, len(checked['entries']), row['name']))
            if row['action'] not in ('unchanged', 'retained'):
                current_item += 1
                progress.enter_progress_frame(1, progress_label + ' %d/%d' % (current_item, pending_count))
            key = row['key']
            if row['action'] == 'retained':
                continue
            entry = by_key[key]
            decision = decisions.get(key, '')
            if decision == 'skip' or row['status'] == 'blocked' or (row['status'] == 'missing' and decision != 'restore') or (row['status'] == 'conflict' and decision not in ('adopt', 'keepUE', 'applyMapping')):
                row['status'] = 'skipped' if row['status'] != 'blocked' else 'blocked'
                continue
            try:
                dependencies = entry.get('textures', {}) if entry['kind'] == 'material' else entry.get('materials', {})
                if any(dep not in resolved for dep in dependencies.values()):
                    raise RuntimeError('必要依赖未完成，当前资源待处理')
                destination = row.get('path', entry['targetPath']).split('.')[0]
                obj = load(destination)
                if decision == 'adopt':
                    destination, obj = relocate_resource(obj, destination, entry['targetPath'])
                if row['action'] in ('renamed', 'updatedRenamed'):
                    destination, obj = relocate_resource(obj, destination, entry['targetPath'])
                if row['action'] in ('unchanged', 'renamed', 'relinked'):
                    if not obj:
                        raise RuntimeError('UE 资产在执行时缺失')
                    if row['action'] in ('renamed', 'relinked') and entry['kind'] != 'material' and entry.get('sourcePath'):
                        import_data = obj.get_editor_property('asset_import_data')
                        if import_data:
                            import_data.scripted_add_filename(entry['sourcePath'], 0, '')
                elif entry['kind'] == 'material':
                    parent = item_parent(entry) if item_material_entry(entry) else weapon_parent(entry) if weapon_material_entry(entry) else monster_parent(entry) if monster_material_entry(entry) else load(entry['config']['parentPath'])
                    if not obj:
                        factory = unreal.MaterialInstanceConstantFactoryNew()
                        directory, name = destination.rsplit('/', 1)
                        obj = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, directory, unreal.MaterialInstanceConstant, factory)
                        MEL.set_material_instance_parent(obj, parent)
                    if not isinstance(obj, unreal.MaterialInstanceConstant):
                        raise RuntimeError('同名对象不是材质实例')
                    if decision != 'keepUE':
                        previous_receipt = receipt(obj, records.get(key))
                        if row.get('oldKey'):
                            previous_receipt = reassociated_receipt(previous_receipt, row['oldKey'], key)
                        if previous_receipt.get('config', {}).get('parentPath') != entry['config']['parentPath']:
                            MEL.set_material_instance_parent(obj, parent)
                        if weapon_material_entry(entry):
                            configure_weapon_instance(obj, entry, previous_receipt)
                        elif monster_material_entry(entry):
                            configure_monster_instance(obj, entry, previous_receipt)
                        elif entry['config'].get('materialRole', '').startswith('hero_'):
                            if previous_receipt.get('config', {}).get('parentPath') != entry['config']['parentPath']:
                                switches = {str(n) for n in MEL.get_static_switch_parameter_names(obj)}
                                for switch, enabled in [('Use VT', entry['config'].get('virtualTexture', False)), ('UseFlowMapTexture', 'Tangent' in entry['textures']), ('Use OpacityMasked', 'Opacity' in entry['textures'])]:
                                    if switch in switches:
                                        MEL.set_material_instance_static_switch_parameter_value(obj, switch, enabled)
                        configure_hair_instance(obj, entry, previous_receipt)
                        configure_emissive_instance(obj, entry, previous_receipt)
                        apply_material_textures(obj, entry, previous_receipt, resolved, decision, row.get('repair', []))
                else:
                    prior_config = receipt(obj, records.get(key)).get('config', {}) if obj else {}
                    same_skeleton_choice = all(prior_config.get(field) == entry.get('config', {}).get(field)
                                               for field in ('skeletonOverride', 'skeletonOverridePath'))
                    if row['action'] == 'updatedRenamed' and same_skeleton_choice and owned_skeleton_entry(entry) and entry['kind'] == 'skeletalMesh' and obj and not load(entry['skeletonPath']):
                        previous_skeleton = obj.get_editor_property('skeleton')
                        if previous_skeleton and str(EAL.get_metadata_tag(previous_skeleton, KEY)) == entry['key'] + ':skeleton':
                            if not EAL.rename_asset(object_path(previous_skeleton), entry['skeletonPath']):
                                raise RuntimeError('武器骨架改名失败')
                            save(previous_skeleton, {'key': entry['key'] + ':skeleton', 'kind': 'weaponSkeleton'})
                    # Hash again immediately before import; source files are never edited.
                    stage = time.perf_counter()
                    with open(entry['sourcePath'], 'rb') as source:
                        actual = hashlib.file_digest(source, 'sha256').hexdigest()
                    timings['hashMs'] += round((time.perf_counter() - stage) * 1000)
                    if actual != entry['sourceHash']:
                        raise RuntimeError('管理文件内容变化，请重新检查')
                    old_materials = {}
                    if obj and receipt(obj, records.get(key)) and entry['kind'] in ('staticMesh', 'skeletalMesh'):
                        slots = obj.get_editor_property('static_materials' if entry['kind'] == 'staticMesh' else 'materials')
                        old_materials = {str(slot.material_slot_name): slot.material_interface for slot in slots}
                    old_config = receipt(obj, records.get(key)).get('config') if obj else None
                    adopt_item_materials = bool(old_config and old_config.get('deferMaterials') and entry.get('config', {}).get('itemMaterialVersion') and decision != 'keepUE')
                    if row['action'] != 'repair':
                        stage = time.perf_counter()
                        # A LOD-only update must not reimport LOD0 or recreate helpers.
                        prior = receipt(obj, records.get(key)) if obj else {}
                        lod_only = bool(obj and entry['kind'] in ('staticMesh', 'skeletalMesh')
                            and prior.get('sourceHash') == entry.get('sourceHash')
                            and {k: v for k, v in prior.get('config', {}).items() if k not in ('lodSources', 'lodImportVersion', 'lodPolicy', 'skinLodSource', 'lodReset', 'ignoreEmbeddedLods')}
                            == {k: v for k, v in entry.get('config', {}).items() if k not in ('lodSources', 'lodImportVersion', 'lodPolicy', 'skinLodSource', 'lodReset', 'ignoreEmbeddedLods')})
                        reset = bool(obj and entry['kind'] in ('staticMesh', 'skeletalMesh') and lod_reset_pending(obj, entry))
                        if reset:
                            reset_model_lods(obj, entry)
                        if not lod_only:
                            obj = import_file(entry, destination)
                        if entry['kind'] in ('staticMesh', 'skeletalMesh'):
                            if not lod_only and entry.get('config', {}).get('ignoreEmbeddedLods'):
                                reset_model_lods(obj, entry)
                            apply_model_lods(obj, entry, prior if lod_only and not reset else None)
                            if entry.get('config', {}).get('lodReset'):
                                EAL.set_metadata_tag(obj, 'AssetHive.LodReset', entry['config']['lodReset'])
                        timings['importMs'] += round((time.perf_counter() - stage) * 1000)
                    elif entry['kind'] in ('staticMesh', 'skeletalMesh') and entry.get('config', {}).get('lodPolicy') == 'lod1-reduction-v2':
                        # Material repair and a missing artist LOD can coexist.
                        apply_model_lods(obj, entry, receipt(obj, records.get(key)))
                    if entry['kind'] == 'texture':
                        if old_config != entry['config']:
                            stage = time.perf_counter()
                            apply_texture(obj, entry)
                            timings['textureSettingsMs'] += round((time.perf_counter() - stage) * 1000)
                    elif entry['kind'] in ('staticMesh', 'skeletalMesh'):
                        prop = 'static_materials' if entry['kind'] == 'staticMesh' else 'materials'
                        slots = obj.get_editor_property(prop)
                        for index, slot in enumerate(slots):
                            name = mapped_slot_name(slot_name(slot), entry)
                            if str(slot.material_slot_name) in old_materials and name not in row.get('repair', []) and not adopt_item_materials and decision != 'applyMapping':
                                slot.material_interface = old_materials[str(slot.material_slot_name)]
                            elif name in entry['materials']:
                                slot.material_interface = resolved[entry['materials'][name]]
                            elif name in entry.get('externalMaterials', {}):
                                slot.material_interface = load(entry['externalMaterials'][name])
                            elif entry.get('config', {}).get('family') == 'item' and entry['config'].get('deferMaterials'):
                                pass  # No Item master has been selected; keep existing/default binding.
                            else:
                                raise RuntimeError('导入后材质槽无法匹配：' + name)
                            # UE Python returns struct copies from array iteration.
                            slots[index] = slot
                        obj.set_editor_property(prop, slots)
                if row.get('oldKey') and entry['kind'] == 'skeletalMesh' and owned_skeleton_entry(entry):
                    for prop, suffix, kind in [('skeleton', 'skeleton', 'weaponSkeleton'), ('physics_asset', 'physics', 'weaponPhysics')]:
                        helper = obj.get_editor_property(prop)
                        if helper and str(EAL.get_metadata_tag(helper, KEY)) == row['oldKey'] + ':' + suffix:
                            save(helper, {'key': key + ':' + suffix, 'kind': kind})
                if row['action'] != 'unchanged':
                    synchronize_owned_helpers(entry, obj)
                if entry['kind'] in ('staticMesh', 'skeletalMesh'):
                    verify_lod_report(entry, lod_policy_report(obj, entry))
                saved = {field: value for field, value in entry.items() if field not in ('bones', 'targetBones')}
                if records.get(key, {}).get('linkRevision'):
                    saved['linkRevision'] = records[key]['linkRevision']
                if entry['kind'] == 'material':
                    saved['appliedMaterial'] = current_material_values(obj, entry)
                    saved['materialBindingVersion'] = 1
                if row['action'] != 'unchanged':
                    stage = time.perf_counter()
                    save(obj, saved)
                    timings['saveMs'] += round((time.perf_counter() - stage) * 1000)
                row['path'] = object_path(obj)
                row['status'] = 'unchanged' if row['action'] == 'unchanged' else 'success'
                resolved[key] = obj
                if entry['kind'] == 'skeletalMesh' and row['action'] != 'unchanged':
                    catalog = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
                    owner = suit_by_key[key]
                    part = key.rsplit(':', 1)[-1].lower()
                    is_suit_part = entry.get('config', {}).get('family') == 'suit' and part in ('head', 'upbody', 'downbody', 'arm')
                    ready = catalog and (catalog.update_model_level(obj, key, owner.get('assetId', ''), owner.get('name', ''), part)
                                         if is_suit_part else catalog.update_model_level(obj, key))
                    row['polycountStatus'] = 'ready' if ready else 'pending'
                    if row['polycountStatus'] == 'pending':
                        row['message'] = '模型已发送，面数等级待统计；请确认新版插件已加载'
                if entry['kind'] in ('staticMesh', 'skeletalMesh'):
                    # Replace the inspection hint with the final outcome, rather
                    # than repeating the same warning twice in the result row.
                    if row.get('message', '').startswith(('未配置减面设置：', '将按 UE 已有设置补齐', '将重新验证美术 LOD', '请补充美术 LOD1')):
                        row.pop('message', None)
                    lod_report = lod_policy_report(obj, entry)
                    if lod_report:
                        row['lodReport'] = lod_report
                        missing = lod_report.get('missing', [])
                        if missing:
                            prefix = '模型保持不变；' if row['status'] == 'unchanged' else '模型已发送；'
                            warning = (prefix + '请补充美术 LOD1 后自动生成 LOD2–5。' if entry.get('config', {}).get('lodPolicy') == 'lod1-reduction-v2'
                                       else prefix + '、'.join('LOD' + str(index) for index in missing) + ' 未生成，请在 UE 配置对应减面设置后重试发送。已提供的美术 LOD 保持不变。')
                            row['message'] = (row.get('message', '') + ' ' + warning).strip()
                next_record = {**saved, 'path': row['path'], 'assetId': row['assetId']}
                if records.get(key, {}).get('linkRevision'):
                    next_record['linkRevision'] = records[key]['linkRevision']
                if row['action'] == 'unchanged' and records.get(key, {}).get('linkOnly'):
                    next_record = records[key]
                old_key = row.get('oldKey')
                removed_old_record = bool(old_key and old_key != key and old_key in records)
                if old_key and old_key != key:
                    records.pop(old_key, None)
                if removed_old_record or records.get(key) != next_record:
                    records[key] = next_record
                    stage = time.perf_counter()
                    write_registry(records)
                    timings['registryMs'] += round((time.perf_counter() - stage) * 1000)
            except Exception as error:
                row.update(status='failed', message=str(error))
                unreal.log_error('AssetHive Suit: ' + traceback.format_exc())
    timings['totalMs'] = round((time.perf_counter() - started) * 1000)
    checked['timings'] = timings
    unreal.log('AssetHiveSuitTiming:' + json.dumps(timings))
    return checked


def suit_status(job):
    if job.get('capabilityProbe'):
        capabilities = ['characterStatusV2', 'characterLinkOnlyV1']
        library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
        if library and all(hasattr(library, name) for name in ('parse_link_mesh_source', 'release_link_mesh_source', 'reimport_linked_model', 'get_link_skeleton_bones', 'verify_groom_source', 'copy_groom_import_options')):
            capabilities.append('characterLinkOnlyV2')
        return {'entries': [], 'capabilities': capabilities}
    ids = {suit['assetId'] for suit in job.get('suits', [])}
    records = {key: value for key, value in read_registry().items() if value.get('assetId') in ids}
    # Local registry files can be absent in another checkout/version of the same
    # project. The library's last known paths must still be checked explicitly.
    for suit in job.get('suits', []):
        for entry in suit.get('entries', []):
            key = entry.get('key', '')
            if key.startswith(suit['assetId'] + ':') and entry.get('path', '').startswith('/Game/'):
                records.setdefault(key, {**entry, 'assetId': suit['assetId']})
    found = {}
    unlinked = {}
    missing = []
    for key, value in records.items():
        obj = load(value.get('path'))
        if obj and (str(EAL.get_metadata_tag(obj, KEY)) == key or value.get('linkOnly') and not str(EAL.get_metadata_tag(obj, KEY)) and same_ue_package(value.get('path'), object_path(obj))):
            found[key] = [obj]
        else:
            if obj:
                unlinked[key] = obj
            missing.append(key)
    if missing:
        found.update(owned_objects(missing))
    return {'entries': [{'key': key, 'assetId': value['assetId'],
                         'present': bool(found.get(key) or unlinked.get(key)),
                         'associationPending': len(found.get(key, [])) > 1 or not found.get(key) and key in unlinked,
                         'path': object_path(found[key][0]) if len(found.get(key, [])) == 1 else object_path(unlinked[key]) if key in unlinked and not found.get(key) else ''}
                        for key, value in records.items()]}


def run_job_file(input_path, output_path):
    try:
        with open(input_path, encoding='utf-8-sig') as source:
            job = json.load(source)
        if job.get('version') != 1:
            raise RuntimeError('Unsupported Suit protocol')
        if job.get('phase') in ('linkInspect', 'linkNormalize', 'linkApply'):
            import runpy
            result = runpy.run_path(os.path.join(os.path.dirname(__file__), 'assethive_link_only.py'))['run'](job)
        elif job.get('phase') == 'status':
            result = suit_status(job)
        elif job.get('phase') == 'inspect':
            result = inspect(job)
        elif job.get('phase') == 'apply':
            result = apply_job(job)
        else:
            raise RuntimeError('Unsupported character operation')
    except Exception as error:
        result = {'error': str(error), 'entries': []}
        unreal.log_error(traceback.format_exc())
    with open(output_path, 'w', encoding='utf-8') as output:
        json.dump(result, output, ensure_ascii=False)
