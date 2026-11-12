# SPDX-License-Identifier: GPL-3.0-or-later
# ngpu_core : noyau de rendu natif Vulkan commun au Dashboard et aux jeux (P8, voir docs/ngpu_core.md).
# Reprise de Skate1Native (GPL-3.0) sur plume (MIT). Independant du jeu et du SDK : peripherique Vulkan, textures + detuilage +
# formats, cibles de rendu (EDRAM), resolves, pipelines, shaders XenosRecomp par cle de microcode.
#
# Utilisation : add_subdirectory(ngpu_core) puis target_link_libraries(<cible> PRIVATE ngpu_core).
# Options : NGPU_BUILD_REPLAY (outil de rejeu hors ligne, demande un cache de shaders de Skate).

cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ngpu_core LANGUAGES CXX)
endif()

if(NOT TARGET plume)
  find_package(Vulkan REQUIRED)
  find_package(X11 REQUIRED)
  # plume : RHI Vulkan seul (renderbag/plume, MIT), compile avec les en-tetes Vulkan du systeme.
  add_library(plume STATIC third_party/plume/plume_vulkan.cpp)
  target_include_directories(plume PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/plume
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/plume/contrib/volk
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/plume/contrib/VulkanMemoryAllocator/include
    ${Vulkan_INCLUDE_DIRS} ${X11_INCLUDE_DIR})
  target_compile_features(plume PUBLIC cxx_std_17)
  set_target_properties(plume PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
  target_link_libraries(plume PUBLIC ${CMAKE_DL_LIBS} ${X11_LIBRARIES})
endif()

# volk definit des POINTEURS GLOBAUX nommes comme les fonctions Vulkan (vkCreateInstance...) : dans un executable qui lie aussi
# libvulkan (backend de presentation de XenonDash), ils prendraient la place des fonctions du chargeur et planteraient. On les
# renomme (ngpu_vk...) pour plume et le noyau seulement : en-tete genere ici depuis les declarations de volk.h, force-inclus en PRIVATE.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/third_party/plume/contrib/volk/volk.h _ngpu_volk)
string(REGEX MATCHALL "extern PFN_vk[A-Za-z0-9_]+ vk[A-Za-z0-9_]+" _ngpu_decls "${_ngpu_volk}")
set(_ngpu_rename "// genere par ngpu_core/CMakeLists.txt : ne pas versionner\n#pragma once\n")
foreach(_d ${_ngpu_decls})
  string(REGEX REPLACE ".* (vk[A-Za-z0-9_]+)$" "\\1" _n "${_d}")
  string(APPEND _ngpu_rename "#define ${_n} ngpu_${_n}\n")
endforeach()
set(NGPU_VOLK_RENAME "${CMAKE_CURRENT_BINARY_DIR}/ngpu_volk_rename.h")
file(WRITE "${NGPU_VOLK_RENAME}" "${_ngpu_rename}")

# En-tete seul : XXH_INLINE_ALL. Pris dans le SDK du projet (sous-module) ou le prefixe installe.
find_path(NGPU_XXHASH_DIR xxhash.h
  HINTS ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rexglue-sdk/thirdparty/xxHash ${REXSDK_PREFIX}/include
  NO_DEFAULT_PATH)
if(NOT NGPU_XXHASH_DIR)
  find_path(NGPU_XXHASH_DIR xxhash.h REQUIRED)
endif()

if(TARGET plume AND NOT NGPU_VOLK_RENAMED)
  target_compile_options(plume PRIVATE -include "${NGPU_VOLK_RENAME}")
  set(NGPU_VOLK_RENAMED ON)
endif()

add_library(ngpu_core STATIC
  src/renderer.cpp
  src/device.cpp
)
target_include_directories(ngpu_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${NGPU_XXHASH_DIR})
target_compile_features(ngpu_core PUBLIC cxx_std_20)
set_target_properties(ngpu_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_libraries(ngpu_core PUBLIC plume)
target_compile_options(ngpu_core PRIVATE -include "${NGPU_VOLK_RENAME}")

option(NGPU_BUILD_REPLAY "Construire l'outil de rejeu hors ligne ngpu_replay" OFF)
if(NGPU_BUILD_REPLAY)
  add_subdirectory(tools/replay)
endif()
