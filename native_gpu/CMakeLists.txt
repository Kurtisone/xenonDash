# native_gpu : rendu natif au niveau D3D (remplace l'émulation du GPU Xenos). Voir docs/rendu_natif.md.
# Bibliothèque OBJECT (symboles forts qui remplacent les alias faibles `sub_XXXXXXXX` du code généré).
# Inclue seulement avec l'option DASH_NATIVE_GPU=ON du projet de la version (désactivée par défaut).

add_library(xenondash_native_gpu OBJECT
    src/d3d_trace.cpp
    src/null_gpu.cpp
    src/vk_backend.cpp
    src/hooks_frame.cpp
)
find_package(Vulkan REQUIRED)
target_include_directories(xenondash_native_gpu PRIVATE ${Vulkan_INCLUDE_DIRS})
target_link_libraries(xenondash_native_gpu PUBLIC Vulkan::Vulkan)
target_include_directories(xenondash_native_gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(xenondash_native_gpu PUBLIC cxx_std_23)
target_link_libraries(xenondash_native_gpu PUBLIC rex::runtime)
rexglue_apply_target_settings(xenondash_native_gpu)
