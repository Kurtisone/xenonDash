# avatar 2.0.13604.0 : AvatarEditor.xex (titre 584D07D1) comme second exécutable du Dashboard Kinect. Même runtime (common/), même
# rendu natif (native_gpu/ profil avatar13604, même D3D que dash.xex) et mêmes modules XAM/HUD recompilés que dashboards/2.0.13604.0-xam
# (bibliothèques libdash_*.so posées à côté de l'exécutable, voir src/module_registry.cpp). Prototype de phase 1.
#
# Le boilerplate du SDK est dans generated/rexglue.cmake (écrit par `rexglue codegen avatar_manifest.toml`).
cmake_minimum_required(VERSION 3.25)
project(avatar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# SDK du dépôt (sdk/) uniquement, comme dashboards/2.0.13604.0-xam/CMakeLists.txt.
if(NOT REXSDK_DIR)
    get_filename_component(_xd_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" ABSOLUTE)
    set(_xd_ver_file "${_xd_sdk}/lib/cmake/rexglue/rexglueConfigVersion.cmake")
    if(NOT EXISTS "${_xd_ver_file}")
        message(FATAL_ERROR "SDK rexglue introuvable : ${_xd_ver_file} (installer le SDK dans sdk/, voir docs/compilation.md)")
    endif()
    file(STRINGS "${_xd_ver_file}" _xd_ver_line REGEX "^set\\(PACKAGE_VERSION \"[^\"]+\"\\)")
    if(NOT _xd_ver_line MATCHES "PACKAGE_VERSION \"([^\"]+)\"")
        message(FATAL_ERROR "Version du SDK illisible dans ${_xd_ver_file}")
    endif()
    set(REXSDK_VERSION "${CMAKE_MATCH_1}" CACHE STRING "Override SDK version (leave empty for default)" FORCE)
    set(CMAKE_PREFIX_PATH "${_xd_sdk}")
    set(rexglue_DIR "${_xd_sdk}/lib/cmake/rexglue" CACHE PATH "Paquet rexglue du dépôt" FORCE)
    set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
endif()

include(generated/rexglue.cmake)

# Le registre des modules est celui du Dashboard (src/module_registry.cpp), pas celui du codegen (vide : aucun [[modules]]).
list(FILTER REXGLUE_ENTRYPOINT_GENERATED_SOURCES EXCLUDE REGEX "module_registry\\.cpp$")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common xenondash_common)

# Sources partagées avec le Dashboard (sans rapport avec dash.xex : noyau vidéo, accroches de XAM, trace du HUD) ; pas dash_font_patch.cpp.
set(DASH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../2.0.13604.0-xam/src)
add_executable(avatar
    src/main.cpp src/module_registry.cpp src/avatar_session.cpp
    ${DASH_SRC}/kernel_video_output.cpp ${DASH_SRC}/hud_d3d_trace.cpp ${DASH_SRC}/xam_neutralize.cpp)
target_include_directories(avatar PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(avatar PRIVATE xenondash_common)

set(NGPU_PROFILE avatar13604 CACHE STRING "Version du D3D invité (native_gpu/CMakeLists.txt)" FORCE)
set(NGPU_HUD ON CACHE BOOL "Facade du HUD de XAM" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../native_gpu xenondash_native_gpu)
target_link_libraries(avatar PRIVATE xenondash_native_gpu)
target_compile_definitions(avatar PRIVATE DASH_NATIVE_GPU=1)
# Les modules (dlopen) appellent des fonctions de l'exécutable (hudd3d_call, xam_skip_device_auth...).
set_target_properties(avatar PROPERTIES ENABLE_EXPORTS ON)
rexglue_setup_target(avatar)
