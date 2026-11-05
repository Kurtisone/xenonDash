# dash - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(DASH_SOURCES
    src/main.cpp
    src/kernel_stubs.cpp
    src/diag_hooks.cpp
)

if(WIN32)
    add_executable(dash WIN32 ${DASH_SOURCES})
else()
    add_executable(dash ${DASH_SOURCES})
endif()

rexglue_setup_target(dash)
