"""Load this prototype for one Blender session; no file/preferences edits."""
import sys
import importlib.util
from pathlib import Path

name='sculpt_detail_transfer_v01'
old=sys.modules.get(name)
if old is not None:
    old.unregister()
    for key in list(sys.modules):
        if key==name or key.startswith(name+'.'):
            del sys.modules[key]
path=Path(__file__).parent/'detail_transfer'/'__init__.py'
spec=importlib.util.spec_from_file_location(name,path,submodule_search_locations=[str(path.parent)])
module=importlib.util.module_from_spec(spec)
sys.modules[name]=module
spec.loader.exec_module(module)
module.register()
