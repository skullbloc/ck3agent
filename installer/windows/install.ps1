<#
.SYNOPSIS
  ck3agent Windows installer (ADR 0001, Windows port). Run from the unpacked release zip.

.DESCRIPTION
  1. Finds Crusader Kings III (-GameDir, else $env:CK3AGENT_GAME_DIR, else the Steam
     libraries named in Steam's libraryfolders.vdf).
  2. Installs the ck3agent command into a private venv under
     %LOCALAPPDATA%\ck3agent (from the wheels in this zip; no network).
  3. Checks that ck3.exe is the one build this release supports (pin.json: the
     CodeView GUID+age and SizeOfImage the bridge's own identity gate compares).
     On a mismatch it stops before touching the game directory; -Force installs
     anyway, and the bridge then refuses to start (the game runs unmodified).
  4. Places the bridge, version.dll, beside ck3.exe. A version.dll there that this
     installer did not put there is never overwritten.
  5. Writes %USERPROFILE%\ck3agent-play\.mcp.json, the play directory Claude Code is
     started in, as `ck3agent setup` does on Linux.
  What it installed is recorded in %LOCALAPPDATA%\ck3agent\install.json, which
  uninstall.ps1 reads.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\install.ps1
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$PlayDir,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
# JSON written as UTF-8 WITHOUT a byte-order mark: Windows PowerShell 5.1's
# `Set-Content -Encoding utf8` adds one, which JSON readers may refuse.
function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Die([string]$msg) { Write-Error $msg -ErrorAction Continue; exit 1 }
function Step([string]$msg) { Write-Host "-- $msg" }

foreach ($f in 'VERSION', 'pin.json', 'version.dll', 'SHA256SUMS') {
    if (-not (Test-Path (Join-Path $here $f))) { Die "$f not found in $here; run install.ps1 from the unpacked release zip" }
}
$version = (Get-Content (Join-Path $here 'VERSION') -Raw).Trim()
$pin = Get-Content (Join-Path $here 'pin.json') -Raw | ConvertFrom-Json
Write-Host "ck3agent $version installer (Windows)"

# The zip's own checksums first: a damaged or altered file stops everything.
Step 'checking the release files against SHA256SUMS'
foreach ($line in Get-Content (Join-Path $here 'SHA256SUMS')) {
    if (-not $line.Trim()) { continue }
    $parts = $line -split '\s+', 2
    $want = $parts[0].ToLowerInvariant(); $name = $parts[1].TrimStart('*')
    $got = (Get-FileHash -Algorithm SHA256 (Join-Path $here $name)).Hash.ToLowerInvariant()
    if ($got -ne $want) { Die "$name does not match SHA256SUMS (got $got)" }
}

$localAppData = if ($env:CK3AGENT_DATA_DIR) { $env:CK3AGENT_DATA_DIR }
                elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'ck3agent' }
                else { Die 'LOCALAPPDATA is not set' }
$userHome = if ($env:USERPROFILE) { $env:USERPROFILE } else { $HOME }

# 1. The game.
function Find-SteamRoots {
    $roots = @()
    try {
        $sp = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        if ($sp) { $roots += $sp }
    } catch { }
    foreach ($r in @($roots)) {
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $roots += $m.Groups[1].Value -replace '\\\\', '\'
            }
        }
    }
    return $roots | Select-Object -Unique
}
if (-not $GameDir) { $GameDir = $env:CK3AGENT_GAME_DIR }
if (-not $GameDir) {
    foreach ($r in Find-SteamRoots) {
        $cand = Join-Path $r 'steamapps\common\Crusader Kings III'
        if (Test-Path (Join-Path $cand 'binaries\ck3.exe')) { $GameDir = $cand; break }
    }
}
if (-not $GameDir) { Die 'Crusader Kings III not found in the Steam libraries; pass -GameDir "<...>\Crusader Kings III"' }
$bin = Join-Path $GameDir 'binaries'
$exe = Join-Path $bin 'ck3.exe'
if (-not (Test-Path $exe)) { Die "no ck3.exe at $exe" }
Step "game: $exe"

# 2. The ck3agent command, in a private venv.
$py = $null
foreach ($c in @(@('py', '-3'), @('python3'), @('python'))) {
    $cmd = Get-Command $c[0] -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }
    $args2 = @($c | Select-Object -Skip 1) + @('-c', 'import sys; sys.exit(sys.version_info < (3, 11))')
    & $cmd.Source @args2 2>$null
    if ($LASTEXITCODE -eq 0) { $py = @($cmd.Source) + @($c | Select-Object -Skip 1); break }
}
if (-not $py) { Die 'need Python 3.11 or newer (python.org installer, "Add to PATH"); none found' }
$venv = Join-Path $localAppData 'venv'
Step "venv: $venv"
New-Item -ItemType Directory -Force -Path $localAppData | Out-Null
$pyArgs = @($py | Select-Object -Skip 1) + @('-m', 'venv', '--clear', $venv)
& $py[0] @pyArgs
if ($LASTEXITCODE -ne 0) { Die 'python -m venv failed' }
$vpy = @((Join-Path $venv 'Scripts\python.exe'), (Join-Path $venv 'bin/python')) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vpy) { Die "no python in the new venv $venv" }
& $vpy -m pip install --quiet --no-index --find-links (Join-Path $here 'wheels') ck3agent
if ($LASTEXITCODE -ne 0) { Die 'pip install ck3agent from the bundled wheels failed' }
$ck3agentExe = @((Join-Path $venv 'Scripts\ck3agent.exe'), (Join-Path $venv 'bin/ck3agent')) | Where-Object { Test-Path $_ } | Select-Object -First 1

# 3. The build (the same check the bridge makes at load).
Step 'checking the game build against pin.json'
$probe = 'import sys, json; from ck3agent._pe import read_pe_identity as r; i = r(sys.argv[1]); ' +
         'print(json.dumps({"codeview": i.codeview_key, "size_of_image": i.size_of_image}))'
$idJson = & $vpy -c $probe $exe
if ($LASTEXITCODE -ne 0) { Die "could not read the identity of $exe" }
$id = $idJson | ConvertFrom-Json
$match = ($id.codeview -eq $pin.codeview_hex) -and ([uint64]$id.size_of_image -eq [uint64]$pin.size_of_image)
if (-not $match) {
    $msg = "ck3.exe is not $($pin.label) (CodeView $($id.codeview), SizeOfImage $($id.size_of_image)); " +
           "this release supports only that build (Steam: Properties > Betas > $($pin.steam_branch))."
    if (-not $Force) { Die "$msg Nothing was placed beside the game. -Force installs anyway (the bridge then stays off)." }
    Write-Warning "$msg Installing anyway (-Force): the bridge will refuse to start."
}

# 4. The bridge.
$dst = Join-Path $bin 'version.dll'
$ours = (Get-FileHash -Algorithm SHA256 (Join-Path $here 'version.dll')).Hash.ToLowerInvariant()
$state = Join-Path $localAppData 'install.json'
$prev = if (Test-Path $state) { Get-Content $state -Raw | ConvertFrom-Json } else { $null }
if (Test-Path $dst) {
    $there = (Get-FileHash -Algorithm SHA256 $dst).Hash.ToLowerInvariant()
    $placedByUs = $prev -and $prev.dll_path -eq $dst -and $prev.dll_sha256 -eq $there
    if (-not $placedByUs -and $there -ne $ours) {
        Die "$dst exists and was not placed by this installer (another mod or tool uses the same name); not overwritten"
    }
}
Copy-Item -Force (Join-Path $here 'version.dll') $dst
Step "bridge: $dst"

# 5. Claude Code's play directory.
if (-not $PlayDir) { $PlayDir = if ($env:CK3AGENT_PLAY_DIR) { $env:CK3AGENT_PLAY_DIR } else { Join-Path $userHome 'ck3agent-play' } }
New-Item -ItemType Directory -Force -Path $PlayDir | Out-Null
$mcpPath = Join-Path $PlayDir '.mcp.json'
$mcp = if (Test-Path $mcpPath) { Get-Content $mcpPath -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
if (-not ($mcp.PSObject.Properties.Match('mcpServers').Count -gt 0)) {
    $mcp | Add-Member -NotePropertyName mcpServers -NotePropertyValue ([pscustomobject]@{})
}
$entry = [pscustomobject]@{ command = $ck3agentExe; args = @('serve') }
if ($mcp.mcpServers.PSObject.Properties.Match('ck3').Count -gt 0) { $mcp.mcpServers.ck3 = $entry }
else { $mcp.mcpServers | Add-Member -NotePropertyName ck3 -NotePropertyValue $entry }
Write-Utf8NoBom $mcpPath ($mcp | ConvertTo-Json -Depth 8)
Step ".mcp.json: $mcpPath"

Write-Utf8NoBom $state ([pscustomobject]@{
    version = $version; dll_path = $dst; dll_sha256 = $ours; venv = $venv
    play_dir = $PlayDir; game_dir = $GameDir; identity_match = $match
} | ConvertTo-Json)

Write-Host ""
Write-Host "Installed ck3agent $version."
Write-Host "  1. Start Crusader Kings III from Steam and load a save. The bridge logs to"
Write-Host "     $localAppData\bridge.log and listens once the save is loaded."
Write-Host "  2. cd `"$PlayDir`" and run claude. Claude cannot play your own character: bind an AI ruler."
Write-Host "uninstall.ps1 removes everything this installer placed."
