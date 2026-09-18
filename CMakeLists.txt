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
    runtime/textedit.c      # TextEdit (TERec in guest memory)
    runtime/files.c         # File Manager over the title's own media
    runtime/toolbox.c       # A-trap dispatch (Pascal/register conventions)
    runtime/platform_sdl.c  # SDL2 window, present, input
)
target_compile_definitions(macrecomp PRIVATE MACRECOMP_NO_TRAP_STUB)
target_include_directories(macrecomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# MACRECOMP_SDL is PUBLIC on purpose: a generated loader carries headless plat_*
# stubs so a title can run with no display, and they are compiled only when this
# is absent. Defining it PRIVATE would build platform_sdl.c and the stubs both,
# and the link would fail on duplicate symbols.
find_package(SDL2 QUIET)
if(SDL2_FOUND)
    target_link_libraries(macrecomp PUBLIC SDL2::SDL2)
    target_compile_definitions(macrecomp PUBLIC MACRECOMP_SDL)
else()
    # fall back to pkg-config (MSYS2/mingw)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET sdl2)
        if(SDL2_FOUND)
            target_include_directories(macrecomp PUBLIC ${SDL2_INCLUDE_DIRS})
            target_link_libraries(macrecomp PUBLIC ${SDL2_LIBRARIES})
            target_compile_definitions(macrecomp PUBLIC MACRECOMP_SDL)
        endif()
    endif()
endif()
if(NOT MSVC)
    target_link_libraries(macrecomp PUBLIC m)
endif()

option(MACRECOMP_EXAMPLES "Build example harnesses" OFF)
if(MACRECOMP_EXAMPLES)
    add_executable(hal_selftest examples/hal_selftest.c
        runtime/quickdraw.c runtime/toolbox.c runtime/dialog.c runtime/textedit.c
        runtime/files.c runtime/m68k.c)
    target_compile_definitions(hal_selftest PRIVATE MACRECOMP_NO_TRAP_STUB)
    target_include_directories(hal_selftest PRIVATE include)
    if(NOT MSVC)
        target_link_libraries(hal_selftest m)
    endif()

    # No HAL and no display: just the function table and its dispatch.
    add_executable(entry_dispatch_test examples/entry_dispatch_test.c runtime/m68k.c)
    target_compile_definitions(entry_dispatch_test PRIVATE MACRECOMP_NO_TRAP_STUB)
    target_include_directories(entry_dispatch_test PRIVATE include)

    enable_testing()
    add_test(NAME hal_selftest COMMAND hal_selftest)
    add_test(NAME entry_dispatch COMMAND entry_dispatch_test)
endif()
