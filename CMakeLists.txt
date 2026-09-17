cmake_minimum_required(VERSION 3.16)
project(macrecomp C)

set(CMAKE_C_STANDARD 11)

# The linkable runtime: m68k execution substrate + Toolbox HAL (QuickDraw/Event/
# Memory) over SDL2. Consumers: add_subdirectory(ext/macrecomp); link `macrecomp`.
add_library(macrecomp STATIC
    runtime/macrecomp.c     # version
    runtime/m68k.c          # CPU state, memory, flags, function table/dispatch
    runtime/quickdraw.c     # 1-bit framebuffer + drawing primitives
    runtime/dialog.c        # Dialog + Control Manager over QuickDraw
    runtime/toolbox.c       # A-trap dispatch (Pascal/register conventions)
    runtime/platform_sdl.c  # SDL2 window, present, input
)
target_compile_definitions(macrecomp PRIVATE MACRECOMP_NO_TRAP_STUB)
target_include_directories(macrecomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_link_libraries(macrecomp PUBLIC SDL2::SDL2)
else()
    # fall back to pkg-config (MSYS2/mingw)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
        target_include_directories(macrecomp PUBLIC ${SDL2_INCLUDE_DIRS})
        target_link_libraries(macrecomp PUBLIC ${SDL2_LIBRARIES})
    endif()
endif()
if(NOT MSVC)
    target_link_libraries(macrecomp PUBLIC m)
endif()

option(MACRECOMP_EXAMPLES "Build example harnesses" OFF)
if(MACRECOMP_EXAMPLES)
    add_executable(hal_selftest examples/hal_selftest.c
        runtime/quickdraw.c runtime/toolbox.c runtime/dialog.c runtime/m68k.c)
    target_compile_definitions(hal_selftest PRIVATE MACRECOMP_NO_TRAP_STUB)
    target_include_directories(hal_selftest PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(hal_selftest m)
    endif()
endif()
