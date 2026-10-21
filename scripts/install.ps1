# Installs GhostCrossplay.dll into a HITMAN 3 game directory and enables it in
# ZHMModSDK's mods.ini. Older GhostDiagnostic.dll/GhostGuest.dll builds and any
# previous GhostCrossplay.dll are renamed to timestamped backups. Rolls back on
# failure. Prints a JSON summary. Close HITMAN first; Program Files needs elevation.
param(
    [string]$Source = (Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\GhostCrossplay.dll'),
    [string]$GameRoot = 'C:\Program Files\Epic Games\HITMAN3'
)

$ErrorActionPreference = 'Stop'
if (Get-Process HITMAN3 -ErrorAction SilentlyContinue) { throw 'Close HITMAN before updating the plugin.' }
if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing plugin: $Source" }
$mods = Join-Path $GameRoot 'Retail\mods'
if (!(Test-Path -LiteralPath $mods -PathType Container)) { throw "Missing mods directory: $mods" }

$target = Join-Path $mods 'GhostCrossplay.dll'
$token = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$staged = "$target.new-$token"
$modsIni = Join-Path $GameRoot 'Retail\mods.ini'
$hadModsIni = Test-Path -LiteralPath $modsIni
$iniText = if ($hadModsIni) { [IO.File]::ReadAllText($modsIni) } else { '' }
$enablePlugin = $iniText -notmatch '(?im)^\s*\[ghostcrossplay\]\s*(?:[;#].*)?$'
$iniBackup = $null
$backups = @()
$installed = $false
$writingIni = $false
try {
    Copy-Item -LiteralPath $Source -Destination $staged
    $expectedHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    if ((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne $expectedHash) {
        throw 'Staged DLL hash does not match the source.'
    }
    if ($enablePlugin -and $hadModsIni) {
        $iniBackup = "$modsIni.backup-$token"
        Copy-Item -LiteralPath $modsIni -Destination $iniBackup
    }
    # Retire both legacy names so the SDK cannot install overlapping hooks.
    # Keep their INIs for first-run settings migration by the unified plugin.
    foreach ($name in @('GhostDiagnostic.dll', 'GhostGuest.dll', 'GhostCrossplay.dll')) {
        $path = Join-Path $mods $name
        if (Test-Path -LiteralPath $path) {
            $backup = "$path.backup-$token"
            Move-Item -LiteralPath $path -Destination $backup
            $backups += [pscustomobject]@{ Original = $path; Backup = $backup }
        }
    }
    Move-Item -LiteralPath $staged -Destination $target
    $installed = $true
    if ($enablePlugin) {
        $writingIni = $true
        [IO.File]::WriteAllText($modsIni, $iniText + "`r`n[GhostCrossplay]`r`n", [Text.UTF8Encoding]::new($false))
    }
} catch {
    if ($installed) { Remove-Item -LiteralPath $target -Force }
    foreach ($entry in $backups) { Move-Item -LiteralPath $entry.Backup -Destination $entry.Original }
    if ($writingIni) {
        if ($iniBackup) { Copy-Item -LiteralPath $iniBackup -Destination $modsIni -Force }
        elseif (!$hadModsIni -and (Test-Path -LiteralPath $modsIni)) { Remove-Item -LiteralPath $modsIni }
    }
    throw
} finally {
    if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged }
}

[ordered]@{
    target = $target
    sha256 = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    backups = $backups
    modsIniBackup = $iniBackup
    enabledSectionAdded = $enablePlugin
} | ConvertTo-Json -Depth 4
