import unreal, runpy
from pathlib import Path
bo=runpy.run_path(str(Path(__file__).resolve().parents[1] / 'Content/Python/assethive_blockout.py'))
master=bo['parent']()
for rgb in ([1,0,0],[0.25,0.5,0.75],[1,1,1]):
 obj=unreal.new_object(unreal.MaterialInstanceConstant)
 bo['apply'](obj,'/Transient/Unused',{'color':rgb})
 actual=bo['values'](obj)
 assert actual['parent']==master.get_path_name(), actual
 assert all(abs(a-b)<0.00001 for a,b in zip(rgb,actual['color'])), actual
 print('BLOCKOUT_INSTANCE_PASS',actual)
