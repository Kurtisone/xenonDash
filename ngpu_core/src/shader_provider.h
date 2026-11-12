// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - source des shaders recompiles (XenosRecomp) pour le noyau de rendu.
//
// Le noyau retrouve un shader par la cle de microcode (XXH3-64 du microcode du shader, cle de cache de
// XenosRecomp avec XENOS_RECOMP_MICROCODE_KEY=1) et attend du SPIR-V non compresse. D'ou il vient depend du
// jeu : tables compilees dans l'executable (Skate, profiles/skate1), fichiers lus au disque (Dashboard 6770).
// Piece nouvelle de XenonDash (le code de Skate1Native lisait des tables generees, g_shaderCacheEntries).

#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace native_gpu {

class ShaderProvider {
 public:
  virtual ~ShaderProvider() = default;
  // Le SPIR-V (non compresse, mots little-endian) du shader de cle `key` ; faux s'il est absent.
  virtual bool Find(uint64_t key, std::vector<uint8_t>& spirv) = 0;
};

// Cache en memoire : des blobs ajoutes d'avance ou des fichiers lus a la premiere demande.
class MapShaderProvider : public ShaderProvider {
 public:
  void AddBlob(uint64_t key, std::vector<uint8_t> spirv) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = {std::move(spirv), std::string()};
  }
  void AddFile(uint64_t key, std::string path) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[key] = {{}, std::move(path)};
  }
  bool Find(uint64_t key, std::vector<uint8_t>& spirv) override {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) return false;
    Entry& entry = it->second;
    if (entry.blob.empty() && !entry.path.empty()) {
      if (FILE* f = std::fopen(entry.path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0) {
          entry.blob.resize(size_t(size));
          if (std::fread(entry.blob.data(), 1, entry.blob.size(), f) != entry.blob.size()) entry.blob.clear();
        }
        std::fclose(f);
      }
    }
    if (entry.blob.empty()) return false;
    spirv = entry.blob;
    return true;
  }

 private:
  struct Entry {
    std::vector<uint8_t> blob;
    std::string path;
  };
  std::mutex mutex_;
  std::unordered_map<uint64_t, Entry> entries_;
};

}  // namespace native_gpu
