import bpy
import runpy
from pathlib import Path
installed = Path(bpy.app.binary_path).parent/'SculptExtended'/'experiment-tools'/'PROVA-POLYPAINT.py'
if not installed.is_file():
    raise RuntimeError('Kit SculptExtended di avvio non trovato')
runpy.run_path(str(installed), run_name='__main__')
runpy.run_path(str(Path(__file__).parent/'DRAW-TRANSPORT-UI.py'),run_name='draw_transport_ui')
