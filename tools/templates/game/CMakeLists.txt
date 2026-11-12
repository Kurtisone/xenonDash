# SPDX-License-Identifier: GPL-3.0-or-later
# Projet de jeu recompile d'une recette XenonDash (@NAME@) : gabarit de tools/recipe_build.py, copie dans le cache
# ~/.local/share/xenondash/games/<TitleID>/<cle>/project/. Le boilerplate du SDK est dans generated/rexglue.cmake (rexglue codegen).
cmake_minimum_required(VERSION 3.25)
project(@NAME@ LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(XD_ROOT "@XENONDASH_ROOT@")

include(generated/rexglue.cmake)

# Hooks du jeu (recette : games/<TitleID>/src)
file(GLOB GAME_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/gen/*.cpp)

# Rendu natif (ngpu_core) : aucun plugin GPU. Cache de shaders XenosRecomp compile avec le jeu.
option(XD_NATIVE_GPU "Rendu natif par ngpu_core (zero emulation GPU)" ON)
set(XD_SHADER_CACHE "" CACHE FILEPATH "shader_cache.cpp produit par XenosRecomp (etape shaders)")
if(XD_NATIVE_GPU)
    if(NOT EXISTS "${XD_SHADER_CACHE}")
        message(FATAL_ERROR "XD_NATIVE_GPU : cache de shaders absent (${XD_SHADER_CACHE})")
    endif()
    add_subdirectory(${XD_ROOT}/ngpu_core ngpu_core)
    find_package(Vulkan REQUIRED)
    file(GLOB NATIVE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/native/*.cpp)
    list(APPEND GAME_SOURCES ${NATIVE_SOURCES}
        ${XD_ROOT}/native_gpu/src/vk_backend.cpp ${XD_SHADER_CACHE})
    set_source_files_properties(${XD_SHADER_CACHE} PROPERTIES
        INCLUDE_DIRECTORIES "${XD_ROOT}/ngpu_core/profiles/skate1")
endif()

add_executable(@NAME@ ${GAME_SOURCES})

if(XD_NATIVE_GPU)
    rexglue_setup_target(@NAME@)
    target_compile_definitions(@NAME@ PRIVATE XD_NATIVE_GPU=1)
    target_include_directories(@NAME@ PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/native ${CMAKE_CURRENT_BINARY_DIR}/gen ${CMAKE_CURRENT_SOURCE_DIR}/gen
        ${XD_ROOT}/native_gpu/src ${XD_ROOT}/ngpu_core/third_party/smol-v ${XD_ROOT}/ngpu_core/profiles/skate1)
    target_link_libraries(@NAME@ PRIVATE ngpu_core Vulkan::Vulkan)
    target_sources(@NAME@ PRIVATE ${XD_ROOT}/ngpu_core/third_party/smol-v/smolv.cpp)
else()
    rexglue_setup_target(@NAME@ GPU_PLUGINS xenos)
endif()

# librexruntime a cote de l'executable (le SDK l'ecrit dans son dossier ; l'hote ne cherche qu'a cote de lui).
if(NOT WIN32 AND TARGET rexruntime)
    add_custom_command(TARGET @NAME@ POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:rexruntime> $<TARGET_FILE_DIR:@NAME@>
        VERBATIM)
endif()
