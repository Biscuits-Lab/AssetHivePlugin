import bpy, os
from pathlib import Path
out=Path(os.environ['ASSETHIVE_SKIN_TEST_ROOT']).resolve()/'skin-transfer-fixtures';out.mkdir(parents=True,exist_ok=True)
for name,segments,offset in [('SK_Base',24,0),('SM_LOD1',12,0),('SM_LOD1_Updated',16,0),('SM_WrongPart',12,10)]:
 bpy.ops.object.select_all(action='SELECT');bpy.ops.object.delete(use_global=False)
 for mat in list(bpy.data.materials): bpy.data.materials.remove(mat)
 bpy.ops.mesh.primitive_uv_sphere_add(segments=segments,ring_count=segments//2,radius=1)
 mesh=bpy.context.object;mesh.name='SkinTransfer';mesh.data.materials.append(bpy.data.materials.new('Body'))
 mesh.location.x=offset
 if name.startswith('SK'):
  bpy.ops.object.armature_add();arm=bpy.context.object;arm.name='Rig'
  bpy.ops.object.mode_set(mode='EDIT');root=arm.data.edit_bones[0];root.name='root';root.head=(0,0,-1);root.tail=(0,0,0)
  tip=arm.data.edit_bones.new('tip');tip.head=(0,0,0);tip.tail=(0,0,1);tip.parent=root
  bpy.ops.object.mode_set(mode='OBJECT')
  groups=[mesh.vertex_groups.new(name=n) for n in ('root','tip')]
  for v in mesh.data.vertices:
   w=(v.co.z+1)/2
   if w<1:groups[0].add([v.index],1-w,'REPLACE')
   if w>0:groups[1].add([v.index],w,'REPLACE')
  mod=mesh.modifiers.new('Armature','ARMATURE');mod.object=arm;mesh.parent=arm
 bpy.ops.object.select_all(action='SELECT')
 bpy.ops.export_scene.fbx(filepath=str(out/(name+'.fbx')),use_selection=True,add_leaf_bones=False,bake_anim=False,object_types={'MESH','ARMATURE'})
