param(
    [string]$SourceRoot = 'C:\BlenderDev\blender',
    [string]$BuildDir = 'C:\BlenderDev\build_windows_x64_shareable',
    [switch]$RemovePatch
)
$ErrorActionPreference = 'Stop'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima di applicare o rimuovere la patch.' }
# Close only reusable MSBuild workers whose parent has already exited.
# Any other build process blocks before sources are changed.
$processes = @(Get-CimInstance Win32_Process)
$orphans = @($processes | Where-Object {
    $p = $_
    $p.Name -eq 'MSBuild.exe' -and $p.CommandLine -match '/nodemode:1' -and
    $p.CommandLine -match '/nodeReuse:true' -and
    !($processes | Where-Object { $_.ProcessId -eq $p.ParentProcessId })
})
$busy = @($processes | Where-Object {
    $_.Name -in @('cmake.exe','MSBuild.exe','cl.exe','link.exe','ninja.exe','nvcc.exe') -and
    $_.ProcessId -notin @($orphans.ProcessId)
})
if ($busy.Count) { throw ('Compilazione ancora attiva: ' + (($busy | ForEach-Object { $_.Name + ' PID=' + $_.ProcessId }) -join ', ')) }
foreach ($node in $orphans) {
    # Recheck identity and parent immediately before stopping this exact worker.
    $live = Get-CimInstance Win32_Process -Filter ("ProcessId = " + $node.ProcessId)
    if (!$live) { continue }
    if ($live.CreationDate -ne $node.CreationDate -or
        $live.CommandLine -ne $node.CommandLine -or
        (Get-Process -Id $live.ParentProcessId -ErrorAction SilentlyContinue)) {
        throw 'Processi MSBuild cambiati durante il controllo. Nessun sorgente modificato.'
    }
    Write-Host ("Chiusura nodo MSBuild senza processo padre: " + $node.ProcessId)
    Stop-Process -Id $node.ProcessId -ErrorAction SilentlyContinue
    Wait-Process -Id $node.ProcessId -Timeout 5 -ErrorAction SilentlyContinue
}
$remaining = @(Get-Process -Name cmake,MSBuild,cl,link,ninja,nvcc -ErrorAction SilentlyContinue)
if ($remaining.Count) {
    foreach ($process in $remaining) {
        Get-CimInstance Win32_Process -Filter ("ProcessId = " + $process.Id) |
            Select-Object Name,ProcessId,ParentProcessId,CommandLine | Format-List | Out-Host
    }
    throw ('Processi ancora presenti: ' + (($remaining | ForEach-Object { $_.ProcessName + ' PID=' + $_.Id }) -join ', ') + '. Nessun sorgente modificato.')
}
$cache = Join-Path $BuildDir 'CMakeCache.txt'
$entry = Select-String -LiteralPath $cache -Pattern '^CMAKE_COMMAND:INTERNAL=(.+)$'
if (!$entry) { throw 'CMake non trovato nella cache della build.' }
$cmake = $entry.Matches[0].Groups[1].Value.Trim()
if (!(Test-Path -LiteralPath $cmake)) { throw 'Eseguibile CMake non trovato.' }
$homeEntry = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'
if (!$homeEntry -or $homeEntry.Matches[0].Groups[1].Value.Replace('/', '\').TrimEnd('\') -ine $SourceRoot.Replace('/', '\').TrimEnd('\')) {
    throw 'La build usa sorgenti diversi. Nessuna modifica.'
}
function Count-Literal([string]$Text, [string]$Needle) {
    $count = 0; $offset = 0
    while ($offset -le $Text.Length) {
        $index = $Text.IndexOf($Needle, $offset, [StringComparison]::Ordinal)
        if ($index -lt 0) { break }
        $count++; $offset = $index + $Needle.Length
    }
    return $count
}
$blocks = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText((Join-Path $PSScriptRoot 'blocks.json')))
$texts = @{}; $initial = @{}
# Validate EVERY block before writing any source. Other experiments are retained.
foreach ($block in $blocks) {
    if ($block.path -isnot [string] -or $block.old -isnot [string] -or $block.new -isnot [string]) {
        throw 'Elenco dei blocchi non valido. Nessun file modificato.'
    }
    $file = Join-Path $SourceRoot $block.path
    if (!$texts.ContainsKey($file)) {
        $texts[$file] = [IO.File]::ReadAllText($file).Replace("`r`n", "`n")
        $initial[$file] = $texts[$file]
    }
    $before = if ($RemovePatch) { [string]$block.new } else { [string]$block.old }
    $after = if ($RemovePatch) { [string]$block.old } else { [string]$block.new }
    $nBefore = Count-Literal $texts[$file] $before
    $nAfter = Count-Literal $texts[$file] $after
    if ($nAfter -eq 1 -and $nBefore -eq 0) { continue }
    if ($nBefore -ne 1 -or $nAfter -ne 0) {
        throw "Blocco diverso o non univoco: $($block.path). Nessun file modificato. Invia questo messaggio."
    }
    $texts[$file] = $texts[$file].Replace($before, $after)
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$backup = Join-Path $PSScriptRoot ('backup-' + $stamp)
New-Item -ItemType Directory -Path $backup | Out-Null
foreach ($file in $texts.Keys) {
    if ($texts[$file] -cne $initial[$file]) {
        $relative = $file.Substring($SourceRoot.TrimEnd('\','/').Length).TrimStart('\','/')
        $target = Join-Path $backup $relative
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $file -Destination $target
    }
}
$exe = Join-Path $BuildDir 'bin\Release\blender.exe'
if (Test-Path -LiteralPath $exe) { Copy-Item -LiteralPath $exe -Destination (Join-Path $backup 'blender.exe') }
foreach ($file in $texts.Keys) {
    if ($texts[$file] -cne $initial[$file]) {
        [IO.File]::WriteAllText($file, $texts[$file], (New-Object Text.UTF8Encoding($false)))
    }
}
Write-Host "Backup: $backup"
& $cmake --build $BuildDir --config Release --target blender --parallel 8
if ($LASTEXITCODE -ne 0) { throw 'Build fallita. Non avviare la vecchia build; invia gli errori.' }
Write-Host 'Compilazione completata. Sculpt Layer Smooth Guard V02 si abilita con AVVIA.ps1.' -ForegroundColor Green
