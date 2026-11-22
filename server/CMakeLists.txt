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

# SQLite (vendored amalgamation, public domain): WAL + JSON, no extension
# loading (no dlopen), foreign keys on by default.
add_library(psp2i_sqlite STATIC third_party/sqlite/sqlite3.c)
target_include_directories(psp2i_sqlite PUBLIC third_party/sqlite)
target_compile_definitions(psp2i_sqlite PUBLIC
    SQLITE_THREADSAFE=1 SQLITE_ENABLE_JSON1 SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_DEFAULT_WAL_SYNCHRONOUS=2 SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE)
if(NOT MSVC)
    target_compile_options(psp2i_sqlite PRIVATE -w)   # third-party: not our warnings
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(psp2i_sqlite PUBLIC Threads::Threads)
    if(NOT APPLE)
        target_link_libraries(psp2i_sqlite PUBLIC m)
    endif()
endif()

add_library(psp2i_server_core STATIC src/server.c src/clock.c src/db.c src/crypto.c src/loot.c)
target_include_directories(psp2i_server_core PUBLIC src)
target_link_libraries(psp2i_server_core PUBLIC psprecomp_net psp2i_sqlite)
if(WIN32)
    target_link_libraries(psp2i_server_core PUBLIC psapi winmm bcrypt)
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
install(FILES config/drop_tables.json config/rates.toml DESTINATION ${CMAKE_INSTALL_DATADIR}/psp2i_server/config)

enable_testing()
add_executable(test_server EXCLUDE_FROM_ALL tests/test_server.c)
target_link_libraries(test_server PRIVATE psp2i_server_core)
add_test(NAME test_server COMMAND test_server)
add_executable(test_loot EXCLUDE_FROM_ALL tests/test_loot.c)
target_link_libraries(test_loot PRIVATE psp2i_server_core)
add_test(NAME test_loot COMMAND test_loot ${CMAKE_CURRENT_SOURCE_DIR}/config/drop_tables.json ${CMAKE_CURRENT_SOURCE_DIR}/config/rates.toml)
if(NOT WIN32)   # the interrupt tests fork and SIGKILL a child mid-transaction
    add_executable(test_db EXCLUDE_FROM_ALL tests/test_db.c)
    target_link_libraries(test_db PRIVATE psp2i_server_core)
    add_test(NAME test_db COMMAND test_db)
endif()
