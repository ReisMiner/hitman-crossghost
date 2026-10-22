# Cross-compiles the plugin for Windows (MSVC ABI) from a Linux host with
# clang-cl, lld-link and an xwin-provided MSVC CRT/Windows SDK.
#
# Usage:
#   cmake -S . -B build-linux -G Ninja \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-windows-msvc.cmake \
#       -DCMAKE_BUILD_TYPE=Release
#
# XWIN_ROOT can point at a different xwin splat; it defaults to /xwin, which is
# where the CI image puts it.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# CMake's compiler/ABI probes default to the Debug configuration, which needs
# the non-redistributable debug CRT that xwin prunes. The plugin only ships a
# release build, so probe with Release instead.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release CACHE STRING "" FORCE)

set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)
set(CMAKE_LINKER lld-link)
set(CMAKE_AR llvm-lib)
set(CMAKE_RC_COMPILER llvm-rc)

set(XWIN_ROOT "/xwin" CACHE PATH "xwin CRT and Windows SDK root")
if(NOT EXISTS "${XWIN_ROOT}/crt/include" OR NOT EXISTS "${XWIN_ROOT}/sdk/include/um")
    message(FATAL_ERROR "xwin sysroot not found at ${XWIN_ROOT}. Run 'xwin --accept-license splat --output ${XWIN_ROOT}' first.")
endif()

# clang-cl on Linux needs an explicit MSVC target and the xwin include dirs.
# CMake drives lld-link directly, so the CRT/SDK library dirs must be passed to
# the linker explicitly as well.
set(_ghost_msvc_flags
    "--target=x86_64-pc-windows-msvc"
    "/vctoolsdir" "${XWIN_ROOT}/crt"
    "/winsdkdir" "${XWIN_ROOT}/sdk"
    "-Wno-unused-command-line-argument")
string(JOIN " " _ghost_msvc_flags ${_ghost_msvc_flags})
set(CMAKE_C_FLAGS_INIT "${_ghost_msvc_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_ghost_msvc_flags}")

set(_ghost_link_flags
    "/libpath:${XWIN_ROOT}/crt/lib/x86_64"
    "/libpath:${XWIN_ROOT}/sdk/lib/um/x86_64"
    "/libpath:${XWIN_ROOT}/sdk/lib/ucrt/x86_64")
string(JOIN " " _ghost_link_flags ${_ghost_link_flags})
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_ghost_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_ghost_link_flags}")

# CTest uses this to execute the cross-built test binaries on the build host.
find_program(GHOST_WINE_EXECUTABLE NAMES wine64 wine)
if(GHOST_WINE_EXECUTABLE)
    set(CMAKE_CROSSCOMPILING_EMULATOR "${GHOST_WINE_EXECUTABLE}" CACHE FILEPATH
        "Emulator for Windows test executables (Wine)")
endif()
