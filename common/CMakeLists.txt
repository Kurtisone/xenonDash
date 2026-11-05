# xenondash_common : code partagé par tous les Dashboards (profil, GPD/succès, entrées, audio silencieux,
# interfaces XAM ImGui, montage de la flash, stubs noyau).
#
# Bibliothèque OBJECT (et non STATIC) : les REX_HOOK(__imp__Xxx, ...) remplacent des fonctions du SDK à
# l'édition de liens, sans être référencés ; dans une archive statique le linker écarterait ces objets.
# Les fichiers objets sont donc ajoutés directement à l'exécutable de chaque version.

add_library(xenondash_common OBJECT
    src/dash_app_base.cpp
    src/dash_profile.cpp
    src/dash_achievements.cpp
    src/dash_content.cpp
    src/dash_xam_ui.cpp
    src/kernel_stubs.cpp
)
target_include_directories(xenondash_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(xenondash_common PUBLIC cxx_std_23)
target_link_libraries(xenondash_common PUBLIC rex::runtime)
rexglue_apply_target_settings(xenondash_common)
