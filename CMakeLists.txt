cmake_minimum_required(VERSION 3.16)
project(macrecomp C)

set(CMAKE_C_STANDARD 11)

# The linkable runtime library. v0 is a stub; the Toolbox HAL grows here.
# Consumers: add_subdirectory(ext/macrecomp) then link `macrecomp`.
add_library(macrecomp STATIC
    runtime/macrecomp.c
)
target_include_directories(macrecomp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# SDL2 is pulled in here once the video/audio HAL exists (Phase 4):
#   find_package(SDL2 REQUIRED)
#   target_link_libraries(macrecomp PUBLIC SDL2::SDL2)

option(MACRECOMP_EXAMPLES "Build example harnesses" OFF)
if(MACRECOMP_EXAMPLES)
    add_subdirectory(examples)
endif()
