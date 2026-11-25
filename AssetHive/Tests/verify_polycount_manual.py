"""Isolated UE regression: routine model updates never write the shared snapshot."""
import hashlib
import stat
from pathlib import Path
import unreal

assert Path(unreal.Paths.get_project_file_path()).stem == 'CharacterLinkTest'
catalog_path = '/Game/Characters/Data/DA_ModelPolycountLevels'
catalog = unreal.load_asset(catalog_path)
assert catalog
filename = Path(unreal.Paths.project_content_dir()) / 'Characters/Data/DA_ModelPolycountLevels.uasset'
before = hashlib.sha256(filename.read_bytes()).hexdigest()
rows = list(catalog.get_editor_property('models'))
assert rows
def snapshot():
    return [tuple(str(row.get_editor_property(key)) for key in
                  ('model', 'triangle_count', 'level', 'family', 'suit_id', 'suit_name', 'suit_part'))
            for row in catalog.get_editor_property('models')]
before_rows = snapshot()
mesh = unreal.load_asset('/Game/Characters/Hero/Asset_PJZ/Male_01/Mesh/M01_Heavy007_Zhou_Manhunt/Mesh/SkeletalMesh/SK_M01_Heavy007_Zhou_Manhunt_Upbody')
assert isinstance(mesh, unreal.SkeletalMesh)
mode = filename.stat().st_mode
try:
    filename.chmod(stat.S_IREAD)
    for identity in ['test:colleague-a:head', 'test:colleague-b:head']:
        assert unreal.AssetHiveModelCatalogLibrary.update_model_level(mesh, identity)
    assert hashlib.sha256(filename.read_bytes()).hexdigest() == before
    assert snapshot() == before_rows
    # The explicit refresh API still supports creating/updating snapshot rows.
    filename.chmod(stat.S_IWRITE | stat.S_IREAD)
    assert unreal.AssetHiveModelCatalogLibrary.update_model_level(
        mesh, 'test:manual-refresh:head', 'test', 'Manual refresh', 'head', True)
    assert any(str(row.get_editor_property('suit_name')) == 'Manual refresh'
               for row in catalog.get_editor_property('models'))
    print('POLYCOUNT_IMPORT_NO_SHARED_WRITE_AND_EXPLICIT_REFRESH_PASS')
finally:
    filename.chmod(mode)
