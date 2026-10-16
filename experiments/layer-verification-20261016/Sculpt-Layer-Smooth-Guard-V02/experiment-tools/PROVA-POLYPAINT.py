"""Session-only verification UI, then the installed complete startup kit."""
import bpy
import runpy
from pathlib import Path
installed = Path(bpy.app.binary_path).parent / 'SculptExtended' / 'experiment-tools' / 'PROVA-POLYPAINT.py'
if not installed.is_file():
    raise RuntimeError('Installed complete SculptExtended startup kit missing')
runpy.run_path(str(installed), run_name='__main__')
runpy.run_path(str(Path(__file__).parent.parent / 'VERIFICA-SMOOTH.py'), run_name='smooth_layer_verification')
