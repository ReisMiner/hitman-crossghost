# Downloads the build dependencies into third_party/ (not committed):
#   - ZHMModSDK 4.2.1 DevPkg (headers, import libraries, CMake package)
#   - microsoft/DirectX-Headers at the commit this project was built with
param([string]$Destination = (Join-Path (Split-Path $PSScriptRoot -Parent) 'third_party'))

$ErrorActionPreference = 'Stop'
$sdkUrl = 'https://github.com/OrfeasZ/ZHMModSDK/releases/download/v4.2.1/DevPkg-ZHMModSDK.zip'
$sdkSha256 = 'D13A986B588588D6456743BA0F9746DEA00B321A964B0269B98711597400E885'
$directXRepository = 'https://github.com/microsoft/DirectX-Headers.git'
$directXCommit = '461ef5138bd30922b44fbad77c4e47662812d2d5'

New-Item -ItemType Directory -Force -Path $Destination | Out-Null

$sdk = Join-Path $Destination 'ZHMModSDK-4.2.1'
if (!(Test-Path -LiteralPath (Join-Path $sdk 'CMakeLists.txt'))) {
    $zip = Join-Path $Destination 'DevPkg-ZHMModSDK-v4.2.1.zip'
    Invoke-WebRequest -Uri $sdkUrl -OutFile $zip
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $sdkSha256) {
        Remove-Item -LiteralPath $zip
        throw 'ZHMModSDK download does not match the expected SHA-256.'
    }
    Expand-Archive -LiteralPath $zip -DestinationPath $sdk
    Remove-Item -LiteralPath $zip
}

$directX = Join-Path $Destination 'DirectX-Headers'
if (!(Test-Path -LiteralPath (Join-Path $directX 'include'))) {
    git clone --quiet $directXRepository $directX
    if ($LASTEXITCODE -ne 0) { throw 'Cloning DirectX-Headers failed.' }
    git -C $directX checkout --quiet $directXCommit
    if ($LASTEXITCODE -ne 0) { throw 'Checking out the pinned DirectX-Headers commit failed.' }
}

Write-Host "Dependencies ready in $Destination"
