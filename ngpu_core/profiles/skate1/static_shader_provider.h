// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - source de shaders de Skate : le cache XenosRecomp compile dans l'executable (tables de shader_cache.h,
// produites par build_shader_cache.sh a partir des fichiers du jeu), SPIR-V encode smol-v, eventuellement zstd.
// Reprend ce que renderer.cpp de Skate1Native (commit 81805f51) faisait dans Initialize et GetShader.

#pragma once

#include <algorithm>
#include <vector>

#include <smolv.h>
#ifdef NATIVE_GPU_ZSTD
#include <zstd.h>
#endif

#include "shader_cache.h"
#include "shader_provider.h"

namespace native_gpu {

class StaticShaderProvider : public ShaderProvider {
 public:
  bool Initialize() {
    if (g_spirvCacheCompressedSize == g_spirvCacheDecompressedSize) {
      spirv_.assign(g_compressedSpirvCache, g_compressedSpirvCache + g_spirvCacheCompressedSize);
      return true;
    }
#ifdef NATIVE_GPU_ZSTD
    spirv_.resize(g_spirvCacheDecompressedSize);
    return ZSTD_decompress(spirv_.data(), spirv_.size(), g_compressedSpirvCache, g_spirvCacheCompressedSize) ==
           spirv_.size();
#else
    return false;
#endif
  }
  bool Find(uint64_t key, std::vector<uint8_t>& spirv) override {
    const ShaderCacheEntry* begin = g_shaderCacheEntries;
    const ShaderCacheEntry* end = begin + g_shaderCacheEntryCount;
    const ShaderCacheEntry* entry = std::lower_bound(
        begin, end, key, [](const ShaderCacheEntry& lhs, uint64_t rhs) { return lhs.hash < rhs; });
    if (entry == end || entry->hash != key) return false;
    // The cache's SPIR-V is smol-v encoded.
    const uint8_t* encoded = &spirv_[entry->spirvOffset];
    spirv.resize(smolv::GetDecodedBufferSize(encoded, entry->spirvSize));
    smolv::Decode(encoded, entry->spirvSize, spirv.data(), spirv.size());
    return true;
  }

 private:
  std::vector<uint8_t> spirv_;
};

}  // namespace native_gpu
