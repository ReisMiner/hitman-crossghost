cmake_minimum_required(VERSION 3.20)

project(GhostCrossplay LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(GHOST_PUBLIC_ORIGIN "https://hitmanghost.ramon.cc" CACHE STRING
    "LocalGhost origin: game backend and room service")
option(GHOST_DEVELOPMENT_SERVICE_OVERRIDE
    "Allow the GHOST_SERVICE_URL environment variable to override the room service (testing only)" OFF)
set(GHOST_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party" CACHE PATH
    "ZHMModSDK and DirectX-Headers (scripts/fetch-deps.ps1)")
set(GHOST_LOCALGHOST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../localghost" CACHE PATH
    "LocalGhost checkout, used by the room tunnel integration test")

if(NOT EXISTS "${GHOST_DEPS_DIR}/ZHMModSDK-4.1.1/CMakeLists.txt")
    message(FATAL_ERROR "ZHMModSDK not found in ${GHOST_DEPS_DIR}. Run scripts/fetch-deps.ps1 first.")
endif()

# ZHMModSDK 4.1.1 DevPkg; the game ships the identical SDK runtime.
set(ZHMMODSDK_DIST_DIR "${GHOST_DEPS_DIR}/ZHMModSDK-4.1.1/release" CACHE PATH "" FORCE)
set(ZHMMODSDK_INCLUDE_DIR "${GHOST_DEPS_DIR}/ZHMModSDK-4.1.1/include" CACHE PATH "" FORCE)
add_subdirectory("${GHOST_DEPS_DIR}/ZHMModSDK-4.1.1" "${CMAKE_BINARY_DIR}/zhmdevpkg")

# Networking code shared by the plugin and the tests. No game or SDK dependency.
# GhostNet embeds the public origin; GhostNetTesting leaves it empty so tests
# can point the room client at a local service.
set(GHOST_NET_SOURCES
    src/net/host_proxy.cpp
    src/net/metadata_fetcher.cpp
    src/net/room_client.cpp
    src/net/udp.cpp)
function(ghost_add_net_library name)
    add_library(${name} STATIC ${GHOST_NET_SOURCES})
    target_include_directories(${name} PUBLIC src)
    target_compile_definitions(${name} PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(${name} PRIVATE /utf-8 /W4)
    target_link_libraries(${name} PUBLIC ws2_32 winhttp bcrypt)
endfunction()

ghost_add_net_library(GhostNet)
target_compile_definitions(GhostNet PRIVATE
    GHOST_SERVICE_URL="${GHOST_PUBLIC_ORIGIN}"
    GHOST_BACKEND_URL="${GHOST_PUBLIC_ORIGIN}"
    $<$<BOOL:${GHOST_DEVELOPMENT_SERVICE_OVERRIDE}>:GHOST_DEVELOPMENT_SERVICE_OVERRIDE>)

add_library(GhostCrossplay SHARED
    src/plugin_entry.cpp
    src/game_build.cpp
    src/backend/backend_patch.cpp
    src/common/memory.cpp
    src/common/sha256.cpp
    src/epic/epic_guest.cpp
    src/epic/epic_plugin.cpp
    src/steam/steam_lobby_shim.cpp
    src/steam/steam_plugin.cpp
    src/ui/room_panel.cpp)
target_include_directories(GhostCrossplay PRIVATE src "${GHOST_DEPS_DIR}/DirectX-Headers/include")
target_compile_definitions(GhostCrossplay PRIVATE
    SPDLOG_FMT_EXTERNAL
    GHOST_BACKEND_URL="${GHOST_PUBLIC_ORIGIN}")
target_compile_options(GhostCrossplay PRIVATE /utf-8)
target_link_libraries(GhostCrossplay PRIVATE
    GhostNet
    ZHMModSDK
    "${ZHMMODSDK_DIST_DIR}/lib/implot.lib"
    bcrypt
    version)

include(CTest)
if(BUILD_TESTING)
    ghost_add_net_library(GhostNetTesting)

    add_executable(GhostRelayTests tests/relay_metadata_test.cpp)
    target_link_libraries(GhostRelayTests PRIVATE GhostNetTesting)
    add_test(NAME relay-metadata COMMAND GhostRelayTests)

    add_test(NAME install-script COMMAND pwsh -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_test.ps1" -Source "$<TARGET_FILE:GhostCrossplay>")

    add_executable(GhostRoomTests tests/room_relay_test.cpp)
    target_link_libraries(GhostRoomTests PRIVATE GhostNetTesting)

    add_executable(GhostMockHost tests/mock_host.cpp)
    target_link_libraries(GhostMockHost PRIVATE GhostNetTesting)

    set(ROOM_SERVICE "${GHOST_LOCALGHOST_DIR}/Server/components/ghostRooms/service.mjs")
    if(EXISTS "${ROOM_SERVICE}")
        find_program(NODE_EXECUTABLE node REQUIRED)
        add_test(NAME room-tunnel COMMAND "${NODE_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/room_tunnel_test.mjs" "${ROOM_SERVICE}" "$<TARGET_FILE:GhostRoomTests>")
    else()
        message(STATUS "LocalGhost not found at ${GHOST_LOCALGHOST_DIR}; skipping the room-tunnel test")
    endif()
endif()
