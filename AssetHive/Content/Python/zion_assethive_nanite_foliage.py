"""USDA / Dynamic Wind 导入。USD 原始几何、UV 和风文件始终保持只读。"""
import hashlib
import json
from pathlib import Path
import re

PARENTS = {
    "grass": "/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_NaniteFoliage_Grass_vt",
    "leaf": "/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_NaniteFoliage_Leaf_VT",
    "trunk": "/Game/MaterialLibrary/Environment/MI_Foliage/MI_Env_NaniteFoliage_Trunk_VT",
}
SLOTS = {"Diffuse": "D", "Normal": "N", "ORM": "ORM", "SubsurfaceColor": "SSC"}


def safe_name(value):
    value = re.sub(r"[^\w]", "_", str(value)).strip("_")
    if not value:
        raise ValueError("资产名称为空")
    return value


def overwrite_targets(policy):
    if isinstance(policy, list):
        if any(x not in {"mesh", "material", "texture"} for x in policy):
            raise ValueError("未知覆盖选项")
        return set(policy)
    return {"mesh"} if policy == "meshOnly" else {"mesh", "material", "texture"}


def variant_paths(folder, variant):
    base = "Env_Foliage_" + safe_name(variant["name"])
    mesh_folder = f"{folder}/{safe_name(variant['name'])}/SKM"
    return base, mesh_folder, f"{mesh_folder}/SK_{base}"


def resource_layout(asset, folder):
    """以实际使用变体决定共享层级；同一纹理跨材质组使用时也只导入一次。"""
    manifest = asset["naniteFoliage"]
    owners = {g["key"]: set() for g in manifest["groups"]}
    variant_names = [safe_name(v["name"]) for v in manifest["variants"]]
    if len({n.casefold() for n in variant_names}) != len(variant_names):
        raise ValueError("变体目录名称重复")
    for variant in manifest["variants"]:
        for material in variant["materials"]:
            owners[material["key"]].add(safe_name(variant["name"]))
    output, textures, file_hashes = {}, {}, {}
    for group in sorted(manifest["groups"], key=lambda g: (g["ownerVariant"], g["name"], g["key"])):
        key = group["key"]
        if not owners[key]:
            raise ValueError(f"材质组没有使用变体：{key}")
        stem = safe_name(asset["name"]) if len(manifest["groups"]) == 1 else safe_name(group["ownerVariant"]) + "_" + safe_name(group["name"])
        destination = folder if len(owners[key]) > 1 else folder + "/" + next(iter(owners[key]))
        output[key] = {"material": f"{destination}/MI/MI_Env_Foliage_{stem}", "textures": {}}
        for slot, file in group["textures"].items():
            if slot not in SLOTS or (slot == "SubsurfaceColor" and group["role"] == "trunk"):
                continue
            if file not in file_hashes:
                with Path(file).open("rb") as source:
                    file_hashes[file] = hashlib.file_digest(source, "sha256").hexdigest()
            identity = (slot, file_hashes[file])
            texture = textures.setdefault(identity, {"name": f"T_Env_Foliage_{stem}_{SLOTS[slot]}", "owners": set(), "uses": []})
            texture["owners"].update(owners[key])
            texture["uses"].append((key, slot))
    for texture in textures.values():
        destination = folder if len(texture["owners"]) > 1 else folder + "/" + next(iter(texture["owners"]))
        for key, slot in texture["uses"]:
            output[key]["textures"][slot] = f"{destination}/Tex/{texture['name']}"
    return output


def geometry_path(folder, variant, obj):
    import unreal
    base, _, _ = variant_paths(folder, variant)
    if isinstance(obj, unreal.PhysicsAsset):
        category = "PhyAssets"
    elif isinstance(obj, unreal.SkeletalMesh):
        category = "SKM" if obj.get_name() == "SK_" + base else "Assembly"
    elif isinstance(obj, unreal.Skeleton):
        category = "Skel"
    elif isinstance(obj, unreal.StaticMesh) and variant["assembly"]:
        category = "Assembly"
    else:
        raise RuntimeError(f"USDA 生成了未支持的资产类型：{obj.get_path_name()} ({obj.get_class().get_name()})")
    return f"{folder}/{safe_name(variant['name'])}/{category}/{obj.get_name()}"


def publish_assets(plans):
    """用原生合并与改名维护资产替换时的外部引用。"""
    import unreal
    if len({path for _, path in plans}) != len(plans):
        raise RuntimeError("USDA 资产目标路径重复")
    for obj, path in plans:
        existing = unreal.load_asset(path)
        if existing and existing.get_class() != obj.get_class():
            raise RuntimeError(f"目标资产类型不一致：{path}")
    for obj, path in plans:
        existing = unreal.load_asset(path)
        if existing and existing != obj:
            if not unreal.EditorAssetLibrary.consolidate_assets(obj, [existing]):
                raise RuntimeError(f"重导资产引用替换失败：{path}")
    renames = [unreal.AssetRenameData(obj, path.rsplit("/", 1)[0], path.rsplit("/", 1)[1]) for obj, path in plans
               if obj.get_path_name().split(".", 1)[0] != path]
    # AssetTools 支持替换合并产生的目标重定向器；EditorAssetLibrary 改名会拒绝该路径。
    if renames and not unreal.AssetToolsHelpers.get_asset_tools().rename_assets(renames):
        raise RuntimeError("无法整理 Nanite Foliage 资产目录")
    for obj, path in plans:
        if obj.get_path_name().split(".", 1)[0] != path:
            raise RuntimeError(f"资产未到达目标目录：{path}")
    return [obj for obj, _ in plans]


def publish_geometry(objects, folder, variant):
    return publish_assets([(obj, geometry_path(folder, variant, obj)) for obj in objects])


def rebuild_physics_asset(mesh, objects, folder, variant):
    """从最终主网格重建物理数据，生成成功后才替换原资产及引用。"""
    import unreal
    base, mesh_folder, _ = variant_paths(folder, variant)
    target = f"{folder}/{safe_name(variant['name'])}/PhyAssets/PHYS_{base}"
    existing = unreal.load_asset(target)
    if existing and not isinstance(existing, unreal.PhysicsAsset):
        raise RuntimeError(f"物理资产目标类型不一致：{target}")
    generated_path = f"{mesh_folder}/{mesh.get_name()}_PhysicsAsset"
    if unreal.EditorAssetLibrary.does_asset_exist(generated_path):
        raise RuntimeError(f"物理资产生成路径已被占用：{generated_path}")
    # 原生接口使用 UE 默认胶囊、骨骼尺寸和约束规则；不沿用 USD 导入中途的结果。
    rebuilt = unreal.SkeletalMeshEditorSubsystem.create_physics_asset(mesh, False, 0)
    if not isinstance(rebuilt, unreal.PhysicsAsset):
        raise RuntimeError(f"物理资产重建失败：{mesh.get_path_name()}")
    if not unreal.SkeletalMeshEditorSubsystem.is_physics_asset_compatible(mesh, rebuilt):
        raise RuntimeError(f"重建后的物理资产与骨架不兼容：{mesh.get_path_name()}")
    updated_objects = [obj for obj in objects if obj != existing]
    publish_assets([(rebuilt, target)])
    # Assembly 主体独立绑定；部件自身的 PhysicsAsset 不替换为整棵树的数据。
    if not unreal.SkeletalMeshEditorSubsystem.assign_physics_asset(mesh, rebuilt):
        raise RuntimeError(f"无法绑定重建的物理资产：{target}")
    return updated_objects + [rebuilt]


def save_asset(asset):
    import unreal
    if not unreal.EditorAssetLibrary.save_loaded_asset(asset, only_if_is_dirty=False):
        raise RuntimeError(f"保存资产失败：{asset.get_path_name()}")


def cleanup_staging(folder):
    """只清理空暂存目录，同时移除内容浏览器仍保留的注册表路径缓存。"""
    import unreal
    staging = folder + "/_Import"
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    if unreal.EditorAssetLibrary.list_assets(staging, recursive=True, include_folder=False):
        raise RuntimeError(f"暂存目录仍有资产，保留以便检查：{staging}")
    paths = registry.get_sub_paths(folder, recurse=True)
    if staging not in paths:
        return
    # 不能仅依赖磁盘是否存在；原生改名会删掉磁盘空目录，但保留浏览器缓存。
    unreal.EditorAssetLibrary.delete_directory(staging)
    if any(path == staging or path.startswith(staging + "/") for path in registry.get_sub_paths(folder, recurse=True)):
        raise RuntimeError(f"无法清除暂存目录缓存：{staging}")


def import_texture(file, destination, name, slot, overwrite):
    import unreal
    existing = unreal.load_asset(f"{destination}/{name}")
    if existing and not isinstance(existing, unreal.Texture2D):
        raise RuntimeError(f"目标不是 Texture2D：{destination}/{name}")
    if existing and not overwrite:
        return existing
    task = unreal.AssetImportTask()
    task.filename = str(file)
    task.destination_path = destination
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    texture = unreal.load_asset(f"{destination}/{name}")
    if not isinstance(texture, unreal.Texture2D):
        raise RuntimeError(f"纹理导入失败：{file}")
    texture.set_editor_property("virtual_texture_streaming", True)
    texture.set_editor_property("srgb", slot in {"Diffuse", "SubsurfaceColor"})
    texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_NORMALMAP if slot == "Normal"
                                else unreal.TextureCompressionSettings.TC_MASKS if slot == "ORM" else unreal.TextureCompressionSettings.TC_DEFAULT)
    # USDA 的法线来自源 USD 纹理，原始方向不额外翻转。
    if slot == "Normal":
        texture.set_editor_property("flip_green_channel", False)
    save_asset(texture)
    return texture


def prepare_materials(asset, folder, force_groups):
    import unreal
    targets = overwrite_targets(asset.get("meshOverwritePolicy", "all"))
    output, updated_materials = {}, []
    layout = resource_layout(asset, folder)
    imported_textures = {}
    for group in asset["naniteFoliage"]["groups"]:
        key, role = group["key"], group["role"]
        parent = unreal.load_asset(PARENTS[role])
        if not isinstance(parent, unreal.MaterialInstanceConstant):
            raise RuntimeError(f"缺少 Nanite Foliage 父材质：{PARENTS[role]}")
        material_path = layout[key]["material"]
        material_folder, name = material_path.rsplit("/", 1)
        material = unreal.load_asset(material_path)
        if material and not isinstance(material, unreal.MaterialInstanceConstant):
            raise RuntimeError(f"目标不是材质实例：{name}")
        force = key in force_groups
        update_material = "material" in targets or (force and not material)
        update_texture = "texture" in targets
        textures = {}
        for slot, file in group["textures"].items():
            if slot not in SLOTS or (slot == "SubsurfaceColor" and role == "trunk"):
                continue
            texture_path = layout[key]["textures"][slot]
            texture_folder, texture_name = texture_path.rsplit("/", 1)
            existing_texture = unreal.load_asset(texture_path)
            create_texture = force and not existing_texture
            if update_texture or create_texture or existing_texture:
                if texture_path not in imported_textures:
                    imported_textures[texture_path] = import_texture(file, texture_folder, texture_name, slot, update_texture or create_texture)
                textures[slot] = imported_textures[texture_path]
            elif update_material:
                raise RuntimeError(f"未选择覆盖纹理且纹理不存在：{texture_name}")
        if update_material:
            parameters = {str(x) for x in unreal.MaterialEditingLibrary.get_texture_parameter_names(parent)}
            for slot in textures:
                if slot + "_VT" not in parameters:
                    raise RuntimeError(f"父材质缺少纹理参数：{slot}_VT")
            if not material:
                material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(name, material_folder, unreal.MaterialInstanceConstant, unreal.MaterialInstanceConstantFactoryNew())
            if not isinstance(material, unreal.MaterialInstanceConstant):
                raise RuntimeError(f"材质实例创建失败：{material_path}")
            # 覆盖材质时，源中已经移除的纹理和旧参数应重新继承父材质默认值。
            unreal.MaterialEditingLibrary.clear_all_material_instance_parameters(material)
            unreal.MaterialEditingLibrary.set_material_instance_parent(material, parent)
            for slot, texture in textures.items():
                parameter = slot + "_VT"
                unreal.MaterialEditingLibrary.set_material_instance_texture_parameter_value(material, parameter, texture)
            unreal.MaterialEditingLibrary.update_material_instance(material)
            save_asset(material)
            updated_materials.append(material)
        output[key] = material or parent
    return output, updated_materials


def prepare_stage(variant, material_paths, destination):
    """派生 USD 仅修改对象名称和 UE 材质引用，不写回原文件、不变换 UV。"""
    from pxr import Usd, UsdGeom, UsdShade, UsdSkel, Sdf
    source = Usd.Stage.Open(variant["file"])
    if not source or not source.GetDefaultPrim():
        raise RuntimeError("USDA 缺少有效 defaultPrim")
    instances = sum(len(UsdGeom.PointInstancer(p).GetProtoIndicesAttr().Get() or []) for p in source.Traverse() if p.IsA(UsdGeom.PointInstancer))
    instances += sum(1 for p in source.Traverse() if p.IsInstance())
    if bool(instances) != bool(variant["assembly"]):
        raise RuntimeError("USDA 实例已变化，请重新导出")
    layer = source.Flatten()
    stage = Usd.Stage.Open(layer)
    original_root = stage.GetDefaultPrim()
    base = "Env_Foliage_" + safe_name(variant["name"])
    # NamespaceEditor 同步更新 Skeleton / material / prototype 关系。
    editor = Usd.NamespaceEditor(stage)
    old_root = original_root.GetPath()
    if not editor.MovePrimAtPath(old_root, Sdf.Path("/" + base)) or not editor.ApplyEdits():
        raise RuntimeError("无法规范化 USD 根节点名称")
    stage.SetDefaultPrim(stage.GetPrimAtPath("/" + base))
    roots = [p.GetPath() for p in stage.Traverse() if p.IsA(UsdSkel.Root)]
    for index, old in enumerate(sorted(roots, key=lambda p: str(p))):
        part_name = base if not variant["assembly"] and len(roots) == 1 else f"{base}_Part_{index + 1:02d}"
        if not editor.MovePrimAtPath(old, old.GetParentPath().AppendChild(part_name)) or not editor.ApplyEdits():
            raise RuntimeError(f"无法规范化 USD 骨骼部件：{old}")
    for prim in list(stage.Traverse()):
        if not prim.IsA(UsdShade.Material):
            continue
        name = prim.GetName()
        if name not in material_paths:
            raise RuntimeError(f"USDA 材质未能一一对应：{name}")
        material = UsdShade.Material(prim)
        shader = UsdShade.Shader.Define(stage, prim.GetPath().AppendChild("AssetHiveUnrealShader"))
        shader.SetSourceAsset(Sdf.AssetPath(material_paths[name]), "unreal")
        material.CreateSurfaceOutput("unreal").ConnectToSource(shader.CreateOutput("out", Sdf.ValueTypeNames.Token))
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not stage.GetRootLayer().Export(str(destination)):
        raise RuntimeError("无法写入派生 USD")
    # UE 的 USD Stage Cache 不能复用上次导出的旧内容。
    cached = Sdf.Layer.Find(str(destination))
    if cached:
        cached.Reload(True)
    return str(destination)


def import_variant(variant, folder, material_paths, derived_dir, preserve_materials=False):
    import unreal
    base, _, primary_path = variant_paths(folder, variant)
    existing = unreal.load_asset(primary_path)
    saved_materials = {}
    if existing and preserve_materials:
        # 包括 Assembly 的所有骨骼部件，按稳定包路径恢复用户现有材质绑定。
        for asset_path in unreal.EditorAssetLibrary.list_assets(f"{folder}/{safe_name(variant['name'])}", recursive=True):
            obj = unreal.load_asset(asset_path)
            if isinstance(obj, unreal.SkeletalMesh):
                saved_materials[obj.get_path_name()] = list(obj.get_editor_property("materials"))
    filename = prepare_stage(variant, material_paths, Path(derived_dir) / f"{base}.usda")
    options = unreal.UsdStageImportOptions()
    for key, value in dict(import_actors=False, import_geometry=True, import_materials=True, import_skeletal_animations=False,
                           import_level_sequences=False, import_groom_assets=False, import_sounds=False, import_sparse_volume_textures=False,
                           use_existing_asset_cache=False, prim_path_folder_structure=False, render_context_to_import="unreal",
                           merge_identical_material_slots=False, share_assets_for_identical_prims=False).items():
        options.set_editor_property(key, value)
    options.set_editor_property("existing_asset_policy", unreal.ReplaceAssetPolicy.REPLACE)
    task = unreal.AssetImportTask()
    task.filename = filename
    staging = f"{folder}/_Import"
    task.destination_path = staging
    task.options = options
    task.factory = unreal.UsdStageAssetImportFactory()
    task.automated = True
    task.replace_existing = True
    task.save = False
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    objects = list(task.get_objects())
    if not any(isinstance(obj, unreal.SkeletalMesh) and obj.get_name() == "SK_" + base for obj in objects):
        raise RuntimeError(f"USDA 导入未生成预期的骨骼网格：SK_{base}")
    for obj in objects:
        if not obj.get_path_name().startswith(staging + "/"):
            raise RuntimeError(f"USDA 返回了暂存目录以外的资产：{obj.get_path_name()}")
    publish_geometry(objects, folder, variant)
    primary = unreal.load_asset(primary_path)
    if not isinstance(primary, unreal.SkeletalMesh):
        raise RuntimeError(f"USDA 导入未生成预期的骨骼网格：{primary_path}")
    for obj in objects:
        if obj.get_path_name() in saved_materials:
            previous = saved_materials[obj.get_path_name()]
            by_name = {str(slot.material_slot_name): slot.material_interface for slot in previous}
            incoming = list(obj.get_editor_property("materials"))
            for index, slot in enumerate(incoming):
                name = str(slot.material_slot_name)
                if name in by_name:
                    slot.material_interface = by_name[name]
                elif index < len(previous):
                    slot.material_interface = previous[index].material_interface
            obj.set_editor_property("materials", incoming)
    return primary, objects


def run_job(job):
    import unreal
    if not hasattr(unreal, "ZionAssetHiveFoliageLibrary"):
        raise RuntimeError("请编译并加载包含 Nanite Foliage 的 AssetHive 插件")
    if not hasattr(unreal, "UsdStageAssetImportFactory"):
        raise RuntimeError("请启用 USD Importer 插件并重启编辑器")
    asset, folder = job["asset"], job["folder"].rstrip("/")
    if not folder.startswith("/Game/") or ".." in folder.split("/"):
        raise ValueError("无效目标路径")
    manifest = asset["naniteFoliage"]
    if manifest.get("version") != 1 or not manifest.get("variants") or not manifest.get("groups"):
        raise ValueError("Nanite Foliage 清单无效")
    targets = overwrite_targets(asset.get("meshOverwritePolicy", "all"))
    missing = [v for v in manifest["variants"] if not unreal.EditorAssetLibrary.does_asset_exist(variant_paths(folder, v)[2])]
    force_groups = {m["key"] for v in missing for m in v["materials"]}
    for v in manifest["variants"]:
        for file in (v["file"], v["windFile"]):
            if not Path(file).is_file():
                raise ValueError(f"源文件不存在：{file}")
    materials, updated_materials = prepare_materials(asset, folder, force_groups)
    derived_dir = Path(unreal.Paths.project_saved_dir()) / "AssetHive" / "NaniteFoliage" / hashlib.sha256(folder.encode()).hexdigest()[:16]
    imported = []
    for variant in manifest["variants"]:
        if variant not in missing and "mesh" not in targets:
            continue
        bindings = {m["name"]: materials[m["key"]].get_path_name() for m in variant["materials"]}
        mesh, objects = import_variant(variant, folder, bindings, derived_dir, preserve_materials=variant not in missing and "material" not in targets)
        error = unreal.ZionAssetHiveFoliageLibrary.apply_wind_data(mesh, variant["windFile"], variant["assembly"])
        if error:
            raise RuntimeError(error)
        objects = rebuild_physics_asset(mesh, objects, folder, variant)
        for obj in objects:
            save_asset(obj)
        imported.append(mesh.get_path_name())
    # 首次使用可能补写用途状态；只补存本次创建/覆盖的 MI，不保存用户未选择覆盖的材质。
    if imported:
        for material in updated_materials:
            save_asset(material)
    cleanup_staging(folder)
    return {"ok": True, "meshes": imported, "materialGroups": len(materials)}


def run_job_file(input_file, output_file):
    try:
        result = run_job(json.loads(Path(input_file).read_text(encoding="utf-8-sig")))
    except Exception as error:
        import unreal
        unreal.log_error(f"Nanite Foliage: {error}")
        result = {"error": str(error)}
    Path(output_file).write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
