import runpy
from pathlib import Path
root=Path(__file__).resolve().parent.parent
runpy.run_path(str(root/'experiment-tools'/'PROVA-POLYPAINT-BASE.py'),run_name='__main__')
runpy.run_path(str(root/'VERIFICA-FACESETS.py'),run_name='facesets_hr_loader')
runpy.run_path(str(root/'LOAD-TRANSFER.py'),run_name='detail_transfer_loader')
