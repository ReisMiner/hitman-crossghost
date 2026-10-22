#!/usr/bin/env bash
# Downloads the build dependencies into third_party/ (not committed):
#   - ZHMModSDK 4.1.1 DevPkg (headers, import libraries, CMake package)
#   - microsoft/DirectX-Headers at the commit this project was built with
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
destination="${1:-$root/third_party}"

sdk_url='https://github.com/OrfeasZ/ZHMModSDK/releases/download/v4.1.1/DevPkg-ZHMModSDK.zip'
sdk_sha256='FCF42DABE1F5E0015FE50B4CFE50244635ED1D556BDEBF8DB642E64F4D9FD683'
directx_repository='https://github.com/microsoft/DirectX-Headers.git'
directx_commit='461ef5138bd30922b44fbad77c4e47662812d2d5'

mkdir -p "$destination"

sdk="$destination/ZHMModSDK-4.1.1"
if [[ ! -f "$sdk/CMakeLists.txt" ]]; then
    zip="$(mktemp "$destination/DevPkg-ZHMModSDK-v4.1.1.XXXXXX.zip")"
    trap 'rm -f "$zip"' EXIT
    curl -fsSL "$sdk_url" -o "$zip"
    echo "$sdk_sha256  $zip" | sha256sum -c -
    mkdir -p "$sdk"
    unzip -q "$zip" -d "$sdk"
    rm -f "$zip"
    trap - EXIT
fi

directx="$destination/DirectX-Headers"
if [[ ! -d "$directx/include" ]]; then
    git clone --quiet "$directx_repository" "$directx"
    git -C "$directx" checkout --quiet "$directx_commit"
fi

echo "Dependencies ready in $destination"
