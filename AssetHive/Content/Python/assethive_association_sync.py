"""Resolve existing character objects before the normal inspect/apply pipeline."""
import os
import re
import json
import unreal


def identity(value):
    name = str(value or '').replace('\\', '/').rsplit('/', 1)[-1]
    name = name.split('.', 1)[0]
    return re.sub(r'_lod0$', '', name, flags=re.I).casefold()


def unique_candidates(names, candidates):
    """Only exact known names are evidence; never fuzzy-match a different character."""
    expected = {identity(name) for name in names if name}
    return sorted({path for name, path in candidates if identity(name) in expected})


def prepare(job, api):
    if not job.get('associationSync') or job.get('_associationPrepared'):
        return
    records = api.read_registry()
    entries = [entry for suit in job['suits'] for entry in suit['entries']]
    classes = {'texture': 'Texture2D', 'material': 'MaterialInstanceConstant',
               'staticMesh': 'StaticMesh', 'skeletalMesh': 'SkeletalMesh', 'groom': 'GroomAsset'}
    # One registry scan; do not load every mesh in the project to search names.
    index = {}
    identities = {}
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    for data in registry.get_assets_by_path('/Game', recursive=True):
        kind = str(data.asset_class_path.asset_name)
        index.setdefault(kind, []).append((str(data.asset_name), str(data.package_name)))
        owner = str(data.get_tag_value(api.KEY) or '')
        if owner:
            identities.setdefault(owner, []).append(str(data.package_name))
    remapped = {}
    used = {}
    for entry in entries:
        try:
            old_path = entry['targetPath']
            known = records.get(entry['key'], {}).get('path')
            authoritative = set(identities.get(entry['key'], []))
            shared_keys = set(entry.get('associationReferenceKeys', [])) if entry['kind'] in ('material', 'texture') else set()
            shared_paths = set()
            for shared_key in shared_keys:
                shared_paths.update(identities.get(shared_key, []))
                shared_path = records.get(shared_key, {}).get('path')
                if shared_path and api.load(shared_path):
                    shared_paths.add(shared_path.split('.', 1)[0])
            if known and api.load(known):
                authoritative.add(known.split('.', 1)[0])
            if len(authoritative) > 1 and not shared_keys:
                raise RuntimeError('同一资产存在多个已关联对象，请联系 TA 核对重复记录')
            names = [entry['name'], os.path.basename(entry.get('sourcePath', '')), *entry.get('associationNames', [])]
            matches = sorted(authoritative) if authoritative else unique_candidates(names, index.get(classes[entry['kind']], []))
            # Include the exact planned destination, even when its name is historical.
            if not authoritative and api.load(old_path) and old_path not in matches:
                matches.append(old_path)
            if shared_paths:
                # Explicit shared membership is the proof, never equal names.
                matches = sorted(authoritative or shared_paths)
            if len(matches) > 1 and not shared_paths:
                raise RuntimeError('找到多个可能对应的 UE 对象，未自动覆盖；请 TA 核对：' + '、'.join(matches))
            if not matches:
                continue
            target = old_path if old_path in matches else matches[0]
            obj = api.load(target)
            if not obj or obj.get_class().get_name() != classes[entry['kind']]:
                raise RuntimeError('找到的 UE 对象类型不匹配，未覆盖：' + target)
            owner = str(api.EAL.get_metadata_tag(obj, api.KEY))
            if owner and owner != entry['key'] and owner not in shared_keys and not api.legacy_blockout_key_matches(entry, owner, api.receipt(obj)):
                raise RuntimeError('该 UE 对象已属于另一个库资产，请 TA 核对归属：' + target)
            if target.casefold() in used and used[target.casefold()] != entry['key']:
                message = '两个库资源指向同一个 UE 对象，请先核对共享关系：' + target
                next(value for value in entries if value['key'] == used[target.casefold()])['validationError'] = message
                raise RuntimeError(message)
            used[target.casefold()] = entry['key']
            entry['targetPath'] = target
            entry['associationExistingPath'] = target
            entry['associationCanonicalPath'] = old_path
            duplicates = sorted((shared_paths | authoritative) - {target})
            for path in duplicates:
                duplicate = api.load(path)
                if not duplicate or duplicate.get_class().get_name() != classes[entry['kind']] or str(api.EAL.get_metadata_tag(duplicate, api.KEY)) not in shared_keys | {entry['key']}:
                    raise RuntimeError('旧共享副本归属发生变化，请重新检查：' + path)
            entry['associationMergePaths'] = duplicates
            remapped[old_path] = target
            if entry['kind'] == 'skeletalMesh':
                skeleton = obj.get_editor_property('skeleton')
                if skeleton and not entry['config'].get('skeletonOverride'):
                    entry['linkedCanonicalSkeletonPath'] = entry.get('skeletonPath', '')
                    entry['skeletonPath'] = api.object_path(skeleton).split('.', 1)[0]
                    entry['linkedSkeletonPath'] = entry['skeletonPath']
                    entry['targetBones'] = json.loads(unreal.AssetHiveModelCatalogLibrary.get_link_skeleton_bones(skeleton))
                    entry['config'] = {**entry['config'], 'ownSkeleton': False, 'createPhysicsAsset': False}
                    physics = obj.get_editor_property('physics_asset')
                    if physics:
                        entry['config']['physicsAssetPath'] = api.object_path(physics).split('.', 1)[0]
        except Exception as error:
            entry['validationError'] = str(error)
    # Preserve explicit material references, including SM-to-SK LOD mappings.
    def replace(value):
        if isinstance(value, dict):
            return {key: replace(item) for key, item in value.items()}
        if isinstance(value, list):
            return [replace(item) for item in value]
        return remapped.get(value, value) if isinstance(value, str) else value
    for entry in entries:
        for field in ['config', 'externalMaterials', 'linkMaterialPaths']:
            if field in entry:
                entry[field] = replace(entry[field])
    job['_associationPrepared'] = True
