# native_gpu : rendu natif au niveau D3D (remplace l'emulation du GPU Xenos). Voir docs/rendu_natif.md et docs/ngpu_core.md.
# Bibliotheque OBJECT (symboles forts qui remplacent les alias faibles `sub_XXXXXXXX` du code genere).
# Inclue seulement avec l'option DASH_NATIVE_GPU=ON du projet de la version (desactivee par defaut).
#
# NGPU_CORE (defaut ON) : facade D3D 6770 sur le noyau de rendu commun ngpu_core (front6770.cpp ; rendu plume/Vulkan de Skate1Native).
# NGPU_CORE=OFF : ancien backend Vulkan maison (hooks_draw.cpp, conserve comme second oracle).

option(NGPU_CORE "Facade 6770 sur le noyau ngpu_core (OFF : ancien backend vk_backend)" ON)

# NGPU_PROFILE : XDK D3D liee dans le Dashboard (defini par le CMakeLists de la version avant add_subdirectory ; defaut 6770).
#   6770  : D3D 2.0.6132, crochets d'adresses fixes (front6770.cpp, hooks_frame.cpp), entrees de trace d3d6770_entries.inc.
#   13604 : D3D 2.0.20158, facade front_profile.cpp pilotee par profiles/dash13604.h (adresses retrouvees par tools/d3d_match.py).
set(NGPU_PROFILE "6770" CACHE STRING "Version du D3D invite : 6770 ou 13604")
if(NGPU_PROFILE STREQUAL "13604")
  set(NGPU_CORE ON)
  set(NGPU_PROFILE_FILE dash13604.h)
  set(NGPU_ENTRIES_FILE d3d13604_entries.inc)
  set(NGPU_SOURCES src/d3d_trace.cpp src/null_gpu.cpp src/vk_backend.cpp src/front_profile.cpp)
else()
  set(NGPU_PROFILE_FILE dash6770.h)
  set(NGPU_ENTRIES_FILE d3d6770_entries.inc)
  set(NGPU_SOURCES src/d3d_trace.cpp src/null_gpu.cpp src/vk_backend.cpp src/hooks_frame.cpp)
  if(NGPU_CORE)
    list(APPEND NGPU_SOURCES src/front6770.cpp)
  else()
    list(APPEND NGPU_SOURCES src/hooks_draw.cpp src/guest_tex.cpp)
  endif()
endif()
add_library(xenondash_native_gpu OBJECT ${NGPU_SOURCES})
target_include_directories(xenondash_native_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/profiles)
target_compile_definitions(xenondash_native_gpu PRIVATE NGPU_PROFILE_HEADER="${NGPU_PROFILE_FILE}" NGPU_ENTRIES_FILE="${NGPU_ENTRIES_FILE}")
find_package(Vulkan REQUIRED)
target_include_directories(xenondash_native_gpu PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(xenondash_native_gpu PUBLIC Vulkan::Vulkan)
target_include_directories(xenondash_native_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(xenondash_native_gpu PUBLIC cxx_std_23)
target_link_libraries(xenondash_native_gpu PUBLIC rex::runtime)
if(NGPU_CORE)
  if(NOT TARGET ngpu_core)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../ngpu_core ngpu_core)
  endif()
  target_link_libraries(xenondash_native_gpu PUBLIC ngpu_core)
  target_compile_definitions(xenondash_native_gpu PUBLIC NGPU_CORE_FRONT=1)
endif()
rexglue_apply_target_settings(xenondash_native_gpu)
