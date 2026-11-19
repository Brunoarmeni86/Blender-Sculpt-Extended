$ErrorActionPreference = 'Stop'
$sourceRoot = 'C:\BlenderDev\blender'
$buildRoot = 'C:\BlenderDev\build_windows_x64_shareable'
$exe = Join-Path $buildRoot 'bin\Release\blender.exe'
$baseDir = 'C:\BlenderDev\Checkpoints'
if (Get-Process -Name blender -ErrorAction SilentlyContinue) { throw 'Chiudi Blender prima del checkpoint.' }
if (!(Test-Path -LiteralPath $exe) -or !(Test-Path -LiteralPath (Join-Path $sourceRoot 'source\blender'))) {
    throw 'Sorgenti o eseguibile non trovati.'
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dest = Join-Path $baseDir ('Sculpt-Pre-Detail-Transport-' + $stamp)
if (Test-Path -LiteralPath $dest) { throw 'Cartella checkpoint gia esistente.' }
New-Item -ItemType Directory -Path $dest -Force | Out-Null
$codeDest = Join-Path $dest 'blender'
New-Item -ItemType Directory -Path $codeDest | Out-Null
$folders = @('source','scripts','intern','extern','build_files','release')
foreach ($folder in $folders) {
    $from = Join-Path $sourceRoot $folder
    if (!(Test-Path -LiteralPath $from)) { throw "Cartella mancante: $from" }
    $to = Join-Path $codeDest $folder
    & robocopy.exe $from $to /E /XJ /R:1 /W:1 /NFL /NDL /NP /XD __pycache__ .git
    if ($LASTEXITCODE -ge 8) { throw "Copia fallita ($folder). Checkpoint parziale: $dest" }
}
foreach ($file in @('CMakeLists.txt','GNUmakefile','make.bat','pyproject.toml')) {
    $from = Join-Path $sourceRoot $file
    if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination (Join-Path $codeDest $file) }
}
Copy-Item -LiteralPath $exe -Destination (Join-Path $dest 'blender.exe')
Copy-Item -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Destination (Join-Path $dest 'CMakeCache.txt')
$allFiles = @(Get-ChildItem -LiteralPath $codeDest -Recurse -File)
$manifest = foreach ($file in $allFiles) {
    [pscustomobject]@{
        RelativePath = $file.FullName.Substring($codeDest.Length + 1)
        Length = $file.Length
        SHA256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
}
$manifest | Export-Csv -LiteralPath (Join-Path $dest 'source-manifest.csv') -NoTypeInformation -Encoding UTF8
$exeHash = (Get-FileHash -LiteralPath (Join-Path $dest 'blender.exe') -Algorithm SHA256).Hash
@(
    "SourceRoot=$sourceRoot",
    "BuildRoot=$buildRoot",
    "Created=$([DateTime]::Now.ToString('o'))",
    "SourceFiles=$($allFiles.Count)",
    "BlenderExeSHA256=$exeHash",
    'Scope=source,scripts,intern,extern,build_files,release and root build files; no library binaries or build objects'
) | Set-Content -LiteralPath (Join-Path $dest 'CHECKPOINT.txt') -Encoding UTF8
Write-Host "Checkpoint: $dest" -ForegroundColor Green
Write-Host "File sorgenti: $($allFiles.Count); blender.exe SHA256: $exeHash"
