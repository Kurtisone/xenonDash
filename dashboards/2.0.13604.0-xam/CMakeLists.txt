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

# Module hud.xex (cible dash_hud, palier G) : même contournement du registre, plus les imports XAM routés vers XAM
# recompilé (src_hud/hud_xam_imports.cpp, produit par tools/gen_module_xam_imports.py).
if(TARGET dash_hud)
    target_sources(dash_hud PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/generated/dash/hud/dash_register.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/src_hud/hud_xam_imports.cpp)
    target_include_directories(dash_hud PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src)
endif()

# Crochets D3D de XAM (palier N2) : définitions fortes des sub_<adresse> de la bibliothèque D3D liée dans xam.xex ;
# elles appellent hudd3d_call (exécutable : src/hud_d3d_trace.cpp, puis native_gpu/src/hud_front.cpp).
if(TARGET dash_xam)
    target_sources(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_d3d/xam_d3d_hooks.cpp)
    target_include_directories(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_d3d)
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

# Rendu natif D3D (comme 2.0.13604.0) : Dashboard par native_gpu/front_profile.cpp, HUD de XAM par native_gpu/src/hud_front.cpp (palier N2).
option(DASH_NATIVE_GPU "Rendu natif au niveau D3D (native_gpu/, ngpu_core), Dashboard et HUD de XAM" OFF)
if(DASH_NATIVE_GPU)
    set(NGPU_PROFILE 13604 CACHE STRING "Version du D3D invité (native_gpu/CMakeLists.txt)" FORCE)
    set(NGPU_HUD ON CACHE BOOL "Facade du HUD de XAM" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../native_gpu xenondash_native_gpu)
    target_link_libraries(dash PRIVATE xenondash_native_gpu)
    target_compile_definitions(dash PRIVATE DASH_NATIVE_GPU=1)
endif()
# Le module XAM (DLL chargée par dlopen) appelle des fonctions de l'exécutable (accroches [[midasm_hook]] de
# xam_overrides.toml, src/xam_neutralize.cpp) : leurs symboles doivent figurer dans la table dynamique de l'exécutable.
set_target_properties(dash PROPERTIES ENABLE_EXPORTS ON)
rexglue_setup_target(dash)
