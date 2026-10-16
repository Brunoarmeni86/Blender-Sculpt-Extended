param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$PublishRepo = 'C:\BlenderDev\Blender-Sculpt-Extended-publish',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable'
)
$ErrorActionPreference = 'Stop'
$branch = 'checkpoint/sculpt-extended-20261015'
$baseCommit = '72dae3ab28b5abafc0fe5156f9a47e484aaab26c'
$snapshot = 'experiments/sculpt-extended-20261016'
function Invoke-Git([string[]]$GitArgs) {
    $result = & git -c submodule.recurse=false -c diff.ignoreSubmodules=all -C $PublishRepo @GitArgs
    if ($LASTEXITCODE -ne 0) { throw ('Git non riuscito: ' + ($GitArgs -join ' ')) }
    return $result
}
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di creare il checkpoint.' }
# Orphaned reusable MSBuild workers cannot receive new jobs from their old parent.
# Ignore only those workers; active parents or compiler/linker processes still block.
$processes = @(Get-CimInstance Win32_Process)
$busy = @($processes | Where-Object {
    $p = $_
    $buildProcess = $p.Name -in @('cmake.exe','MSBuild.exe','cl.exe','link.exe','ninja.exe','nvcc.exe')
    $orphanWorker = $p.Name -eq 'MSBuild.exe' -and
        $p.CommandLine -match '/nodemode:1' -and $p.CommandLine -match '/nodeReuse:true' -and
        !($processes | Where-Object { $_.ProcessId -eq $p.ParentProcessId })
    $buildProcess -and !$orphanWorker
})
if ($busy.Count) { throw 'Compilazione ancora attiva. Attendi prima del checkpoint.' }
if (!(Test-Path -LiteralPath (Join-Path $PublishRepo '.git'))) { throw "Repository Git non trovato: $PublishRepo. Non viene creato un nuovo clone." }
$top = ([string](Invoke-Git @('rev-parse','--show-toplevel'))).Replace('/', '\').TrimEnd('\')
if ($top -ine $PublishRepo.Replace('/', '\').TrimEnd('\')) { throw 'Percorso repository non corrispondente.' }
$currentBranch = [string](Invoke-Git @('branch','--show-current'))
if ($currentBranch -cne $branch) { throw "Branch corrente: $currentBranch. Atteso: $branch. Nessuna copia effettuata." }
$remote = [string](Invoke-Git @('remote','get-url','origin'))
if ($remote -notmatch '(?i)github\.com[:/]Brunoarmeni86/Blender-Sculpt-Extended(?:\.git)?/?$') { throw 'Origin punta a un repository diverso. Nessuna copia effettuata.' }
$staged = @(Invoke-Git @('diff','--cached','--name-only','--ignore-submodules=all'))
if ($staged.Count) { throw 'Esistono modifiche gia in staging. Nessuna copia effettuata; invia git diff --cached --stat.' }
$userName = [string](Invoke-Git @('config','user.name'))
$userEmail = [string](Invoke-Git @('config','user.email'))
if (!$userName.Trim() -or !$userEmail.Trim()) { throw 'Identita Git mancante nel repository. Nessuna copia effettuata.' }
Invoke-Git @('merge-base','--is-ancestor',$baseCommit,'HEAD') | Out-Null
Invoke-Git @('fetch','--no-recurse-submodules','origin',("refs/heads/$branch" + ':refs/remotes/origin/' + $branch)) | Out-Host
# Allow a retry after a local commit whose push failed, but never overwrite remote history.
Invoke-Git @('merge-base','--is-ancestor',("refs/remotes/origin/$branch"),'HEAD') | Out-Null
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$homeEntry = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'
if (!$homeEntry -or $homeEntry.Matches[0].Groups[1].Value.Replace('/', '\').TrimEnd('\') -ine $SourceRoot.Replace('/', '\').TrimEnd('\')) {
    throw 'La build usa sorgenti diversi. Nessuna copia effettuata.'
}
$native = @(
    'source/blender/blenkernel/intern/multires.cc',
    'source/blender/blenkernel/intern/multires_weight_multi_cache_v01.hh',
    'source/blender/blenkernel/intern/paint.cc',
    'source/blender/blenkernel/intern/subdiv_ccg.cc',
    'source/blender/blenkernel/intern/pbvh.cc',
    'source/blender/blenkernel/intern/multires_pbvh_group_probe_v01.hh',
    'source/blender/blenkernel/BKE_sculpt_cold_start_profile.hh',
    'source/blender/modifiers/intern/MOD_multires.cc',
    'source/blender/draw/intern/draw_sculpt.cc',
    'source/blender/draw/intern/draw_sculpt.hh',
    'source/blender/draw/intern/multires_overlay_neutral_v01.hh',
    'source/blender/draw/engines/overlay/overlay_sculpt.hh',
    'source/blender/editors/sculpt_paint/mesh/sculpt.cc',
    'source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc',
    'source/blender/editors/include/ED_sculpt.hh',
    'source/blender/editors/util/ed_util.cc',
    'source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc',
    'source/blender/editors/sculpt_paint/mesh/brushes/smooth.cc'
)
$kitRoot = Join-Path $BuildDir 'bin\Release\SculptExtended'
$kit = @(
    'AVVIA-ESPERIMENTO.ps1',
    'LOAD-SIDEBAR.py',
    'addon/__init__.py',
    'experiment-tools/AVVIA-ESPERIMENTO.ps1',
    'experiment-tools/LAYER-DIAGNOSTICS.py',
    'experiment-tools/POLYPAINT-BAKE.py',
    'experiment-tools/PROVA-POLYPAINT.py'
)
$items = @()
foreach ($relative in $native) { $items += [pscustomobject]@{ Source = (Join-Path $SourceRoot $relative); Target = $relative } }
foreach ($relative in $kit) { $items += [pscustomobject]@{ Source = (Join-Path $kitRoot $relative); Target = ($snapshot + '/' + $relative) } }
foreach ($name in @('AVVIA-COMPLETO.ps1','AGGIORNA-GITHUB.ps1','LEGGIMI.md')) {
    $items += [pscustomobject]@{ Source = (Join-Path $PSScriptRoot $name); Target = ($snapshot + '/' + $name) }
}
foreach ($item in $items) {
    if (!(Test-Path -LiteralPath $item.Source -PathType Leaf)) { throw "File richiesto mancante: $($item.Source). Nessuna copia effettuata." }
}
$markers = @{
    'source/blender/editors/sculpt_paint/mesh/sculpt_ops.cc' = 'MULTIRES_STARTUP_REUSE_V02'
    'source/blender/editors/include/ED_sculpt.hh' = 'object_sculpt_mode_restore_reuse_ready'
    'source/blender/editors/util/ed_util.cc' = 'MULTIRES_STARTUP_REUSE_V02'
    'source/blender/blenkernel/intern/subdiv_ccg.cc' = 'MULTIRES_CCG_FULL_BATCH_V01'
    'source/blender/blenkernel/intern/multires.cc' = 'LAYER_WEIGHT_MULTI_CACHE_V01'
    'source/blender/draw/intern/draw_sculpt.cc' = 'MULTIRES_OVERLAY_NEUTRAL_V01'
    'source/blender/editors/sculpt_paint/mesh/sculpt_undo.cc' = 'OPEN_FILE_UNDO_ANCHOR_V01'
    'source/blender/editors/sculpt_paint/mesh/brushes/smooth.cc' = 'LAYER_DELTA_SMOOTH_GUARD_V02'
}
foreach ($relative in $markers.Keys) {
    if (![IO.File]::ReadAllText((Join-Path $SourceRoot $relative)).Contains($markers[$relative])) {
        throw "Correzione attesa non trovata: $relative. Nessuna copia effettuata."
    }
}
$exe = Join-Path $BuildDir 'bin\Release\blender.exe'
if (!(Test-Path -LiteralPath $exe)) { throw 'Blender compilato non trovato.' }
$files = @($items | ForEach-Object { $_.Target })
$manifestPath = $snapshot + '/CHECKPOINT-FILES.json'
$files += $manifestPath
$pending = @(Invoke-Git (@('diff','--name-only','--ignore-submodules=all','--') + $files))
if ($pending.Count) { throw ('Esistono modifiche locali nelle destinazioni del repository di pubblicazione: ' + ($pending -join ', ') + '. Nessuna copia effettuata.') }
# Refuse to overwrite untracked or previously edited destinations with different data.
foreach ($item in $items) {
    $target = Join-Path $PublishRepo $item.Target
    $tracked = @(Invoke-Git @('ls-files','--',$item.Target))
    if (!$tracked.Count -and (Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath $item.Source).Hash) {
        throw "Destinazione non tracciata gia presente e diversa: $($item.Target). Nessuna copia effettuata."
    }
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = Join-Path $PSScriptRoot ('backup-publish-' + $stamp)
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($item in $items) {
    $target = Join-Path $PublishRepo $item.Target
    if (Test-Path -LiteralPath $target) {
        $saved = Join-Path $backup $item.Target
        New-Item -ItemType Directory -Force -Path (Split-Path $saved) | Out-Null
        Copy-Item -LiteralPath $target -Destination $saved
    }
}
foreach ($item in $items) {
    $target = Join-Path $PublishRepo $item.Target
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    if ([IO.Path]::GetFullPath($item.Source) -ine [IO.Path]::GetFullPath($target)) {
        Copy-Item -LiteralPath $item.Source -Destination $target
    }
    if ((Get-FileHash -LiteralPath $item.Source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) { throw "Verifica copia fallita: $($item.Target). Nessun commit effettuato." }
}
$manifest = [ordered]@{
    Timestamp = (Get-Date).ToString('o')
    BaseCheckpoint = $baseCommit
    Branch = $branch
    ObservedExecutableSHA256 = (Get-FileHash -LiteralPath $exe).Hash
    Profile = 'weight-cache-1024-overlay-neutral-open-file-undo-startup-reuse-v02-full-ccg-batch-v01-smooth-guard-v02-pbvh-factor1'
    Files = @($items | ForEach-Object { [ordered]@{ Path = $_.Target; SHA256 = (Get-FileHash -LiteralPath (Join-Path $PublishRepo $_.Target)).Hash } })
}
$writeManifest = $true
$manifestFile = Join-Path $PublishRepo $manifestPath
if (Test-Path -LiteralPath $manifestFile) {
    $previous = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($manifestFile))
    $same = $previous.BaseCheckpoint -ceq $manifest.BaseCheckpoint -and
            $previous.Branch -ceq $manifest.Branch -and
            $previous.Profile -ceq $manifest.Profile -and
            $previous.ObservedExecutableSHA256 -ceq $manifest.ObservedExecutableSHA256 -and
            $previous.Files.Count -eq $manifest.Files.Count
    if ($same) {
        for ($i = 0; $i -lt $manifest.Files.Count; $i++) {
            if ($previous.Files[$i].Path -cne $manifest.Files[$i].Path -or
                $previous.Files[$i].SHA256 -cne $manifest.Files[$i].SHA256) { $same = $false; break }
        }
    }
    $writeManifest = !$same
}
if ($writeManifest) {
    [IO.File]::WriteAllText($manifestFile, ($manifest | ConvertTo-Json -Depth 5), (New-Object Text.UTF8Encoding($false)))
}
Invoke-Git (@('add','--') + $files) | Out-Host
$actualStaged = @(Invoke-Git @('diff','--cached','--name-only','--ignore-submodules=all'))
$unexpected = @($actualStaged | Where-Object { $files -cnotcontains $_ })
if ($unexpected.Count) { throw ('File in staging non previsti: ' + ($unexpected -join ', ') + '. Nessun commit effettuato.') }
Invoke-Git @('diff','--cached','--stat','--ignore-submodules=all') | Out-Host
if ($actualStaged.Count) {
    Invoke-Git @('commit','-m','Checkpoint validated isolated Smooth guard and complete Sculpt profile') | Out-Host
}
Invoke-Git @('push','--recurse-submodules=no','origin',("HEAD:refs/heads/$branch")) | Out-Host
$localHead = [string](Invoke-Git @('rev-parse','HEAD'))
$remoteLine = [string](Invoke-Git @('ls-remote','--heads','origin',("refs/heads/$branch")))
$remoteHead = ($remoteLine -split '\s+')[0]
if ($localHead -cne $remoteHead) { throw 'Push eseguito ma verifica SHA remoto non corrispondente. Invia questo messaggio.' }
Write-Host "PUBBLICATO E VERIFICATO: $localHead" -ForegroundColor Green
Write-Host "Branch: $branch"
Write-Host "Backup repository: $backup"
Write-Host 'Sorgenti di lavoro e build non modificati. Nessuna compilazione avviata.'



