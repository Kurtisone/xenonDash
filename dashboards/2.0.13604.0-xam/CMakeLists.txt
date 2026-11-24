# dash 2.0.13604.0 avec XAM 13604 recompilé (module ReXGlue, palier A) : projet de test, copie de 2.0.13604.0.
#
# Le boilerplate du SDK est dans generated/rexglue.cmake (écrit par `rexglue codegen`).

cmake_minimum_required(VERSION 3.25)
project(dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cohérence generated/ <-> entrées du codegen : tools/regen_xam_modules.py écrit generated/inputs.sha256 (empreinte du manifeste,
# des overrides et des XEX listés). Si les entrées ont changé depuis la génération, le code généré est périmé : on arrête le
# configure au lieu de compiler un mélange. Même calcul que inputs_digest() de l'outil (lignes « chemin:sha256 » triées).
# Contournement ponctuel : -DXD_SKIP_GENERATED_CHECK=ON. Régénération : python3 tools/regen_xam_modules.py.
if(NOT XD_SKIP_GENERATED_CHECK)
    set(_xd_stamp ${CMAKE_CURRENT_SOURCE_DIR}/generated/inputs.sha256)
    if(EXISTS ${_xd_stamp})
        file(READ ${CMAKE_CURRENT_SOURCE_DIR}/dash_manifest.toml _xd_man)
        string(REGEX MATCHALL "(^|\n)file_path = \"[^\"]+\"" _xd_fp "${_xd_man}")
        set(_xd_rels dash_manifest.toml ../2.0.13604.0/dash_overrides.toml ../2.0.13604.0/overrides.toml)
        foreach(_m ${_xd_fp})
            string(REGEX REPLACE "^\n?file_path = \"([^\"]+)\"" "\\1" _r "${_m}")
            list(APPEND _xd_rels "${_r}")
        endforeach()
        file(GLOB _xd_ov RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*overrides.toml)
        list(APPEND _xd_rels ${_xd_ov})
        list(REMOVE_DUPLICATES _xd_rels)
        list(SORT _xd_rels)
        set(_xd_text "")
        foreach(_r ${_xd_rels})
            if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_r})
                file(SHA256 ${CMAKE_CURRENT_SOURCE_DIR}/${_r} _h)
            else()
                set(_h absent)
            endif()
            string(APPEND _xd_text "${_r}:${_h}\n")
        endforeach()
        string(SHA256 _xd_now "${_xd_text}")
        file(STRINGS ${_xd_stamp} _xd_then LIMIT_COUNT 1)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_xd_stamp} ${CMAKE_CURRENT_SOURCE_DIR}/dash_manifest.toml)
        if(NOT _xd_now STREQUAL _xd_then)
            message(FATAL_ERROR "generated/ périmé : les entrées du codegen (manifeste, overrides, XEX) ont changé depuis la "
                                "dernière génération (empreinte ${_xd_then}, actuelle ${_xd_now}). Lancer "
                                "python3 tools/regen_xam_modules.py puis relancer le configure (ou -DXD_SKIP_GENERATED_CHECK=ON).")
        endif()
    else()
        message(WARNING "generated/inputs.sha256 absent : cohérence generated/ <-> entrées non vérifiée "
                        "(python3 tools/regen_xam_modules.py l'écrit).")
    endif()
endif()

# SDK : version EXACTE et SDK du dépôt (sdk/) uniquement, jamais celui du système ni un autre CMAKE_PREFIX_PATH. generated/rexglue.cmake
# est réécrit par le codegen (« DO NOT EDIT », find_package(rexglue 0.10.0 sans EXACT) : on impose donc ici, avant son inclusion,
# la version (REXSDK_VERSION -> find_package(... EXACT)) et le dossier du paquet (rexglue_DIR). Après une reconstruction du SDK
# dont le numéro change, mettre à jour XD_REXGLUE_VERSION (voir sdk/lib/cmake/rexglue/rexglueConfigVersion.cmake).
if(NOT REXSDK_DIR)
    set(XD_REXGLUE_VERSION "0.10.0.43")
    get_filename_component(_xd_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../../sdk" ABSOLUTE)
    set(REXSDK_VERSION "${XD_REXGLUE_VERSION}" CACHE STRING "Override SDK version (leave empty for default)" FORCE)
    set(CMAKE_PREFIX_PATH "${_xd_sdk}")
    set(rexglue_DIR "${_xd_sdk}/lib/cmake/rexglue" CACHE PATH "Paquet rexglue du dépôt" FORCE)
    set(CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH OFF)
    set(CMAKE_FIND_USE_PACKAGE_REGISTRY OFF)
    set(CMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY OFF)
endif()

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

# Modules XUI du HUD de XAM (palier N3) : vk.xex (clavier virtuel), ..., Guide.AvatarMiniCreator.xex (cible nommée par le codegen
# d'après le fichier : Guide_AvatarMiniCreator). Même procédé que dash_hud ; imports `xam.xex`
# produits par tools/gen_module_xam_imports.py dans src_xui/<nom>_xam_imports.cpp.
# Guide.AvatarMiniCreator.xex n'est pas versionné (fichier Microsoft) : sans lui, le module d'avatar n'est pas généré et XAM
# refuse la création d'avatar (XexLoadImage refusé : module non recompilé). On le signale au lieu de l'omettre en silence.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/assets/sep/Guide.AvatarMiniCreator.xex)
    message(WARNING "assets/sep/Guide.AvatarMiniCreator.xex absent : module d'avatar (dash_Guide_AvatarMiniCreator) non "
                    "construit, création d'avatar indisponible. Copier le fichier depuis la mise à jour (dossier $SystemUpdate) "
                    "puis relancer tools/regen_xam_modules.py.")
elseif(NOT TARGET dash_Guide_AvatarMiniCreator)
    message(WARNING "assets/sep/Guide.AvatarMiniCreator.xex présent mais module d'avatar non généré : relancer tools/regen_xam_modules.py")
endif()
foreach(xui_mod vk signin createprofile gamerprofile deviceselector ximecore Guide_AvatarMiniCreator)
    if(TARGET dash_${xui_mod})
        target_sources(dash_${xui_mod} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/generated/dash/${xui_mod}/dash_register.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/src_xui/${xui_mod}_xam_imports.cpp)
        target_include_directories(dash_${xui_mod} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../common/src)
    endif()
endforeach()

# Crochets D3D de XAM (palier N2) : définitions fortes des sub_<adresse> de la bibliothèque D3D liée dans xam.xex ;
# elles appellent hudd3d_call (exécutable : src/hud_d3d_trace.cpp, puis native_gpu/src/hud_front.cpp).
if(TARGET dash_xam)
    target_sources(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_d3d/xam_d3d_hooks.cpp)
    target_include_directories(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_d3d)
endif()

# Correctifs de comportement de XAM recompilé (palier N4) : définitions fortes de sub_<adresse> (même mécanisme que les crochets D3D).
if(TARGET dash_xam)
    target_sources(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_patch/xam_input_patch.cpp)
    # Diagnostic de la boucle d'image du HUD (DASH_HUD_LOOP_TRACE=1), sans effet sinon.
    target_sources(dash_xam PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src_xam_patch/xam_hud_trace.cpp)
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
# Le rendu natif est le seul chemin (le repli présentateur SDK / ImGui a été retiré).
set(NGPU_PROFILE 13604 CACHE STRING "Version du D3D invité (native_gpu/CMakeLists.txt)" FORCE)
set(NGPU_HUD ON CACHE BOOL "Facade du HUD de XAM" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../native_gpu xenondash_native_gpu)
target_link_libraries(dash PRIVATE xenondash_native_gpu)
target_compile_definitions(dash PRIVATE DASH_NATIVE_GPU=1)
# Le module XAM (DLL chargée par dlopen) appelle des fonctions de l'exécutable (accroches [[midasm_hook]] de
# xam_overrides.toml, src/xam_neutralize.cpp) : leurs symboles doivent figurer dans la table dynamique de l'exécutable.
set_target_properties(dash PROPERTIES ENABLE_EXPORTS ON)
rexglue_setup_target(dash)
