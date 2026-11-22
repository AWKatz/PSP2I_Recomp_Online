# Tests are self-contained: no game data, no ROMs, no disc images. Every input
# is either a hand-written instruction encoding or a synthetic buffer built in
# the test itself.

add_executable(test_decode test_decode.c)
target_link_libraries(test_decode PRIVATE allegrex_core)
add_test(NAME decode COMMAND test_decode)

add_executable(test_runtime test_runtime.c)
target_link_libraries(test_runtime PRIVATE psprecomp)
add_test(NAME runtime COMMAND test_runtime)

add_executable(test_container test_container.c)
target_link_libraries(test_container PRIVATE allegrex_core)
add_test(NAME container COMMAND test_container)

# Published standard vectors only (FIPS-197, SP 800-38A, RFC 4493) — no PSP
# key material, no game data.
add_executable(test_crypto test_crypto.c)
target_link_libraries(test_crypto PRIVATE allegrex_core)
add_test(NAME crypto COMMAND test_crypto)

add_executable(test_kirk test_kirk.c)
target_link_libraries(test_kirk PRIVATE allegrex_core)
add_test(NAME kirk COMMAND test_kirk)

add_executable(test_emit test_emit.c)
target_link_libraries(test_emit PRIVATE allegrex_core psprecomp)
add_test(NAME emit COMMAND test_emit)



add_executable(test_hle test_hle.c)
target_link_libraries(test_hle PRIVATE psprecomp allegrex_core)
add_test(NAME hle COMMAND test_hle)

add_executable(test_vfpu test_vfpu.c)
target_link_libraries(test_vfpu PRIVATE psprecomp)
add_test(NAME vfpu COMMAND test_vfpu)

add_executable(test_ctors test_ctors.c)
target_link_libraries(test_ctors PRIVATE psprecomp)
add_test(NAME ctors COMMAND test_ctors)

add_executable(test_raster test_raster.c)
target_link_libraries(test_raster PRIVATE psprecomp)
add_test(NAME raster COMMAND test_raster)

# PGF font parsing. Synthetic data only; set PSPRECOMP_FONT_DIR to a directory
# holding the PSP's own font files to run the extra real-font checks.
add_executable(test_font test_font.c)
target_link_libraries(test_font PRIVATE psprecomp)
add_test(NAME font COMMAND test_font)

add_executable(test_ge_vertex test_ge_vertex.c)
target_link_libraries(test_ge_vertex PRIVATE psprecomp)
add_test(NAME ge_vertex COMMAND test_ge_vertex)

if(ENABLE_MMO_NET)
    add_executable(test_net_transport test_net_transport.c)
    target_link_libraries(test_net_transport PRIVATE psprecomp)
    add_test(NAME net_transport COMMAND test_net_transport)

    add_executable(test_enet_transport test_enet_transport.c)
    target_link_libraries(test_enet_transport PRIVATE psprecomp)
    add_test(NAME enet_transport COMMAND test_enet_transport)

    add_executable(test_toml_lite test_toml_lite.c)
    target_link_libraries(test_toml_lite PRIVATE psprecomp)
    add_test(NAME toml_lite COMMAND test_toml_lite)

    add_executable(test_mmo_handshake test_mmo_handshake.c)
    target_link_libraries(test_mmo_handshake PRIVATE psprecomp)
    add_test(NAME mmo_handshake COMMAND test_mmo_handshake)

    add_executable(test_net_policy test_net_policy.c)   # psprecomp_net alone, as the server links it
    target_link_libraries(test_net_policy PRIVATE psprecomp_net)
    add_test(NAME net_policy COMMAND test_net_policy)

    # not a test: a stand-in server that prints client handshakes (Task 1.3 check)
    add_executable(mmo_listen mmo_listen.c)
    target_link_libraries(mmo_listen PRIVATE psprecomp)
endif()
