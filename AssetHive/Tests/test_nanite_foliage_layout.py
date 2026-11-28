"""无需编辑器：验证材质组与纹理各自的跨变体共享归属。"""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("nanite_foliage", Path(__file__).parents[1] / "Content/Python/zion_assethive_nanite_foliage.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class NaniteFoliageLayoutTests(unittest.TestCase):
    def test_shared_atlas_and_variant_specific_materials(self):
        with tempfile.TemporaryDirectory() as directory:
            paths = {}
            for name, data in {"d1": b"diffuse1", "d2": b"diffuse2", "n1": b"normal", "n2": b"normal"}.items():
                file = Path(directory) / name
                file.write_bytes(data)
                paths[name] = str(file)
            asset = {"name": "Katsura", "naniteFoliage": {
                "variants": [
                    {"name": "Katsura_02", "materials": [{"key": "shared"}, {"key": "leaf2"}]},
                    {"name": "Katsura_06", "materials": [{"key": "shared"}, {"key": "leaf6"}]},
                ],
                "groups": [
                    {"key": "shared", "ownerVariant": "Katsura_02", "name": "Trunk", "role": "trunk", "textures": {"Diffuse": paths["d1"]}},
                    {"key": "leaf2", "ownerVariant": "Katsura_02", "name": "Leaf", "role": "leaf", "textures": {"Diffuse": paths["d2"], "Normal": paths["n1"]}},
                    {"key": "leaf6", "ownerVariant": "Katsura_06", "name": "Leaf", "role": "leaf", "textures": {"Normal": paths["n2"]}},
                ],
            }}
            layout = module.resource_layout(asset, "/Game/NaniteFoliage")
            self.assertTrue(layout["shared"]["material"].startswith("/Game/NaniteFoliage/MI/"))
            self.assertTrue(layout["shared"]["textures"]["Diffuse"].startswith("/Game/NaniteFoliage/Tex/"))
            self.assertTrue(layout["leaf2"]["material"].startswith("/Game/NaniteFoliage/Katsura_02/MI/"))
            self.assertTrue(layout["leaf2"]["textures"]["Diffuse"].startswith("/Game/NaniteFoliage/Katsura_02/Tex/"))
            self.assertTrue(layout["leaf6"]["material"].startswith("/Game/NaniteFoliage/Katsura_06/MI/"))
            self.assertEqual(layout["leaf2"]["textures"]["Normal"], layout["leaf6"]["textures"]["Normal"])
            self.assertTrue(layout["leaf2"]["textures"]["Normal"].startswith("/Game/NaniteFoliage/Tex/"))
            asset["naniteFoliage"]["groups"].reverse()
            self.assertEqual(layout, module.resource_layout(asset, "/Game/NaniteFoliage"))

    def test_conflicting_variant_names_are_rejected(self):
        asset = {"naniteFoliage": {"groups": [], "variants": [{"name": "tree 01"}, {"name": "tree_01"}]}}
        with self.assertRaisesRegex(ValueError, "变体目录名称重复"):
            module.resource_layout(asset, "/Game/NaniteFoliage")

    def test_empty_cached_staging_is_removed_without_disk_directory(self):
        registry = Mock()
        registry.get_sub_paths.side_effect = [["/Game/NaniteFoliage/_Import", "/Game/NaniteFoliage/_Import/Variant"], []]
        library = Mock()
        library.list_assets.return_value = []
        unreal = SimpleNamespace(EditorAssetLibrary=library, AssetRegistryHelpers=SimpleNamespace(get_asset_registry=lambda: registry))
        with patch.dict("sys.modules", {"unreal": unreal}):
            module.cleanup_staging("/Game/NaniteFoliage")
        library.delete_directory.assert_called_once_with("/Game/NaniteFoliage/_Import")

    def test_staging_with_assets_is_never_deleted(self):
        library = Mock()
        library.list_assets.return_value = ["/Game/NaniteFoliage/_Import/UnfinishedMesh"]
        unreal = SimpleNamespace(EditorAssetLibrary=library, AssetRegistryHelpers=SimpleNamespace(get_asset_registry=Mock()))
        with patch.dict("sys.modules", {"unreal": unreal}), self.assertRaisesRegex(RuntimeError, "暂存目录仍有资产"):
            module.cleanup_staging("/Game/NaniteFoliage")
        library.delete_directory.assert_not_called()

    def test_failed_physics_generation_does_not_replace_existing_asset(self):
        class PhysicsAsset:
            pass
        existing = PhysicsAsset()
        native = Mock()
        native.create_physics_asset.return_value = None
        library = Mock()
        library.does_asset_exist.return_value = False
        unreal = SimpleNamespace(PhysicsAsset=PhysicsAsset, load_asset=lambda _: existing,
                                 EditorAssetLibrary=library, SkeletalMeshEditorSubsystem=native)
        mesh = Mock()
        mesh.get_name.return_value = "SK_Env_Foliage_Grass_01"
        with patch.dict("sys.modules", {"unreal": unreal}), patch.object(module, "publish_assets") as publish:
            with self.assertRaisesRegex(RuntimeError, "物理资产重建失败"):
                module.rebuild_physics_asset(mesh, [mesh, existing], "/Game/NaniteFoliage", {"name": "Grass_01"})
        publish.assert_not_called()
        native.assign_physics_asset.assert_not_called()

    def test_assembly_rebuild_preserves_part_physics(self):
        class PhysicsAsset:
            pass
        part, rebuilt = PhysicsAsset(), PhysicsAsset()
        native = Mock()
        native.create_physics_asset.return_value = rebuilt
        native.is_physics_asset_compatible.return_value = True
        native.assign_physics_asset.return_value = True
        library = Mock()
        library.does_asset_exist.return_value = False
        unreal = SimpleNamespace(PhysicsAsset=PhysicsAsset, load_asset=lambda _: None,
                                 EditorAssetLibrary=library, SkeletalMeshEditorSubsystem=native)
        mesh = Mock()
        mesh.get_name.return_value = "SK_Env_Foliage_Tree_01"
        with patch.dict("sys.modules", {"unreal": unreal}), patch.object(module, "publish_assets") as publish:
            objects = module.rebuild_physics_asset(mesh, [mesh, part], "/Game/NaniteFoliage", {"name": "Tree_01"})
        self.assertEqual(objects, [mesh, part, rebuilt])
        publish.assert_called_once_with([(rebuilt, "/Game/NaniteFoliage/Tree_01/PhyAssets/PHYS_Env_Foliage_Tree_01")])
        native.assign_physics_asset.assert_called_once_with(mesh, rebuilt)

    def test_material_overwrite_removes_obsolete_texture_overrides(self):
        class Material:
            def __init__(self):
                self.parameters = {"ORM_VT": "old ORM", "SubsurfaceColor_VT": "old leaf texture"}
        parent, material = Material(), Material()
        editing = Mock()
        editing.get_texture_parameter_names.return_value = ["Diffuse_VT", "Normal_VT", "ORM_VT"]
        editing.clear_all_material_instance_parameters.side_effect = lambda value: value.parameters.clear()
        editing.set_material_instance_texture_parameter_value.side_effect = lambda value, name, texture: value.parameters.update({name: texture})
        unreal = SimpleNamespace(MaterialInstanceConstant=Material, MaterialEditingLibrary=editing,
                                 load_asset=lambda value: parent if value in module.PARENTS.values() else material)
        asset = {"meshOverwritePolicy": ["material"], "naniteFoliage": {"groups": [
            {"key": "g", "role": "trunk", "textures": {"Diffuse": "d.png", "Normal": "n.png"}},
        ]}}
        layout = {"g": {"material": "/Game/Test/MI/Tree", "textures": {"Diffuse": "/Game/Test/Tex/D", "Normal": "/Game/Test/Tex/N"}}}
        with patch.dict("sys.modules", {"unreal": unreal}), patch.object(module, "resource_layout", return_value=layout), \
                patch.object(module, "import_texture", side_effect=lambda file, *_: file), patch.object(module, "save_asset"):
            bindings, updated = module.prepare_materials(asset, "/Game/Test", set())
        self.assertEqual(bindings, {"g": material})
        self.assertEqual(updated, [material])
        self.assertEqual(material.parameters, {"Diffuse_VT": "d.png", "Normal_VT": "n.png"})

    def test_selective_reimport_never_saves_unselected_material(self):
        class Material:
            def get_path_name(self):
                return "/Game/Test/MI/Tree.Tree"
        parent, material = Material(), Material()
        editing = Mock()
        editing.get_texture_parameter_names.return_value = []
        mesh, physics = Mock(), Mock()
        mesh.get_path_name.return_value = "/Game/Test/Tree_01/SKM/SK_Env_Foliage_Tree_01"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "source.usda"
            source.write_text("fixture", encoding="utf-8")
            variant = {"name": "Tree_01", "file": str(source), "windFile": str(source), "assembly": True,
                       "materials": [{"name": "Mesh_Mat", "key": "g"}]}
            asset = {"naniteFoliage": {"version": 1, "variants": [variant], "groups": [{"key": "g", "role": "trunk", "textures": {}}]}}
            unreal = SimpleNamespace(MaterialInstanceConstant=Material, MaterialEditingLibrary=editing,
                load_asset=lambda value: parent if value in module.PARENTS.values() else material,
                EditorAssetLibrary=SimpleNamespace(does_asset_exist=lambda _: True),
                Paths=SimpleNamespace(project_saved_dir=lambda: directory), UsdStageAssetImportFactory=object,
                ZionAssetHiveFoliageLibrary=SimpleNamespace(apply_wind_data=lambda *_: ""))
            for policy in [[], ["texture"], ["mesh"], ["mesh", "texture"], ["material"]]:
                with self.subTest(policy=policy), patch.dict("sys.modules", {"unreal": unreal}), \
                        patch.object(module, "resource_layout", return_value={"g": {"material": "/Game/Test/MI/Tree", "textures": {}}}), \
                        patch.object(module, "import_variant", return_value=(mesh, [mesh])) as import_mesh, \
                        patch.object(module, "rebuild_physics_asset", return_value=[mesh, physics]) as rebuild, \
                        patch.object(module, "save_asset") as save, patch.object(module, "cleanup_staging"):
                    asset["meshOverwritePolicy"] = policy
                    module.run_job({"asset": asset, "folder": "/Game/Test"})
                    saved = [call.args[0] for call in save.call_args_list]
                    self.assertEqual(material in saved, "material" in policy)
                    self.assertEqual(import_mesh.call_count, int("mesh" in policy))
                    self.assertEqual(rebuild.call_count, int("mesh" in policy))


if __name__ == "__main__":
    unittest.main()
