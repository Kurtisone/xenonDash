# dash 2.0.13604.0 avec XAM 13604 recompilé (module ReXGlue, palier A) : projet de test, copie de 2.0.13604.0.
#
# Le boilerplate du SDK est dans generated/rexglue.cmake (écrit par `rexglue codegen`).

cmake_minimum_required(VERSION 3.25)
project(dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Module XAM (cible dash_xam, créée par generated/dash/dll_targets.cmake) : le gabarit du SDK (fork, commit e28335b) ne
# range plus {projet}_register.cpp dans les sources : ReXModule_Register / ReXModule_GetImageInfo manquaient dans la DLL
# (« ReXModule_Register not found »). On ajoute le fichier généré à la cible. À retirer quand le SDK le rétablit
# (docs/xam_recompile.md).
if(TARGET dash_xam)
    target_sources(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/generated/xam/dash_register.cpp)
endif()

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
