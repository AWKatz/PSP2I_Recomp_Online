# Linux desktop build (x86_64 and arm64; the Steam Deck included). Packed as AppImages
# by port/linux/appimage/build_appimage.sh. The same SDL host as macOS: see port/sdl_desktop.cmake.
cmake_minimum_required(VERSION 3.22)
project(psp2i_linux C CXX)
set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include(${PORT}/sdl_desktop.cmake)
