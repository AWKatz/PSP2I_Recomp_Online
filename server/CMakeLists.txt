# psp2i_server: the PSP2i dedicated server (headless).
#
# Standalone:   cmake -S server -B server/build -DCMAKE_BUILD_TYPE=Release
#               cmake --build server/build
# Or with the game: -DBUILD_DEDICATED_SERVER=ON on port/macos, port/linux or
# port (Windows) builds it next to psp2i.
#
# Links psprecomp_net (the ENet transport, the control protocol, LAN discovery,
# the config reader) and nothing else: no game runtime, rendering, audio, SDL.
# ENet is built from source and linked statically.
cmake_minimum_required(VERSION 3.22)
project(psp2i_server C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(BUILD_DEDICATED_SERVER "Build psp2i_server" ON)
if(NOT BUILD_DEDICATED_SERVER)
    return()
endif()
if(NOT TARGET psprecomp_net)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../psprecomp ${CMAKE_CURRENT_BINARY_DIR}/psprecomp EXCLUDE_FROM_ALL)
endif()

add_library(psp2i_server_core STATIC src/server.c src/clock.c)
target_include_directories(psp2i_server_core PUBLIC src)
target_link_libraries(psp2i_server_core PUBLIC psprecomp_net)
if(WIN32)
    target_link_libraries(psp2i_server_core PUBLIC psapi winmm)
endif()
if(NOT MSVC)
    target_compile_options(psp2i_server_core PRIVATE -Wall -Wextra)
endif()

add_executable(psp2i_server src/main.c)
target_link_libraries(psp2i_server PRIVATE psp2i_server_core)

# A fully static Linux binary (copy one file to a VPS): -DPSP2I_SERVER_STATIC=ON
option(PSP2I_SERVER_STATIC "Link psp2i_server statically (Linux)" OFF)
if(PSP2I_SERVER_STATIC AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(psp2i_server PRIVATE -static)
endif()

include(GNUInstallDirs)
install(TARGETS psp2i_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_executable(test_server EXCLUDE_FROM_ALL tests/test_server.c)
target_link_libraries(test_server PRIVATE psp2i_server_core)
add_test(NAME test_server COMMAND test_server)
