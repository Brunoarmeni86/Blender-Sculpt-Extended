Multires Bake CPU - prova Normal / Vector ai bordi UV

Chiudi Blender e usa la Developer PowerShell di Visual Studio 2022.
Estrai entrambi i file dello ZIP nella stessa cartella, poi esegui:

  & "C:\Users\olias\Downloads\Bake-UV-Coverage\APPLY-BAKE-UV-COVERAGE.ps1"

Adatta soltanto il percorso dello script alla cartella estratta.
La patch parte dal multires_bake.cc stabile 83d81b2d e include i tre guard
corretti nel precedente commit baking 9e2d5da7. Modifica un solo file.
Il controllo git apply --check precede backup, modifica e compilazione.
La build esistente viene compilata e installata incrementalmente.

Per togliere questa patch (stessa cartella e stessa patch):

  & "C:\Users\olias\Downloads\Bake-UV-Coverage\APPLY-BAKE-UV-COVERAGE.ps1" -Revert

Con la nuova build, confronto allo stesso sampling 4x4:
chiudi Blender, nella PowerShell imposta
  $env:BLENDER_MULTIRES_BAKE_UV_COVERAGE = "0"
e avvia un nuovo processo di Blender. Il parametro e' letto una sola volta
per processo. Imposta "1" per la nuova copertura.

Test: immagine Float, inizialmente Margin 0; Normal e Vector Displacement
Object/Tangent, feature vicino ai bordi, isole sottili, UV specchiate e UDIM.
Ripeti con worker diversi e confronta i float prima e dopo il padding.
Poi confronta la mesh con mappe riapplicate al vero sculpt high-res.

Controllo nativo automatico (processi separati, solo scene factory):
  & "C:\Users\olias\Downloads\Bake-UV-Coverage\TEST-BAKE-UV-COVERAGE.ps1"
Verifica un pixel UV parziale per Normal e Vector, Object/Tangent e Low
Resolution Mesh ON/OFF. Genera native-coverage-1.json e native-coverage-0.json.
Il controllo misura la copertura; non misura la fedelta' di uno sculpt reale.

Costo aggiuntivo: owner temporanei 8 byte/pixel, 128 MiB per tile 4K,
liberati dopo la rasterizzazione del tile. Tempi e memoria da verificare.
Il displacement scalare mantiene il sampler attuale. Il TBN HQ non viene
modificato in questa prova. Nessun codice GPU o Edit/Undo incluso.

Qui sono passati test C++ isolati e sanitizer. La build Blender/Windows,
lo script PowerShell e i bake reali non sono stati eseguiti in questo ambiente.
