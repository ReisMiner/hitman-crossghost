# Configures and builds CrossGhost.dll. Without -NoPublish the DLL is
# copied to dist/. Run scripts/fetch-deps.ps1 once before the first build.
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    # CrossGhost server origin compiled into the DLL (game backend and room service).
    [string]$PublicOrigin = 'https://hitmanghost.ramon.cc',
    [switch]$NoPublish,
    # Lets GHOST_SERVICE_URL override the room service at runtime (testing only).
    [switch]$DevelopmentServiceOverride
)

$ErrorActionPreference = 'Stop'
$build = Join-Path $PSScriptRoot 'build'
$dist = Join-Path $PSScriptRoot 'dist'

cmake -S $PSScriptRoot -B $build -G 'Visual Studio 17 2022' -A x64 `
    "-DGHOST_PUBLIC_ORIGIN=$PublicOrigin" `
    "-DGHOST_DEVELOPMENT_SERVICE_OVERRIDE=$($DevelopmentServiceOverride.IsPresent.ToString().ToUpperInvariant())"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }
cmake --build $build --config $Config --parallel
if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" }

$dll = Join-Path $build "$Config\CrossGhost.dll"
if (!(Test-Path -LiteralPath $dll)) { throw "Build did not produce $dll" }
if (!$NoPublish) {
    if (!$PublicOrigin) { throw 'Publishing requires -PublicOrigin. Use -NoPublish for local development.' }
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $dist 'CrossGhost.dll') -Force
    $dll = Join-Path $dist 'CrossGhost.dll'
    Write-Host "Universal DLL ready: $dll"
}
Get-FileHash -LiteralPath $dll -Algorithm SHA256 | Format-List Path, Hash
