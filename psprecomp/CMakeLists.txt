# psprecomp — a static-recompilation toolkit for PlayStation Portable games.
#
# This file builds two things:
#   1. the `psprecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory map, semantic helpers)
#   2. `allegrexrecomp` — the tool that turns a PSP module into C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psprecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `psprecomp` target.
#
# The core has no external dependencies on purpose. SDL2 and friends only ever
# appear behind an opt-in option, so a fresh clone builds with nothing installed.

cmake_minimum_required(VERSION 3.16)
project(psprecomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # The generated C is millions of lines; MSVC's default object format
    # cannot hold that many sections.
    add_compile_options(/bigobj)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

add_library(psprecomp STATIC
    src/cpu.c
    src/mem.c
    src/dispatch.c
    src/ctors.c
    src/render.c
    src/vfpu.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/gpu.c
    src/hle/ge_vertex.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
    src/hle/system.c
    src/hle/modules.c
    src/hle/atrac.c
    src/hle/mpeg.c
    src/hle/utility.c
    src/hle/font.c
    src/hle/net.c
    src/hle/net_sock.c
    src/hle/http.c
    src/hle/offline.c
)

target_include_directories(psprecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Multiplayer (MMO) networking: the ad hoc transport seam. Off by default, so
# the single-player runtime is built from exactly the sources above. A game
# repo passes -DENABLE_MMO_NET=ON (the client) or -DBUILD_DEDICATED_SERVER=ON
# (a headless server, which links only psprecomp_net: no game runtime).
option(ENABLE_MMO_NET "Build the MMO network transport layer" OFF)
option(BUILD_DEDICATED_SERVER "Build the network library for a dedicated server" OFF)
if(ENABLE_MMO_NET OR BUILD_DEDICATED_SERVER)
    # ENet (MIT), built from source and linked statically: no runtime dependency.
    include(FetchContent)
    FetchContent_Declare(enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG        2662c0de09e36f2a2030ccc2c528a3e4c9e8138a   # v1.3.18
        GIT_SHALLOW    FALSE)
    FetchContent_MakeAvailable(enet)
    target_include_directories(enet PUBLIC ${enet_SOURCE_DIR}/include)
    if(WIN32)
        target_link_libraries(enet PUBLIC ws2_32 winmm)
    endif()
    if(NOT MSVC)
        target_compile_options(enet PRIVATE -w)   # third-party: not our warnings
    endif()

    # The transport, the control protocol's helpers, LAN discovery, the
    # config reader: shared by the MMO client and the dedicated server.
    add_library(psprecomp_net STATIC src/net/transport.c src/net/enet_transport.c
        src/net/lan_discovery.c src/net/toml_lite.c)
    target_include_directories(psprecomp_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(psprecomp_net PUBLIC enet)
    if(WIN32)
        target_link_libraries(psprecomp_net PUBLIC ws2_32)
    endif()
endif()
if(ENABLE_MMO_NET)
    target_sources(psprecomp PRIVATE src/net/adhoc_mmo.c src/net/adhoc_matching.c src/net/adhoc_sockets.c)
    target_compile_definitions(psprecomp PUBLIC PSPRECOMP_MMO_NET=1)
    target_link_libraries(psprecomp PUBLIC psprecomp_net)
endif()

# Networking HLE (src/hle/net.c) uses the host's sockets and adapter list.
if(WIN32)
    target_link_libraries(psprecomp PUBLIC ws2_32 iphlpapi)
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
