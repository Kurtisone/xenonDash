# SPDX-License-Identifier: GPL-3.0-or-later
# ngpu_replay : rejeu hors ligne d'une capture d'image (.sk1c) de Skate avec le noyau (reprise de tools/native_replay de Skate1Native).
# Demande le cache de shaders de Skate, genere a partir des fichiers du jeu (profiles/skate1/shader_cache.h) :
#   cmake -S ngpu_core -B out/ngpu_replay -DNGPU_BUILD_REPLAY=ON -DNGPU_SKATE_SHADER_CACHE=<.../shader_cache.cpp>
# Sans fichier de cache fourni, un cache vide est compile (verifie la compilation seulement).
set(NGPU_SKATE_SHADER_CACHE "" CACHE FILEPATH "Cache de shaders XenosRecomp de Skate (shader_cache.cpp)")
if(NGPU_SKATE_SHADER_CACHE)
  set(_cache "${NGPU_SKATE_SHADER_CACHE}")
else()
  set(_cache "${CMAKE_CURRENT_BINARY_DIR}/empty_shader_cache.cpp")
  file(WRITE "${_cache}" "#include \"shader_cache.h\"\nShaderCacheEntry g_shaderCacheEntries[1] = {};\nconst size_t g_shaderCacheEntryCount = 0;\nconst uint8_t g_compressedDxilCache[1] = {0};\nconst size_t g_dxilCacheCompressedSize = 0, g_dxilCacheDecompressedSize = 0;\nconst uint8_t g_compressedSpirvCache[1] = {0};\nconst size_t g_spirvCacheCompressedSize = 0, g_spirvCacheDecompressedSize = 0;\n")
endif()
find_library(ZSTD_LIBRARY zstd)
add_executable(ngpu_replay replay.cpp ${_cache} ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/smol-v/smolv.cpp)
target_include_directories(ngpu_replay PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../profiles ${CMAKE_CURRENT_SOURCE_DIR}/../../profiles/skate1
  ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/smol-v)
target_link_libraries(ngpu_replay PRIVATE ngpu_core)
if(ZSTD_LIBRARY)
  target_link_libraries(ngpu_replay PRIVATE ${ZSTD_LIBRARY})
  target_compile_definitions(ngpu_replay PRIVATE NATIVE_GPU_ZSTD=1)
endif()
