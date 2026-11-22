# PSP savedata encryption (libkirk + the sceChnnlsv model): PPSSPP/PSP saves load and save as on hardware.
set(_SC ${CMAKE_CURRENT_LIST_DIR})
add_library(psp2i_savecrypt STATIC
    ${_SC}/host/savecrypt.c
    ${_SC}/third_party/libkirk/AES.c ${_SC}/third_party/libkirk/SHA1.c ${_SC}/third_party/libkirk/bn.c
    ${_SC}/third_party/libkirk/ec.c ${_SC}/third_party/libkirk/kirk_engine.c)
target_include_directories(psp2i_savecrypt PUBLIC ${_SC}/host PRIVATE ${_SC}/third_party/libkirk)
target_link_libraries(psp2i_savecrypt PUBLIC psprecomp)
if(NOT MSVC)   # third-party code: keep its warnings out of ours
    set_source_files_properties(${_SC}/third_party/libkirk/AES.c ${_SC}/third_party/libkirk/SHA1.c
        ${_SC}/third_party/libkirk/bn.c ${_SC}/third_party/libkirk/ec.c ${_SC}/third_party/libkirk/kirk_engine.c
        PROPERTIES COMPILE_OPTIONS -w)
endif()
