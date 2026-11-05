# dash - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Code partagé entre versions (bibliothèque OBJECT : tous les hooks sont liés)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common xenondash_common)

# Sources propres à la version 2.0.6770.0
set(DASH_SOURCES
    src/main.cpp
    src/version_stubs.cpp
    src/diag_hooks.cpp
)

if(WIN32)
    add_executable(dash WIN32 ${DASH_SOURCES})
else()
    add_executable(dash ${DASH_SOURCES})
endif()

target_link_libraries(dash PRIVATE xenondash_common)
rexglue_setup_target(dash)
