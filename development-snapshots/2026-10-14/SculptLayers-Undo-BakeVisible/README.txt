SCULPT LAYERS – PRIMO UNDO + BAKE LAYER VISIBILI
14 ottobre 2026

Ramo GitHub:
fix/first-layer-undo-visible-base-bake-20261014

Commit finale: 3f94fe3642f58e6060c88fd7da2848729917e7a9
Base stabile: 83d81b2dfc2af8ba7763d33fce212dbaa9b5dc49

Contenuto:
- CREA-WORKTREE-TEST.ps1
  Recupera il ramo GitHub creando una CARTELLA DI TEST separata.
  Non modifica main, la working tree esistente o i build correnti.
- test_visible_bake_v2.cpp
  Harness C++ isolato del trasferimento displacement visibili -> base.

UTILIZZO WINDOWS:
1. Aprire PowerShell di Visual Studio 2022.
2. Eseguire, dalla cartella del kit:
   .\CREA-WORKTREE-TEST.ps1 -Repository "C:\BlenderDev\blender"
3. Configurare una nuova build CMake con sorgenti in:
   C:\BlenderDev\Blender-UndoBake-20261014
4. Testare su copia del file .blend di riproduzione.

ATTENZIONE:
La prova C++ isolata è passata su Linux, ma non verifica il runtime reale
di Blender: la compilazione Windows, Undo/Redo e il salvataggio/riapertura
DEVONO essere ancora verificati prima di un merge.

Test particolarmente importante:
a) Sculpt Level 2, Total Level 5, con 7 layer già esistenti.
b) Selezionare un layer diverso, scolpire, Undo, Redo.
c) Verificare che la base nativa NON cambi per un Undo della pennellata.
d) Fare Bake con layer visibili/nascosti e Strength negativa.
e) Verificare che i layer nascosti restino invariati.
f) Ripetere Undo/Redo del bake e save/reopen.

Limitazione attuale:
Il bake RIFIUTA volontariamente un layer visibile con vere deformazioni
L0 del cage (serve una procedura di reshape distinta).

Per eseguire l'harness C++ su Windows:
  cl /std:c++17 /EHsc test_visible_bake_v2.cpp
  test_visible_bake_v2.exe

Repository:
https://github.com/Brunoarmeni86/Blender-Sculpt-Extended/tree/fix/first-layer-undo-visible-base-bake-20261014
