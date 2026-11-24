"""Read-only content inspection and sidecar-only association. Never save a UObject."""
import hashlib
import json
import os
import uuid
from pathlib import Path
import unreal


def digest_file(path):
    with open(path, 'rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()


def package_path(value):
    return str(value).split('.', 1)[0]


def package_files(value):
    package = package_path(value)
    if package.startswith('/Game/'):
        base = Path(unreal.Paths.project_content_dir()) / package[len('/Game/'):]
    elif package.startswith('/Engine/'):
        base = Path(unreal.Paths.engine_content_dir()) / package[len('/Engine/'):]
    else:
        raise RuntimeError('不支持的资源包位置：' + package)
    paths = [str(base) + extension for extension in ('.uasset', '.uexp', '.ubulk') if Path(str(base) + extension).is_file()]
    if not paths or not Path(str(base) + '.uasset').is_file():
        raise RuntimeError('资源尚未保存或已在磁盘删除：' + package)
    return paths


def snapshot(obj):
    package = package_path(obj.get_path_name())
    dirty = {p.get_name().casefold() for p in unreal.EditorLoadingAndSavingUtils.get_dirty_content_packages()}
    if package.casefold() in dirty:
        raise RuntimeError('资源存在未保存编辑，请保存后重新核验：' + package)
    files = package_files(package)
    return {'path': obj.get_path_name(), 'class': obj.get_class().get_name(),
            'files': {os.path.basename(file): digest_file(file) for file in files}}


def dependencies(obj):
    result = {}
    def add(value):
        if value and value.get_path_name() not in result:
            state = snapshot(value)
            result[state['path']] = state
            if isinstance(value, unreal.MaterialInstanceConstant):
                add(value.get_editor_property('parent'))
                for name in unreal.MaterialEditingLibrary.get_texture_parameter_names(value):
                    add(unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(value, name))
            elif isinstance(value, unreal.Material):
                for texture in unreal.MaterialEditingLibrary.get_used_textures(value):
                    add(texture)
    if isinstance(obj, unreal.MaterialInstanceConstant):
        add(obj.get_editor_property('parent'))
        for name in unreal.MaterialEditingLibrary.get_texture_parameter_names(obj):
            add(unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(obj, name))
    groom_class = getattr(unreal, 'GroomAsset', None)
    if groom_class and isinstance(obj, groom_class):
        for slot in obj.get_editor_property('hair_groups_materials'):
            add(slot.get_editor_property('material'))
    if isinstance(obj, (unreal.StaticMesh, unreal.SkeletalMesh)):
        slots = obj.get_editor_property('static_materials' if isinstance(obj, unreal.StaticMesh) else 'materials')
        for slot in slots:
            material = slot.get_editor_property('material_interface')
            add(material)
            if material:
                if isinstance(material, unreal.MaterialInstanceConstant):
                    add(material.get_editor_property('parent'))
                    for name in unreal.MaterialEditingLibrary.get_texture_parameter_names(material):
                        add(unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(material, name))
                elif isinstance(material, unreal.Material):
                    for texture in unreal.MaterialEditingLibrary.get_used_textures(material):
                        add(texture)
        if isinstance(obj, unreal.SkeletalMesh):
            add(obj.get_editor_property('skeleton'))
            add(obj.get_editor_property('physics_asset'))
    return result


def material_contract(obj, entry, targets):
    from assethive_suit_import import current_material_values
    current = current_material_values(obj, entry)
    canonical = lambda value: package_path(value).casefold()
    if not current or canonical(current['parent']) != canonical(entry.get('config', {}).get('parentPath', '')):
        raise RuntimeError('材质母材质不一致，未修改 UE 材质')
    for channel, key in entry.get('textures', {}).items():
        if key not in targets or canonical(current['textures'].get(channel, '')) != canonical(targets[key]):
            raise RuntimeError('材质贴图依赖不一致：' + channel)
    return current


def store_path():
    return Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())) / 'Config/AssetHive/Links.json'


def material_evidence(obj, entry, targets):
    expected = dict(entry.get('linkMaterialPaths', {}))
    expected.update(entry.get('externalMaterials', {}))
    for slot, key in entry.get('materials', {}).items():
        if key not in targets:
            raise RuntimeError('材质依赖记录不完整')
        expected[slot] = targets[key]
    if entry['kind'] not in ('staticMesh', 'skeletalMesh'):
        return {}, {}
    if entry.get('unresolvedMaterials'):
        raise RuntimeError('源模型存在未确定的材质映射')
    slots = obj.get_editor_property('static_materials' if isinstance(obj, unreal.StaticMesh) else 'materials')
    exported = {}
    for index, slot in enumerate(slots):
        material = slot.get_editor_property('material_interface')
        if not material:
            continue
        name, path = material.get_name(), package_path(material.get_path_name())
        if name in exported and exported[name].casefold() != path.casefold():
            raise RuntimeError('不同材质使用相同导出名称，无法唯一核验')
        if name in exported:
            # UE FBX exporter appends the slot index for repeated bindings.
            alias = name + '_' + str(index)
            if alias in exported and exported[alias].casefold() != path.casefold():
                raise RuntimeError('重复材质导出名称存在歧义')
            exported[alias] = path
        exported[name] = path
    # FBX exports use bound material names, which may differ from UE slot names.
    # The desktop compares these resolved identities on EVERY triangle.
    return expected, exported if expected else {}


def read_links():
    file = store_path()
    return json.loads(file.read_text(encoding='utf-8')) if file.is_file() else {}


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix('.' + uuid.uuid4().hex + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(temporary, path)


def check_dir(token):
    if len(token) != 32 or any(c not in '0123456789abcdef' for c in token):
        raise RuntimeError('Invalid link check token')
    return Path(unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_saved_dir())) / 'AssetHive/LinkChecks' / token


def prepare(job):
    from assethive_suit_import import read_registry, same_ue_package
    associations = read_registry()
    token = uuid.uuid4().hex
    directory = check_dir(token)
    directory.mkdir(parents=True, exist_ok=True)
    links = read_links()
    rows = []
    result = {'entries': rows, 'inspectToken': token, 'projectPath': unreal.Paths.convert_relative_path_to_full(unreal.Paths.get_project_file_path())}
    for suit in job['suits']:
        targets = {entry['key']: entry['targetPath'] for entry in suit['entries']}
        for entry in suit['entries']:
            row = {'assetId': suit['assetId'], 'key': entry['key'], 'name': entry['name'], 'kind': entry['kind'],
                   'partSlot': entry.get('partSlot', ''), 'targetPath': entry['targetPath'], 'status': 'blocked', 'action': 'linkOnly'}
            try:
                from assethive_suit_import import resolve_hero_shared_materials
                resolve_hero_shared_materials(entry)
                expected = {'staticMesh': unreal.StaticMesh, 'skeletalMesh': unreal.SkeletalMesh, 'texture': unreal.Texture2D,
                            'material': unreal.MaterialInstanceConstant, 'groom': getattr(unreal, 'GroomAsset', None)}.get(entry['kind'])
                if not expected:
                    raise RuntimeError('此类型尚不支持仅关联')
                candidates = list({package_path(value).casefold(): package_path(value) for value in
                                   (links.get(entry['key'], {}).get('path'), entry['targetPath']) if value}.values())
                if any(not path.startswith('/Game/') or '..' in path.split('/') for path in candidates):
                    raise RuntimeError('Invalid link path')
                existing = [path for path in candidates if unreal.EditorAssetLibrary.does_asset_exist(path)]
                if len(existing) > 1:
                    raise RuntimeError('历史位置与目标位置均存在候选，请先确认整套资源位置')
                obj = unreal.EditorAssetLibrary.load_asset(existing[0]) if existing else None
                if not obj:
                    row['status'] = 'missing'
                    raise RuntimeError('当前 UE 中不存在，不会自动导入')
                row['path'] = obj.get_path_name()
                if not isinstance(obj, expected):
                    raise RuntimeError('同名资源类型不一致')
                owner = str(unreal.EditorAssetLibrary.get_metadata_tag(obj, 'AssetHive.SuitKey'))
                if owner and owner != entry['key']:
                    raise RuntimeError('资源已属于其他 AssetHive 身份')
                if any(key != entry['key'] and package_path(value['path']).casefold() == package_path(obj.get_path_name()).casefold() for key, value in links.items()):
                    raise RuntimeError('该对象已关联另一库文件')
                state, deps = snapshot(obj), dependencies(obj)
                baseline = {key: value for key, value in entry.items() if key not in ('bones', 'targetBones')}
                if entry['kind'] in ('staticMesh', 'skeletalMesh'):
                    subsystem_type = unreal.StaticMeshEditorSubsystem if entry['kind'] == 'staticMesh' else unreal.SkeletalMeshEditorSubsystem
                    subsystem = unreal.get_editor_subsystem(subsystem_type)
                    count = subsystem.get_lod_count(obj)
                    pending_lods = [lod['index'] for lod in entry.get('config', {}).get('lodSources', []) if lod['index'] >= count]
                    row['pendingLods'] = pending_lods
                    if pending_lods:
                        baseline['config'] = {**entry['config'], 'lodSources': [lod for lod in entry['config']['lodSources'] if lod['index'] < count]}
                        baseline['linkedManifestFingerprint'] = entry.get('fingerprint')
                        # Missing LODs are not part of the verified baseline.
                        # The next explicit send updates LODs without LOD0 import.
                        baseline['fingerprint'] = None
                row.update(state=state, dependencies=deps, baseline=baseline, fingerprint=entry.get('fingerprint'))
                previous = associations.get(entry['key'], {})
                row['alreadyLinked'] = bool(previous and same_ue_package(previous.get('path'), row['path'])
                                            and (previous.get('linkedManifestFingerprint') or previous.get('fingerprint')) == entry.get('fingerprint'))
                if entry['kind'] == 'material':
                    baseline['appliedMaterial'] = material_contract(obj, entry, targets)
                    baseline['materialBindingVersion'] = 1
                    row.update(status='checking', verification='materialContract')
                    rows.append(row)
                    atomic_json(directory / 'check.json', result)
                    continue
                source_materials, exported_materials = material_evidence(obj, entry, targets)
                expected_materials = list(entry.get('externalMaterials', {}).values()) + [targets[key] for key in entry.get('materials', {}).values() if key in targets]
                present = {package_path(path).casefold() for path in deps}
                if any(package_path(value).casefold() not in present for value in expected_materials):
                    raise RuntimeError('材质依赖不一致，不改动现有绑定')
                if entry['kind'] == 'skeletalMesh' and entry.get('skeletonPath') and package_path(entry['skeletonPath']).casefold() not in present:
                    if not entry.get('config', {}).get('ownSkeleton') or unreal.EditorAssetLibrary.does_asset_exist(entry['skeletonPath']):
                        raise RuntimeError('骨架依赖不一致')
                    skeleton = obj.get_editor_property('skeleton')
                    if not skeleton:
                        raise RuntimeError('模型缺少有效骨架')
                    # Only the skeleton actually used by the verified model is
                    # retained. Never infer a skeleton from its basename.
                    baseline['linkedSkeletonPath'] = skeleton.get_path_name()
                    baseline['linkedCanonicalSkeletonPath'] = entry['skeletonPath']
                source = entry.get('sourcePath', '')
                if not source or digest_file(source) != entry.get('sourceHash'):
                    raise RuntimeError('源文件发生变化，请重新检查')
                if entry['kind'] == 'groom':
                    library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
                    if not library or not hasattr(library, 'verify_groom_source'):
                        raise RuntimeError('请更新正式插件，以核验 Groom 源内容')
                    if not library.verify_groom_source(obj, source):
                        raise RuntimeError('Groom 曲线或属性不一致，或缺少可核验的导入设置')
                    row.update(status='checking', verification='groomContent', sourcePath=source, sourceHash=entry['sourceHash'])
                    rows.append(row)
                    atomic_json(directory / 'check.json', result)
                    continue
                export = directory / (str(len(rows)) + ('.tga' if entry['kind'] == 'texture' else '.fbx'))
                task = unreal.AssetExportTask()
                task.object = obj
                task.filename = str(export)
                task.automated = True
                task.prompt = False
                task.replace_identical = True
                if entry['kind'] == 'texture':
                    task.exporter = unreal.TextureExporterTGA()
                else:
                    task.exporter = unreal.SkeletalMeshExporterFBX() if entry['kind'] == 'skeletalMesh' else unreal.StaticMeshExporterFBX()
                    task.options = unreal.FbxExportOption()
                    task.options.ascii = False
                    task.options.level_of_detail = bool(entry.get('config', {}).get('lodSources'))
                    task.options.export_source_mesh = False
                    task.options.collision = False
                    task.options.vertex_color = True
                    task.options.export_morph_targets = True
                    task.options.bake_material_inputs = unreal.FbxMaterialBakeMode.DISABLED
                if not unreal.Exporter.run_asset_export_task(task) or not export.is_file():
                    raise RuntimeError('无法读取 UE 内容用于核验')
                lod_sources = entry.get('config', {}).get('lodSources', [])
                for lod in lod_sources:
                    if digest_file(lod['sourcePath']) != lod['sourceHash']:
                        raise RuntimeError('美术 LOD 文件已变化，请重新核验')
                row.update(status='checking', path=obj.get_path_name(), exportedPath=str(export), exportedHash=digest_file(export),
                           sourcePath=source, sourceHash=entry['sourceHash'], state=state, dependencies=deps,
                           sourceMaterialPaths=source_materials, exportedMaterialPaths=exported_materials, lodSources=lod_sources)
            except Exception as error:
                row['message'] = str(error)
            rows.append(row)
            atomic_json(directory / 'check.json', result)
    atomic_json(directory / 'check.json', result)
    return result


def apply(job):
    prepared = json.loads((check_dir(job['inspectToken']) / 'check.json').read_text(encoding='utf-8'))
    links = read_links()
    before = json.dumps(links, sort_keys=True)
    selected = set(job.get('verifiedKeys', []))
    rows = []
    completed = set()
    order = {'texture': 0, 'material': 1, 'staticMesh': 2, 'skeletalMesh': 2, 'groom': 2}
    for original in sorted(prepared['entries'], key=lambda value: order.get(value['kind'], 3)):
        row = dict(original)
        if row['key'] not in selected or row['status'] != 'checking':
            row.update(status='skipped')
        else:
            try:
                baseline = row.get('baseline', {})
                required = baseline.get('textures', {}) if row['kind'] == 'material' else baseline.get('materials', {})
                if any(key not in completed for key in required.values()):
                    raise RuntimeError('关联依赖未完成，当前资源未关联')
                obj = unreal.EditorAssetLibrary.load_asset(row['path'])
                if not obj or snapshot(obj) != row['state'] or dependencies(obj) != row['dependencies']:
                    raise RuntimeError('UE 内容或依赖已变化，需重新核验')
                if row.get('verification') != 'materialContract' and digest_file(row['sourcePath']) != row['sourceHash']:
                    raise RuntimeError('核验源文件已变化')
                if row.get('exportedPath') and digest_file(row['exportedPath']) != row['exportedHash']:
                    raise RuntimeError('核验文件已变化')
                if any(digest_file(lod['sourcePath']) != lod['sourceHash'] for lod in row.get('lodSources', [])):
                    raise RuntimeError('美术 LOD 文件已变化，需重新核验')
                if any(digest_file(level['exportedPath']) != level['exportedHash'] for level in row.get('normalizedSources', [])):
                    raise RuntimeError('内容解析缓存已变化，需重新核验')
                owner = str(unreal.EditorAssetLibrary.get_metadata_tag(obj, 'AssetHive.SuitKey'))
                if owner and owner != row['key']:
                    raise RuntimeError('对象归属发生变化')
                if any(key != row['key'] and package_path(value['path']).casefold() == package_path(row['path']).casefold() for key, value in links.items()):
                    raise RuntimeError('对象已关联其他文件')
                if row.get('alreadyLinked'):
                    from assethive_suit_import import read_registry, same_ue_package
                    previous = read_registry().get(row['key'], {})
                    if not same_ue_package(previous.get('path'), row['path']) or (previous.get('linkedManifestFingerprint') or previous.get('fingerprint')) != row.get('fingerprint'):
                        raise RuntimeError('关联记录已变化，请重新检查')
                    completed.add(row['key'])
                    row.update(status='unchanged', message='已关联；内容与依赖核验通过，无需重复关联')
                    rows.append(row)
                    continue
                links[row['key']] = dict(row.get('baseline', {}))
                links[row['key']].update({field: row[field] for field in ['assetId', 'kind', 'path', 'state', 'dependencies']})
                if 'sourceHash' in row:
                    links[row['key']]['sourceHash'] = row['sourceHash']
                links[row['key']]['linkOnly'] = True
                links[row['key']]['linkRevision'] = uuid.uuid4().hex
                completed.add(row['key'])
                row.update(status='success', message='仅关联完成；UE 资源内容与依赖未修改')
            except Exception as error:
                row.update(status='failed', message=str(error))
        rows.append(row)
    if json.dumps(links, sort_keys=True) != before:
        atomic_json(store_path(), links)
    return {'entries': rows, 'inspectToken': job['inspectToken'], 'projectPath': prepared['projectPath']}


def normalize_sources(job):
    directory = check_dir(job['inspectToken'])
    prepared = json.loads((directory / 'check.json').read_text(encoding='utf-8'))
    keys = set(job.get('keys', []))
    output = []
    library = getattr(unreal, 'AssetHiveModelCatalogLibrary', None)
    if not library or not all(hasattr(library, name) for name in ('parse_link_mesh_source', 'release_link_mesh_source')):
        raise RuntimeError('请更新正式插件，以核验原始 FBX 与 UE 坐标/导入设置的一致性')
    for row in prepared['entries']:
        if row['key'] not in keys or row['status'] != 'checking' or row['kind'] not in ('staticMesh', 'skeletalMesh'):
            continue
        obj = unreal.EditorAssetLibrary.load_asset(row['path'])
        if snapshot(obj) != row['state'] or dependencies(obj) != row['dependencies']:
            raise RuntimeError('UE 内容或依赖已变化，请重新核验')
        sources = [{'index': 0, 'sourcePath': row['sourcePath'], 'sourceHash': row['sourceHash']}] + [lod for lod in row.get('lodSources', []) if lod['index'] not in row.get('pendingLods', [])]
        normalized = []
        model = None
        try:
            for source in sources:
                if digest_file(source['sourcePath']) != source['sourceHash']:
                    raise RuntimeError('源文件已变化，请重新核验')
                if source['index'] == 0:
                    model = library.parse_link_mesh_source(obj, source['sourcePath'])
                else:
                    subsystem_type = unreal.StaticMeshEditorSubsystem if row['kind'] == 'staticMesh' else unreal.SkeletalMeshEditorSubsystem
                    subsystem = unreal.get_editor_subsystem(subsystem_type) or unreal.get_default_object(subsystem_type)
                    if subsystem.import_lod(model, source['index'], source['sourcePath']) != source['index']:
                        raise RuntimeError('无法解析源 LOD' + str(source['index']))
                    library.protect_imported_lod(model, source['index'])
                if not model or not model.get_path_name().startswith('/Temp/AssetHiveLink_'):
                    raise RuntimeError('无法只读解析源模型')
            if model:
                if row['kind'] == 'staticMesh':
                    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
                    # Interchange assets have no UFbxStaticMeshImportData for
                    # the native parser to copy. Match actual build settings on
                    # the private model, including generated lightmap UVs.
                    for source in sources:
                        settings = subsystem.get_lod_build_settings(obj, source['index'])
                        subsystem.set_lod_build_settings(model, source['index'], settings)
                mappings = {name.casefold(): value for name, value in row['sourceMaterialPaths'].items()}
                prop = 'static_materials' if row['kind'] == 'staticMesh' else 'materials'
                slots = model.get_editor_property(prop)
                for index, slot in enumerate(slots):
                    if not mappings:
                        continue
                    name = str(slot.get_editor_property('imported_material_slot_name'))
                    path = mappings.get(name.casefold()) or mappings.get(str(slot.material_slot_name).casefold())
                    if not path:
                        raise RuntimeError('源模型材质映射不完整：' + name)
                    slot.material_interface = unreal.EditorAssetLibrary.load_asset(path)
                    slots[index] = slot
                model.set_editor_property(prop, slots)
                destination = directory / (hashlib.sha256(row['key'].encode('utf-8')).hexdigest()[:16] + '_normalized.fbx')
                task = unreal.AssetExportTask()
                task.object = model
                task.filename = str(destination)
                task.automated = True
                task.prompt = False
                task.replace_identical = True
                task.exporter = unreal.StaticMeshExporterFBX() if row['kind'] == 'staticMesh' else unreal.SkeletalMeshExporterFBX()
                task.options = unreal.FbxExportOption()
                task.options.ascii = False
                task.options.level_of_detail = len(sources) > 1
                task.options.export_source_mesh = False
                task.options.collision = False
                task.options.vertex_color = True
                task.options.export_morph_targets = True
                task.options.bake_material_inputs = unreal.FbxMaterialBakeMode.DISABLED
                if not unreal.Exporter.run_asset_export_task(task):
                    raise RuntimeError('无法读取解析后的源内容')
                normalized = [{**source, 'sourceLod':source['index'], 'exportedPath':str(destination),'exportedHash':digest_file(destination)} for source in sources]
        finally:
            if model:
                library.release_link_mesh_source(model)
        if snapshot(obj) != row['state'] or dependencies(obj) != row['dependencies']:
            raise RuntimeError('核验过程中目标内容发生变化，未关联')
        row['normalizedSources'] = normalized
        output.append({'key':row['key'],'normalizedSources':normalized})
    atomic_json(directory / 'check.json', prepared)
    return {'entries':output, 'inspectToken':job['inspectToken']}


def run(job):
    return {'linkInspect': prepare, 'linkNormalize': normalize_sources, 'linkApply': apply}[job['phase']](job)
