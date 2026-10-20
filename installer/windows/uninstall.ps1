<#
.SYNOPSIS
  Removes what install.ps1 placed (ADR 0001, Windows port).

.DESCRIPTION
  Reads %LOCALAPPDATA%\ck3agent\install.json. Removes the bridge (version.dll beside
  ck3.exe) only if it is still the file the installer put there (same SHA-256); a
  version.dll that changed since is left in place and named. Removes the venv and
  the ck3 entry of the play directory's .mcp.json (other servers in it are kept).
  -KeepData keeps %LOCALAPPDATA%\ck3agent (logs, state).
#>
[CmdletBinding()]
param([switch]$KeepData)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3
# JSON written as UTF-8 WITHOUT a byte-order mark: Windows PowerShell 5.1's
# `Set-Content -Encoding utf8` adds one, which JSON readers may refuse.
function Write-Utf8NoBom([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text, (New-Object System.Text.UTF8Encoding $false))
}

$localAppData = if ($env:CK3AGENT_DATA_DIR) { $env:CK3AGENT_DATA_DIR }
                elseif ($env:LOCALAPPDATA) { Join-Path $env:LOCALAPPDATA 'ck3agent' }
                else { Write-Error 'LOCALAPPDATA is not set'; exit 1 }
$state = Join-Path $localAppData 'install.json'
if (-not (Test-Path $state)) { Write-Host "nothing to remove: $state not found"; exit 0 }
$s = Get-Content $state -Raw | ConvertFrom-Json

if (Test-Path $s.dll_path) {
    $there = (Get-FileHash -Algorithm SHA256 $s.dll_path).Hash.ToLowerInvariant()
    if ($there -eq $s.dll_sha256) { Remove-Item -Force $s.dll_path; Write-Host "removed $($s.dll_path)" }
    else { Write-Warning "$($s.dll_path) changed since install (another tool's file?); left in place" }
}

$mcpPath = Join-Path $s.play_dir '.mcp.json'
if (Test-Path $mcpPath) {
    $mcp = Get-Content $mcpPath -Raw | ConvertFrom-Json
    if (($mcp.PSObject.Properties.Match('mcpServers').Count -gt 0) -and ($mcp.mcpServers.PSObject.Properties.Match('ck3').Count -gt 0)) {
        $mcp.mcpServers.PSObject.Properties.Remove('ck3')
        Write-Utf8NoBom $mcpPath ($mcp | ConvertTo-Json -Depth 8)
        Write-Host "removed the ck3 entry from $mcpPath"
    }
}

if (Test-Path $s.venv) { Remove-Item -Recurse -Force $s.venv; Write-Host "removed $($s.venv)" }
Remove-Item -Force $state
if (-not $KeepData) { Remove-Item -Recurse -Force $localAppData -ErrorAction SilentlyContinue; Write-Host "removed $localAppData" }
Write-Host 'ck3agent uninstalled.'
