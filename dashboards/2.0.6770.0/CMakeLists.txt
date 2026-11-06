# dash 2.0.6770.0 - projet ReXGlue (gabarit : tools/templates/dashboard, copié par tools/port_dashboard.sh).
#
# Le boilerplate du SDK est dans generated/rexglue.cmake (écrit par `rexglue codegen`).

cmake_minimum_required(VERSION 3.25)
project(dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Code partagé entre versions (bibliothèque OBJECT : tous les hooks sont liés)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common xenondash_common)

# Sources propres à la version : main.cpp, plus tout autre src/*.cpp (stubs d'imports, hooks de diagnostic...)
file(GLOB DASH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

if(WIN32)
    add_executable(dash WIN32 ${DASH_SOURCES})
else()
    add_executable(dash ${DASH_SOURCES})
endif()

target_link_libraries(dash PRIVATE xenondash_common)
rexglue_setup_target(dash)
