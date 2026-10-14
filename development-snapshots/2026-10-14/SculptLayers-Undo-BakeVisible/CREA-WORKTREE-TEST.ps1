param(
  [string]$Repository = "C:\BlenderDev\blender",
  [string]$TestSource = "C:\BlenderDev\Blender-UndoBake-20261014"
)

$ErrorActionPreference = "Stop"
$branch = "fix/first-layer-undo-visible-base-bake-20261014"
$remoteRef = "refs/remotes/origin/$branch"

if (!(Test-Path $Repository)) { throw "Repository non trovato: $Repository" }
if (Test-Path $TestSource) { throw "Cartella di prova già esistente: $TestSource. Non sovrascrivo." }

& git -C $Repository rev-parse --is-inside-work-tree
if ($LASTEXITCODE -ne 0) { throw "La cartella indicata non è una repository Git." }

Write-Host "Recupero il ramo sperimentale. Non modifico la tua working tree corrente."
& git -C $Repository fetch origin "+refs/heads/${branch}:${remoteRef}"
if ($LASTEXITCODE -ne 0) { throw "Fetch fallito." }

Write-Host "Creo una worktree di test separata:"
& git -C $Repository worktree add --detach $TestSource $remoteRef
if ($LASTEXITCODE -ne 0) { throw "Creazione worktree fallita." }

Write-Host ""
Write-Host "Sorgenti di prova pronti in $TestSource"
Write-Host "La baseline stabile e la build corrente sono rimaste inalterate."
Write-Host "Per compilare, configura una nuova build CMake riferita a questi sorgenti."
