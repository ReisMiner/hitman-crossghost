#!/usr/bin/env bash
# Cross-builds dist/GhostCrossplay.dll on Linux (MSVC ABI via clang-cl + xwin).
# Mirrors build.ps1: CONFIG selects the build type, GHOST_PUBLIC_ORIGIN overrides
# the backend origin, and BUILD_DIR overrides the build directory.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
config="${CONFIG:-Release}"
origin="${GHOST_PUBLIC_ORIGIN:-https://hitmanghost.ramon.cc}"
build="${BUILD_DIR:-$root/build-linux}"
xwin_root="${XWIN_ROOT:-/xwin}"

extra=()
if [[ -n "${GHOST_DEVELOPMENT_SERVICE_OVERRIDE:-}" ]]; then
    extra+=(-DGHOST_DEVELOPMENT_SERVICE_OVERRIDE=ON)
fi
if [[ -n "${GHOST_TEST_EMULATOR:-}" ]]; then
    extra+=(-DCMAKE_CROSSCOMPILING_EMULATOR="$GHOST_TEST_EMULATOR")
fi

cmake -S "$root" -B "$build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/toolchain-windows-msvc.cmake" \
    -DCMAKE_BUILD_TYPE="$config" \
    -DGHOST_PUBLIC_ORIGIN="$origin" \
    -DXWIN_ROOT="$xwin_root" \
    "${extra[@]}"
cmake --build "$build" --parallel

dll="$build/GhostCrossplay.dll"
if [[ ! -f "$dll" ]]; then
    echo "Build did not produce $dll" >&2
    exit 1
fi

mkdir -p "$root/dist"
cp "$dll" "$root/dist/GhostCrossplay.dll"
sha256sum "$root/dist/GhostCrossplay.dll"
