# The desktop SDL3 build of PSP2i (macOS and Linux): the SDL host shared with
# Android, OpenGL 3.3+/4.1, FFmpeg movie video. Included by port/macos and
# port/linux after project(); PORT must point at port/.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(SDL3 CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_subdirectory(${PORT}/../psprecomp psprecomp EXCLUDE_FROM_ALL)
add_subdirectory(${PORT}/atrac_dll atrac_dll)
# The C generated from your EBOOT.BIN (build_macos.sh / build_flatpak.sh make it).
get_filename_component(_ROOT ${PORT}/.. ABSOLUTE)
set(PSP2I_GEN_DIR ${_ROOT}/build/gen CACHE PATH "Generated game code (allegrexrecomp emit)")
if(NOT EXISTS ${PSP2I_GEN_DIR}/recomp_funcs.c)
    message(FATAL_ERROR "No generated game code in ${PSP2I_GEN_DIR}: run build_macos.sh (or allegrexrecomp emit EBOOT.BIN build/gen recomp ...) first")
endif()
set(SPLIT_DIR ${CMAKE_BINARY_DIR}/gen_split)
execute_process(COMMAND ${Python3_EXECUTABLE} ${PORT}/android/split_gen.py
    ${PSP2I_GEN_DIR}/recomp_funcs.c ${SPLIT_DIR} 24 RESULT_VARIABLE split_rc)
if(NOT split_rc EQUAL 0)
    message(FATAL_ERROR "Could not split generated game code")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PSP2I_GEN_DIR}/recomp_funcs.c)
file(GLOB GEN_PARTS ${SPLIT_DIR}/recomp_part_*.c)
set_source_files_properties(${GEN_PARTS} PROPERTIES COMPILE_OPTIONS "-O1;-g0")
add_executable(psp2i ${PORT}/host/sdl_main.c ${PORT}/host/gl_ge.c ${PORT}/host/postfx.c
    ${PORT}/host/gl_load.c ${PORT}/host/atrac_at3.c ${PORT}/host/camera.c
    ${PORT}/host/framerate.c ${PORT}/host/menu.c ${PORT}/host/texpack.c ${GEN_PARTS} ${PSP2I_GEN_DIR}/recomp_imports.c)
target_include_directories(psp2i PRIVATE ${PORT}/host ${PSP2I_GEN_DIR} ${SPLIT_DIR}
    ${PORT}/third_party/xxhash ${PORT}/third_party/stb)
include(${PORT}/savecrypt.cmake)
target_link_libraries(psp2i PRIVATE psprecomp psp2i_savecrypt SDL3::SDL3)

enable_testing()   # PSP savedata crypto against a PPSSPP-written PARAM.SFO (port/tests/data)
add_executable(test_savecrypt EXCLUDE_FROM_ALL ${PORT}/tests/test_savecrypt.c)
target_link_libraries(test_savecrypt PRIVATE psp2i_savecrypt)
add_test(NAME test_savecrypt COMMAND test_savecrypt ${PORT}/tests/data/savecrypt_param.sfo)
if(TARGET psp2i_atrac)
    add_dependencies(psp2i psp2i_atrac)
    add_custom_command(TARGET psp2i POST_BUILD COMMAND ${CMAKE_COMMAND} -E
        copy_if_different $<TARGET_FILE:psp2i_atrac> $<TARGET_FILE_DIR:psp2i>)
endif()

if(ENABLE_MMO_NET)   # psprecomp's option: the MMO client start-up
    target_sources(psp2i PRIVATE ${PORT}/host/mmo_client.c)
endif()

option(PSP2I_LIBAV "Decode movie video with FFmpeg" ON)
if(PSP2I_LIBAV)
    find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h REQUIRED)
    find_library(AVCODEC_LIBRARY avcodec REQUIRED)
    find_library(AVUTIL_LIBRARY avutil REQUIRED)
    find_library(SWSCALE_LIBRARY swscale REQUIRED)
    add_library(psp2i_video STATIC ${PORT}/host/h264_libav.c)
    target_include_directories(psp2i_video PRIVATE ${AVCODEC_INCLUDE_DIR}
        ${PORT}/../psprecomp/include PUBLIC ${PORT}/host)
    target_link_libraries(psp2i_video PUBLIC ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
    target_compile_definitions(psp2i PRIVATE PSP2I_LIBAV)
    target_link_libraries(psp2i PRIVATE psp2i_video)
endif()
if(PSP2I_LIBAV)
    add_executable(video_decode EXCLUDE_FROM_ALL ${PORT}/tests/video_decode.c)
    target_include_directories(video_decode PRIVATE ${AVCODEC_INCLUDE_DIR} ${PORT}/../psprecomp/include)
    target_link_libraries(video_decode PRIVATE psp2i_video)
endif()

# The dedicated server, next to the game (-DBUILD_DEDICATED_SERVER=ON).
if(BUILD_DEDICATED_SERVER)
    add_subdirectory(${PORT}/../server ${CMAKE_BINARY_DIR}/server)
endif()

# Install layout (Linux packages, the Flatpak): the game and the ATRAC
# decoder side by side in bin/ (psp2i loads it from its own folder).
include(GNUInstallDirs)
install(TARGETS psp2i RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET psp2i_atrac)
    install(TARGETS psp2i_atrac LIBRARY DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(UNIX AND NOT APPLE)   # Linux: libm and libdl are not implicit
    find_package(Threads REQUIRED)
    target_link_libraries(psp2i PRIVATE m ${CMAKE_DL_LIBS} Threads::Threads)
endif()
