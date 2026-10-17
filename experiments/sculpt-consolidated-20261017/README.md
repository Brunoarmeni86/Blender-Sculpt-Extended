# Avvio consolidato e test

Questo runtime conserva il profilo sperimentale e aggiunge Face Sets HR/Extract e
Detail Transfer. Non salva preferenze o installa addon permanentemente.

Dalla cartella del repository, in PowerShell:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\experiments\sculpt-consolidated-20261017\AVVIA.ps1" -BlenderExe "C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe"
```

Passare `-SourceRoot` se i sorgenti non sono in `C:\BlenderDev\blender`;
`-BlendFile` per aprire un file. Richiede un eseguibile costruito con i sorgenti
consolidati, non una vecchia build ufficiale. Il launcher non compila e non
copia automaticamente i sorgenti nella macchina Windows.

Ogni fixture ha `tests/<nome>/VERIFICA.ps1` e `TEST-IN-BLENDER.py`:

- `topology`: binding, Edit, ID non canonici, Undo, subdivision, duplicate/reopen.
- `extraction`: Mask/Face Set Extract e limiti.
- `merge`: fusioni, gruppi e Bake.
- `safety`: Apply Scale e protezione Join.
- `detail-transfer`: preview, import e Undo/Redo.

Esempio, chiudendo prima Blender e aspettando il termine della build:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File ".\experiments\sculpt-consolidated-20261017\tests\safety\VERIFICA.ps1" -BlenderExe "C:\BlenderDev\build_windows_x64_shareable\bin\Release\blender.exe"
```

Eseguire una fixture alla volta. Creano una scena factory, JSON, log e .blend di
esempio nella propria cartella. Non aprono i file di lavoro. Non sono benchmark.
I test sono ripubblicati per ripetibilita'; non e' stata eseguita qui una nuova
build completa del commit consolidato.

[Esiti e limiti](../../doc/sculpt_extended/checkpoints/2026-10-17/README.md).
