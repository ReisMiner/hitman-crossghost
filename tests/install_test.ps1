param([Parameter(Mandatory = $true)][string]$Source)
$ErrorActionPreference = 'Stop'
$installer = Join-Path (Split-Path $PSScriptRoot -Parent) 'scripts\install.ps1'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('ghost-install-test-' + [guid]::NewGuid().ToString('N'))
$mods = Join-Path $fixture 'Retail\mods'
$modsIni = Join-Path $fixture 'Retail\mods.ini'

function Require([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}

try {
    New-Item -ItemType Directory -Path $mods | Out-Null
    $settings = "[sdk]`r`nui_toggle_key = 0xE047`r`n[GhostDiagnostic]`r`n[OtherPlugin]`r`nvalue = keep-me`r`n"
    [IO.File]::WriteAllText($modsIni, $settings)
    [IO.File]::WriteAllText((Join-Path $mods 'GhostDiagnostic.dll'), 'old Epic binary')
    [IO.File]::WriteAllText((Join-Path $mods 'GhostGuest.dll'), 'old Steam binary')
    [IO.File]::WriteAllText((Join-Path $mods 'GhostCrossplay.dll'), 'pre-rename binary')
    [IO.File]::WriteAllText((Join-Path $mods 'ghostdiagnostic.ini'), "[ghost_diagnostic]`r`nforce_local_host = false`r`n")
    [IO.File]::WriteAllText((Join-Path $mods 'ghostguest.ini'), "[ghost_guest]`r`nRelayTarget = 10.0.0.9:30001`r`n")
    $legacyHashes = @{}
    foreach ($name in @('GhostDiagnostic.dll', 'GhostGuest.dll', 'GhostCrossplay.dll', 'ghostdiagnostic.ini', 'ghostguest.ini')) {
        $legacyHashes[$name] = (Get-FileHash -LiteralPath (Join-Path $mods $name)).Hash
    }

    $result = (& $installer -Source $Source -GameRoot $fixture) | ConvertFrom-Json
    Require ($result.sha256 -eq (Get-FileHash -LiteralPath $Source).Hash) 'Installed bytes differ from the supplied universal DLL.'
    Require (!(Test-Path -LiteralPath (Join-Path $mods 'GhostDiagnostic.dll'))) 'Legacy Epic DLL remains loadable.'
    Require (!(Test-Path -LiteralPath (Join-Path $mods 'GhostGuest.dll'))) 'Legacy Steam DLL remains loadable.'
    Require (!(Test-Path -LiteralPath (Join-Path $mods 'GhostCrossplay.dll'))) 'Pre-rename GhostCrossplay DLL remains loadable.'
    Require ((Split-Path $result.target -Leaf) -eq 'CrossGhost.dll') 'Plugin was not installed as CrossGhost.dll.'
    Require ($result.backups.Count -eq 3) 'Every legacy DLL must be backed up.'
    foreach ($backup in $result.backups) {
        $name = Split-Path $backup.Original -Leaf
        Require ((Get-FileHash -LiteralPath $backup.Backup).Hash -eq $legacyHashes[$name]) 'A legacy DLL backup changed bytes.'
    }
    foreach ($name in @('ghostdiagnostic.ini', 'ghostguest.ini')) {
        Require ((Get-FileHash -LiteralPath (Join-Path $mods $name)).Hash -eq $legacyHashes[$name]) 'Legacy settings were changed.'
    }
    $enabled = [IO.File]::ReadAllText($modsIni)
    Require ($enabled.StartsWith($settings)) 'Unrelated SDK settings were changed.'
    Require ($enabled.Contains('[CrossGhost]')) 'CrossGhost was not enabled.'

    # Reinstallation must retain user settings and avoid duplicate SDK sections.
    $unifiedIni = Join-Path $mods 'crossghost.ini'
    [IO.File]::WriteAllText($unifiedIni, "[ghost_guest]`r`nRelayTarget = 10.1.1.1:30001`r`n")
    $settingsHash = (Get-FileHash -LiteralPath $unifiedIni).Hash
    $second = (& $installer -Source $Source -GameRoot $fixture) | ConvertFrom-Json
    Require (!$second.enabledSectionAdded) 'Repeated installation duplicated the enabled section.'
    Require ([IO.File]::ReadAllText($modsIni) -eq $enabled) 'Reinstallation rewrote unrelated SDK configuration.'
    Require ((Get-FileHash -LiteralPath $unifiedIni).Hash -eq $settingsHash) 'Unified settings were overwritten.'

    $rejected = $false
    try { & $installer -Source (Join-Path $fixture 'missing.dll') -GameRoot $fixture | Out-Null }
    catch { $rejected = $true }
    Require $rejected 'A missing source was accepted.'
    Require ((Get-FileHash -LiteralPath $result.target).Hash -eq $result.sha256) 'Failed installation changed the active DLL.'
    Write-Output 'PASS: unified installation, legacy backups, settings preservation, repeated update, missing-source rejection'
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
