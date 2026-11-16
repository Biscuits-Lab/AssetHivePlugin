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
TEMPLATE = '/Game/MaterialLibrary/Character/MI_Suit_ComplexShading/'
WEAPON_TEMPLATE = '/Game/MaterialLibrary/Weapon/Common/M_Weapon_Slab'


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


def receipt(obj):
    try:
        return json.loads(EAL.get_metadata_tag(obj, STAMP) or '{}')
    except ValueError:
        return {}


def save(obj, entry):
    EAL.set_metadata_tag(obj, KEY, entry['key'])
    EAL.set_metadata_tag(obj, STAMP, json.dumps(entry, ensure_ascii=False))
    if not EAL.save_loaded_asset(obj, only_if_is_dirty=False):
        raise RuntimeError('UE asset save failed: ' + object_path(obj))


def registry_path():
    return os.path.join(unreal.Paths.project_saved_dir(), 'AssetHive', 'SuitImports.json')


def read_registry():
    path = registry_path()
    if not os.path.isfile(path):
        return {}
    with open(path, encoding='utf-8') as source:
        return json.load(source)


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
    # Verified against Zion M_Char_Suit_ComplexShading via the editor, not guessed from texture order.
    mapping = ({'D': 'Diffuse', 'N': 'Normal', 'ORM': 'ORM', 'A': 'OpacityMasked'}
               if entry and weapon_entry(entry) else
               {'D': 'VT_Diffuse', 'N': 'VT_Normal', 'ORM': 'VT_ORM', 'A': 'VT_OpacityMasked'})
    return {channel: name if name in names else channel for channel, name in mapping.items() if name in names or channel in names}


def weapon_entry(entry):
    return entry.get('config', {}).get('family') == 'weapon'


def validate_weapon_helper(entry, path, key, expected_type):
    root = entry['targetPath'].split('/Mesh/')[0].split('/Material/')[0]
    if not path.startswith(root + '/') or any(p in ('', '.', '..') for p in path[1:].split('/')):
        raise RuntimeError('武器依赖路径不属于当前武器：' + path)
    obj = load(path)
    if obj and (not isinstance(obj, expected_type) or str(EAL.get_metadata_tag(obj, KEY)) != key):
        raise RuntimeError('武器依赖名称已被占用：' + path)
    return obj


def weapon_physics_enabled(entry):
    return entry['kind'] == 'skeletalMesh' and weapon_entry(entry) and entry.get('config', {}).get('createPhysicsAsset', False)


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


def weapon_parent(entry):
    """Reuse the project's shared master; never create or modify a base material."""
    path = entry.get('config', {}).get('parentPath', '')
    parent = load(path)
    if path.split('.')[0] != WEAPON_TEMPLATE or not isinstance(parent, unreal.Material):
        raise RuntimeError('武器母材质缺失：' + WEAPON_TEMPLATE)
    parameters = material_parameters(parent, entry)
    for channel in entry.get('textures', {}):
        if channel not in parameters:
            raise RuntimeError('武器母材质缺少贴图参数：' + channel)
    switches = {str(name) for name in MEL.get_static_switch_parameter_names(parent)}
    if not {'Use VT', 'Use OpacityMasked'}.issubset(switches):
        raise RuntimeError('武器母材质缺少 Use VT / Use OpacityMasked 开关')
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


def imported_bone_name(name):
    # Match this engine's FFbxImporter::MakeName + FSkeletalMeshImportData::FixupBoneName.
    return re.sub(r'[.,/`%]', '_', name.rsplit(':', 1)[-1]).strip().replace(' ', '-').lower()


def validate_entry(entry):
    if entry.get('validationError'):
        raise RuntimeError(entry['validationError'])
    path = entry.get('targetPath', '')
    if not path.startswith(WEAPON_ROOT if weapon_entry(entry) else ROOT) or any(segment in ('', '.', '..') for segment in path[1:].split('/')):
        raise RuntimeError('Invalid Suit destination: ' + path)
    if entry['kind'] not in ('texture', 'material', 'staticMesh', 'skeletalMesh'):
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
        if not path.startswith('/Game/') or not isinstance(load(path), unreal.MaterialInterface):
            raise RuntimeError('指定的共享材质不存在或类型不正确：' + path)
    if entry['kind'] != 'material' and not os.path.isfile(entry.get('sourcePath', '')):
        raise RuntimeError('Managed source missing: ' + entry.get('sourcePath', ''))
    if entry['kind'] == 'skeletalMesh':
        owned = weapon_entry(entry) and entry.get('config', {}).get('ownSkeleton')
        skeleton = validate_weapon_helper(entry, entry.get('skeletonPath', ''), entry['key'] + ':skeleton', unreal.Skeleton) if owned else load(entry.get('skeletonPath'))
        if not owned and not isinstance(skeleton, unreal.Skeleton):
            raise RuntimeError('请指定有效的已有 Skeleton；此 SK 暂不导入')
        if weapon_physics_enabled(entry):
            validate_weapon_helper(entry, entry['config'].get('physicsAssetPath', ''), entry['key'] + ':physics', unreal.PhysicsAsset)
        target = {bone['name'].lower(): bone['parent'].lower() for bone in entry.get('targetBones', [])}
        incoming = [{'name': imported_bone_name(bone['name']), 'parent': imported_bone_name(bone['parent'])} for bone in entry.get('bones', [])]
        if len({bone['name'] for bone in incoming}) != len(incoming):
            raise RuntimeError('骨骼名称规范化后重名；请由绑定人员整理')
        incompatible = [bone['name'] for bone in incoming if bone['name'] not in target or target[bone['name']] != bone['parent']]
        if not incoming or ((not owned or skeleton) and (not target or incompatible)):
            raise RuntimeError('FBX 骨骼名称/父子层级与目标 Skeleton 不兼容：' + ', '.join(incompatible[:8]) + '；请由绑定人员确认，不修改共享骨架')
    if entry['kind'] == 'texture' and entry['channel'] == 'N' and entry['config'].get('normalFormat', '').lower() not in ('directx', 'opengl', 'dx', 'gl'):
        raise RuntimeError('请确认法线格式 DirectX / OpenGL')
    if entry['kind'] == 'material':
        if weapon_entry(entry):
            weapon_parent(entry)
            return
        parent = load(entry['config'].get('parentPath'))
        if not parent or not entry['config']['parentPath'].startswith(TEMPLATE):
            raise RuntimeError('套装母材质模板缺失')
        parameters = material_parameters(parent)
        if not MEL.get_material_instance_static_switch_parameter_value(parent, 'Use VT'):
            raise RuntimeError('套装模板未启用 Use VT；请核对工程模板')
        if 'A' in entry['textures'] and not MEL.get_material_instance_static_switch_parameter_value(parent, 'Use OpacityMasked'):
            raise RuntimeError('透明模板未启用 Use OpacityMasked；请核对工程模板')
        for channel in entry['textures']:
            if channel not in parameters:
                raise RuntimeError('母材质贴图参数待指定：' + channel)


def inspect(job):
    records = read_registry()
    resources = [(suit, entry) for suit in job['suits'] for entry in suit['entries']]
    unresolved = []
    located = {}
    for _, entry in resources:
        previous = records.get(entry['key'], {})
        obj = load(previous.get('path'))
        if obj and str(EAL.get_metadata_tag(obj, KEY)) == entry['key']:
            located[entry['key']] = [obj]
        elif previous:
            unresolved.append(entry['key'])
    # Query every identity, so copying an owned asset also surfaces an ambiguity.
    indexed = owned_objects([entry['key'] for _, entry in resources])
    for key, matches in indexed.items():
        if matches:
            located[key] = matches
    rows = []
    for suit, entry in resources:
        row = {'assetId': suit['assetId'], 'key': entry['key'], 'name': entry['name'], 'kind': entry['kind'],
               'partSlot': entry.get('partSlot', ''), 'targetPath': entry['targetPath'], 'status': 'ready', 'action': 'added', 'message': ''}
        try:
            validate_entry(entry)
            matches = located.get(entry['key'], [])
            if len(matches) > 1:
                raise RuntimeError('多个 UE 对象具有相同来源身份，请先明确目标')
            obj = matches[0] if matches else load(entry['targetPath'])
            previous = records.get(entry['key'], {})
            if obj:
                key = str(EAL.get_metadata_tag(obj, KEY))
                row['path'] = object_path(obj)
                if key != entry['key']:
                    if key:
                        raise RuntimeError('此目标属于另一个库资产，不能覆盖')
                    row.update(status='conflict', action='adopt', message='同名 UE 资产尚未关联，确认关联后才可更新')
                else:
                    saved = receipt(obj)
                    row['action'] = 'unchanged' if saved.get('fingerprint') == entry['fingerprint'] else 'updated'
                    # A manually moved object stays where the user put it unless the canonical name changed.
                    if previous.get('targetPath') and previous['targetPath'] != entry['targetPath']:
                        collision = load(entry['targetPath'])
                        if collision and collision != obj:
                            raise RuntimeError('改名目标已被其他 UE 对象占用')
                        row['action'] = 'renamed' if row['action'] == 'unchanged' else 'updatedRenamed'
                    if entry['kind'] == 'material' and row['action'] not in ('unchanged', 'renamed'):
                        current = current_material_values(obj, entry)
                        row['currentMaterial'] = current
                        if material_conflicts(current, saved, entry):
                            row.update(status='conflict', message='UE 材质已手调：选择保留 UE 或采用本次映射')
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
        elif entry['kind'] in ('staticMesh', 'skeletalMesh'):
            slots = obj.get_editor_property('static_materials' if entry['kind'] == 'staticMesh' else 'materials')
            repair = [slot_name(slot) for slot in slots
                      if not slot.material_interface and entry['materials'].get(slot_name(slot)) in missing]
        if weapon_physics_enabled(entry) and not obj.get_editor_property('physics_asset'):
            row.update(action='updated', message='补充武器物理资产')
        elif repair:
            row.update(action='repair', repair=repair, message='依赖资源已删除；仅在你补回后修复空引用')
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
    previous_physics = weapon_physics_for_import(entry, load(destination)) if weapon_physics_enabled(entry) else None
    if entry['kind'] == 'texture':
        task.factory = unreal.TextureFactory()
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
        mesh_import_data.set_editor_property('convert_scene', True)
        mesh_import_data.set_editor_property('force_front_x_axis', False)
        task.options = options
        task.factory = unreal.FbxFactory()
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    obj = load(destination)
    expected = {'texture': unreal.Texture2D, 'staticMesh': unreal.StaticMesh, 'skeletalMesh': unreal.SkeletalMesh}[entry['kind']]
    if not isinstance(obj, expected) or not task.imported_object_paths:
        raise RuntimeError('UE 未返回预期类型的导入资产：' + entry['name'])
    if entry['kind'] == 'skeletalMesh':
        skeleton = obj.get_editor_property('skeleton')
        if weapon_entry(entry) and not load(entry['skeletonPath']):
            if not skeleton or not EAL.rename_asset(object_path(skeleton), entry['skeletonPath']):
                raise RuntimeError('武器骨架规范命名失败')
            save(skeleton, {'key': entry['key'] + ':skeleton', 'kind': 'weaponSkeleton'})
        if skeleton != load(entry['skeletonPath']):
            raise RuntimeError('导入后的 Skeleton 不符合目标')
    if weapon_physics_enabled(entry):
        save_weapon_physics(entry, obj, previous_physics)
    return obj


def apply_texture(obj, entry):
    channel = entry['channel']
    properties = {
        'compression_settings': unreal.TextureCompressionSettings.TC_NORMALMAP if channel == 'N'
            else unreal.TextureCompressionSettings.TC_DEFAULT if channel == 'D' else unreal.TextureCompressionSettings.TC_MASKS,
        'srgb': channel == 'D',
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
    changed = {name: value for name, value in properties.items() if obj.get_editor_property(name) != value}
    # UObject.set_editor_properties issues one Pre/PostEditChange for the batch.
    # Individual setters rebuild texture resources repeatedly, even for unchanged values.
    if changed:
        obj.set_editor_properties(changed)


def apply_job(job):
    started = time.perf_counter()
    checked = inspect(job)
    timings = {'inspectMs': round((time.perf_counter() - started) * 1000), 'hashMs': 0, 'importMs': 0, 'textureSettingsMs': 0, 'saveMs': 0, 'registryMs': 0}
    if checked['inspectToken'] != job.get('inspectToken'):
        raise RuntimeError('UE 状态在检查后变化，请重新检查')
    records = read_registry()
    decisions = job.get('decisions', {})
    by_key = {entry['key']: entry for suit in job['suits'] for entry in suit['entries']}
    resolved = {}
    pending_count = sum(row['action'] not in ('unchanged', 'retained') for row in checked['entries'])
    families = {asset.get('family', 'suit') for asset in job['suits']}
    labels = {'suit': '套装', 'weapon': '武器', 'monster': '怪物', 'npc': 'NPC', 'hair': '头发', 'face': '脸部'}
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
                if row['action'] in ('renamed', 'updatedRenamed'):
                    if destination != entry['targetPath'] and not EAL.rename_asset(destination, entry['targetPath']):
                        raise RuntimeError('UE 资产改名失败；请检查目标冲突、写入权限和版本控制连接')
                    destination = entry['targetPath']; obj = load(destination)
                if row['action'] in ('unchanged', 'renamed'):
                    if not obj:
                        raise RuntimeError('UE 资产在执行时缺失')
                    if row['action'] == 'renamed' and entry['kind'] != 'material':
                        import_data = obj.get_editor_property('asset_import_data')
                        if import_data:
                            import_data.scripted_add_filename(entry['sourcePath'], 0, '')
                elif entry['kind'] == 'material':
                    parent = weapon_parent(entry) if weapon_entry(entry) else load(entry['config']['parentPath'])
                    if not obj:
                        factory = unreal.MaterialInstanceConstantFactoryNew()
                        directory, name = destination.rsplit('/', 1)
                        obj = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, directory, unreal.MaterialInstanceConstant, factory)
                        MEL.set_material_instance_parent(obj, parent)
                    if not isinstance(obj, unreal.MaterialInstanceConstant):
                        raise RuntimeError('同名对象不是材质实例')
                    if decision != 'keepUE':
                        previous_receipt = receipt(obj)
                        if previous_receipt.get('config', {}).get('parentPath') != entry['config']['parentPath']:
                            MEL.set_material_instance_parent(obj, parent)
                        if weapon_entry(entry):
                            configure_weapon_instance(obj, entry, previous_receipt)
                        parent_changed = previous_receipt.get('config', {}).get('parentPath') != entry['config']['parentPath']
                        parameters = material_parameters(obj, entry)
                        saved_mapping = receipt(obj).get('textures', {})
                        for channel, texture_key in entry['textures'].items():
                            if channel not in parameters:
                                raise RuntimeError('UE 手调母材质缺少参数：' + channel)
                            if parent_changed or saved_mapping.get(channel) != texture_key or decision == 'applyMapping' or not receipt(obj) or channel in row.get('repair', []):
                                MEL.set_material_instance_texture_parameter_value(obj, parameters[channel], resolved[texture_key])
                        MEL.update_material_instance(obj)
                else:
                    if weapon_entry(entry) and entry['kind'] == 'skeletalMesh' and obj and not load(entry['skeletonPath']):
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
                    if obj and receipt(obj) and entry['kind'] in ('staticMesh', 'skeletalMesh'):
                        slots = obj.get_editor_property('static_materials' if entry['kind'] == 'staticMesh' else 'materials')
                        old_materials = {str(slot.material_slot_name): slot.material_interface for slot in slots}
                    old_config = receipt(obj).get('config') if obj else None
                    if row['action'] != 'repair':
                        stage = time.perf_counter()
                        obj = import_file(entry, destination)
                        timings['importMs'] += round((time.perf_counter() - stage) * 1000)
                    if entry['kind'] == 'texture':
                        if old_config != entry['config']:
                            stage = time.perf_counter()
                            apply_texture(obj, entry)
                            timings['textureSettingsMs'] += round((time.perf_counter() - stage) * 1000)
                    else:
                        prop = 'static_materials' if entry['kind'] == 'staticMesh' else 'materials'
                        slots = obj.get_editor_property(prop)
                        for index, slot in enumerate(slots):
                            name = slot_name(slot)
                            if str(slot.material_slot_name) in old_materials and name not in row.get('repair', []):
                                slot.material_interface = old_materials[str(slot.material_slot_name)]
                            elif name in entry['materials']:
                                slot.material_interface = resolved[entry['materials'][name]]
                            elif name in entry.get('externalMaterials', {}):
                                slot.material_interface = load(entry['externalMaterials'][name])
                            else:
                                raise RuntimeError('导入后材质槽无法匹配：' + name)
                            # UE Python returns struct copies from array iteration.
                            slots[index] = slot
                        obj.set_editor_property(prop, slots)
                saved = dict(entry)
                if entry['kind'] == 'material':
                    saved['appliedMaterial'] = current_material_values(obj, entry)
                if row['action'] != 'unchanged':
                    stage = time.perf_counter()
                    save(obj, saved)
                    timings['saveMs'] += round((time.perf_counter() - stage) * 1000)
                row['path'] = object_path(obj)
                row['status'] = 'unchanged' if row['action'] == 'unchanged' else 'success'
                resolved[key] = obj
                next_record = {**saved, 'path': row['path'], 'assetId': row['assetId']}
                if records.get(key) != next_record:
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
    ids = {suit['assetId'] for suit in job.get('suits', [])}
    records = {key: value for key, value in read_registry().items() if value.get('assetId') in ids}
    found = {}
    missing = []
    for key, value in records.items():
        obj = load(value.get('path'))
        if obj and str(EAL.get_metadata_tag(obj, KEY)) == key:
            found[key] = [obj]
        else:
            missing.append(key)
    if missing:
        found.update(owned_objects(missing))
    return {'entries': [{'key': key, 'assetId': value['assetId'],
                         'present': len(found.get(key, [])) == 1,
                         'path': object_path(found[key][0]) if len(found.get(key, [])) == 1 else ''}
                        for key, value in records.items()]}


def run_job_file(input_path, output_path):
    try:
        with open(input_path, encoding='utf-8-sig') as source:
            job = json.load(source)
        if job.get('version') != 1:
            raise RuntimeError('Unsupported Suit protocol')
        result = suit_status(job) if job.get('phase') == 'status' else inspect(job) if job.get('phase') == 'inspect' else apply_job(job)
    except Exception as error:
        result = {'error': str(error), 'entries': []}
        unreal.log_error(traceback.format_exc())
    with open(output_path, 'w', encoding='utf-8') as output:
        json.dump(result, output, ensure_ascii=False)
