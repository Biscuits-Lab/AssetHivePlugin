import bpy
import os
from pathlib import Path
root=Path(os.environ['ASSETHIVE_BLOCKOUT_FIXTURE_DIR'])
root.mkdir(parents=True, exist_ok=True)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
parts=[]
for i,(name,color) in enumerate([('lambert1',(1,0,0,1)),('blinn2',(0,1,0,1)),('DefaultMaterial',(1,1,1,1))]):
    bpy.ops.mesh.primitive_cube_add(size=1, location=(i*2,0,0))
    obj=bpy.context.object
    mat=bpy.data.materials.new(name)
    mat.diffuse_color=color
    mat.use_nodes=True
    mat.node_tree.nodes.get('Principled BSDF').inputs['Base Color'].default_value=color
    obj.data.materials.append(mat)
    parts.append(obj)
bpy.ops.object.select_all(action='DESELECT')
for obj in parts: obj.select_set(True)
bpy.context.view_layer.objects.active=parts[0]
bpy.ops.object.join()
mesh=bpy.context.object
mesh.name='BlockoutFixture'
bpy.ops.export_scene.fbx(filepath=str(root/'SM_BlockoutFixture_01.fbx'),use_selection=True,object_types={'MESH'},add_leaf_bones=False,bake_anim=False)
bpy.ops.object.armature_add(location=(0,0,0))
rig=bpy.context.object
rig.name='Armature'
rig.data.bones[0].name='root'
mod=mesh.modifiers.new('Armature','ARMATURE');mod.object=rig
mesh.vertex_groups.new(name='root').add(list(range(len(mesh.data.vertices))),1,'REPLACE')
bpy.ops.object.select_all(action='DESELECT')
mesh.select_set(True);rig.select_set(True)
bpy.ops.export_scene.fbx(filepath=str(root/'SK_BlockoutFixture_01.fbx'),use_selection=True,object_types={'MESH','ARMATURE'},add_leaf_bones=False,bake_anim=False)
