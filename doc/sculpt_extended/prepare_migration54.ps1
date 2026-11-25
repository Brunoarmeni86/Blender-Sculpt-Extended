# SPDX-License-Identifier: GPL-2.0-or-later
# Fetch immutable upstream and inventory local changes without touching working files.
[CmdletBinding()]
param(
    [string]$Repository = (Get-Location).Path,
    [string]$Worktree = 'C:\BlenderDev\blender54-stock-20261125',
    [string]$Inventory = 'C:\BlenderDev\migration54-inventory-20261125',
    [switch]$PublishUpstream
)
$ErrorActionPreference = 'Stop'
$source = '6aac2da6b01481fd71b83132882e6ee4e1193bfc'
$release = '09a16d4b8482200101cb30728881ca4524edf019'
$upstream = '668b05cf77f45b0b599d9aab730f30efd28e7705'
$branch = 'migration/54-upstream-20261125'
$upstreamUrl = 'https://github.com/blender/blender.git'

function Invoke-GitChecked {
    param([Parameter(ValueFromRemainingArguments=$true)][string[]]$GitArgs)
    & git -C $Repository @GitArgs
    if ($LASTEXITCODE -ne 0) { throw "git failed ($LASTEXITCODE): $($GitArgs -join ' ')" }
}

$Repository = (Resolve-Path -LiteralPath $Repository).Path
$origin = (Invoke-GitChecked remote get-url origin | Select-Object -Last 1).Trim()
if ($origin -notmatch '(?i)github\.com[:/]Brunoarmeni86/Blender-Sculpt-Extended(\.git)?/?$') {
    throw "Unexpected origin: $origin. Use a checkout of Sculpt Extended."
}
if ((Invoke-GitChecked rev-parse --is-shallow-repository | Select-Object -Last 1) -eq 'true') {
    throw 'A full history is required for an accurate migration inventory.'
}
if (Test-Path -LiteralPath $Worktree) { throw "Worktree path already exists: $Worktree" }
if (Test-Path -LiteralPath $Inventory) { throw "Inventory path already exists: $Inventory" }
$existingBranch = & git -C $Repository branch --list $branch
if ($LASTEXITCODE -ne 0 -or $existingBranch) { throw "Local branch already exists: $branch" }

# Fetch updates refs/objects, not the checked-out files.
Invoke-GitChecked fetch --no-tags origin '+refs/heads/*:refs/remotes/origin/*'
Invoke-GitChecked fetch --no-tags $upstreamUrl $upstream
$versionPath = 'source/blender/blenkernel/BKE_blender_version.h'
$version = Invoke-GitChecked show "${upstream}:$versionPath"
if (-not ($version -match '^#define BLENDER_VERSION 504\s*$') -or
    -not ($version -match '^#define BLENDER_VERSION_CYCLE alpha\s*$')) {
    throw 'Pinned source is not the verified Blender 5.4 Alpha.'
}
foreach ($sha in @($source, $release, $upstream)) {
    Invoke-GitChecked cat-file -t $sha | Out-Null
}

New-Item -ItemType Directory -Path $Inventory | Out-Null
$version | Set-Content -Encoding UTF8 (Join-Path $Inventory 'upstream-version.h')
@("release=$release", "source=$source", "upstream=$upstream") |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'inputs.txt')
Invoke-GitChecked status --porcelain=v1 |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'local-uncommitted.txt')
Invoke-GitChecked diff --name-status $release $source |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'release-to-v30b3-files.txt')
Invoke-GitChecked log --format='%H %s' "$release..$source" |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'release-to-v30b3-commits.txt')

$common = Invoke-GitChecked merge-base $upstream $source | Select-Object -Last 1
$common | Set-Content -Encoding UTF8 (Join-Path $Inventory 'upstream-common-ancestor.txt')
Invoke-GitChecked diff --name-status $common $source |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'common-to-custom-files.txt')
Invoke-GitChecked diff --name-status $common $upstream |
    Set-Content -Encoding UTF8 (Join-Path $Inventory 'common-to-upstream-files.txt')
# These include any differing upstream maintenance commits as well; classify before porting.
$refs = Invoke-GitChecked for-each-ref '--format=%(refname)' refs/remotes/origin/
$uniqueDir = Join-Path $Inventory 'branch-unique'
New-Item -ItemType Directory -Path $uniqueDir | Out-Null
foreach ($ref in $refs) {
    if ($ref -eq 'refs/remotes/origin/HEAD') { continue }
    $name = $ref.Substring('refs/remotes/origin/'.Length) -replace '[^A-Za-z0-9._-]', '_'
    # Include a short SHA in the filename to avoid ambiguous sanitized names.
    $tip = Invoke-GitChecked rev-parse --short=12 $ref | Select-Object -Last 1
    $unique = Invoke-GitChecked log --right-only --cherry-pick --no-merges --format='%H %s' "$source...$ref"
    @("ref=$ref", "tip=$tip", $unique) |
        Set-Content -Encoding UTF8 (Join-Path $uniqueDir "$name-$tip.txt")
}
# Audit exact official 5.2.3 tag if available. Do not confuse the shared ancestor
# with the stock release; fail explicitly if fetch is blocked.
& git -C $Repository fetch --no-tags $upstreamUrl 'refs/tags/v5.2.3'
if ($LASTEXITCODE -eq 0) {
    $stock = Invoke-GitChecked rev-parse 'FETCH_HEAD^{commit}' | Select-Object -Last 1
    $stock | Set-Content -Encoding UTF8 (Join-Path $Inventory 'stock-523-sha.txt')
    Invoke-GitChecked diff --name-status $stock $release |
        Set-Content -Encoding UTF8 (Join-Path $Inventory 'stock523-to-release-files.txt')
    Invoke-GitChecked diff --name-status $stock $source |
        Set-Content -Encoding UTF8 (Join-Path $Inventory 'stock523-to-v30b3-files.txt')
} else {
    Write-Warning 'Official v5.2.3 tag fetch failed. Stock-release audit remains incomplete.'
}

# No existing branches are rewritten; worktree checkout is separate.
Invoke-GitChecked worktree add -b $branch $Worktree $upstream
$actual = (& git -C $Worktree rev-parse HEAD | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0 -or $actual -ne $upstream) {
    throw 'New worktree did not resolve to the pinned SHA.'
}
if ($PublishUpstream) {
    Invoke-GitChecked push origin "refs/heads/${branch}:refs/heads/${branch}"
}
Write-Host "Stock 5.4 worktree: $Worktree"
Write-Host "Migration inventory: $Inventory"
Write-Host 'No port, build, merge, reset, or runtime test was performed.'
