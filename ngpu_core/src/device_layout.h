// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - disposition de l'ombre du peripherique D3D, parametrable par profil (recompilateur generique, lot B2).
//
// Le noyau (renderer.cpp, xenos_decode.h) lit une ombre CANONIQUE : celle de la XDK 2.0.5632 de Skate. Les XDK suivantes gardent
// les memes blocs mais en deplacent certains (2.0.21256 : tampon d'indices et shaders courants 384 octets plus loin). Un profil
// decrit ou se trouvent les blocs dans le peripherique du jeu ; la facade en tire, a chaque appel, un instantane canonique
// (`Canonicalize`). Le noyau ne change pas : une facade dont le jeu a deja l'ombre canonique copie simplement le peripherique.
// Les valeurs d'un profil generique sont lues dans le code du jeu par tools/d3d_match.py (jamais une table par jeu).

#pragma once

#include <cstdint>
#include <cstring>

#include "xenos_decode.h"

namespace xenos {

struct DeviceLayout {
  // Blocs lus par le noyau
  uint32_t fetch_constants = device::kFetchConstants;            // 32 x 24 octets
  uint32_t vertex_constants = device::kVertexConstants;          // 256 float4
  uint32_t pixel_constants = device::kPixelConstants;            // 256 float4
  uint32_t vertex_bool_constants = device::kVertexBoolConstants;  // 4 mots
  uint32_t pixel_bool_constants = device::kPixelBoolConstants;    // 4 mots
  // Decalage de chaque groupe de registres (meme ordre et memes tailles que kRegisterGroups)
  uint32_t register_groups[7] = {10368, 10444, 10528, 10548, 10596, 10680, 10832};
  // Champs lus par la facade
  uint32_t index_buffer = device::kIndexBuffer;
  uint32_t pixel_shader = device::kPixelShader;
  uint32_t vertex_shader = device::kVertexShader;

  bool IsCanonical() const {
    const DeviceLayout canonical;
    return std::memcmp(this, &canonical, sizeof(*this)) == 0;
  }
};

// Taille d'un instantane canonique (jusqu'aux shaders courants compris).
constexpr uint32_t kCanonicalStateSize = 0x3400;

// Ecrit dans `out` (kCanonicalStateSize octets) l'instantane canonique du peripherique `device` (octets invites) decrit par `layout`.
inline void Canonicalize(const DeviceLayout& layout, const uint8_t* device, uint8_t* out) {
  if (layout.IsCanonical()) {
    std::memcpy(out, device, kCanonicalStateSize);
    return;
  }
  std::memset(out, 0, kCanonicalStateSize);
  std::memcpy(out + device::kFetchConstants, device + layout.fetch_constants, 32 * 24);
  std::memcpy(out + device::kVertexConstants, device + layout.vertex_constants, 256 * 16);
  std::memcpy(out + device::kPixelConstants, device + layout.pixel_constants, 256 * 16);
  std::memcpy(out + device::kVertexBoolConstants, device + layout.vertex_bool_constants, 16);
  std::memcpy(out + device::kPixelBoolConstants, device + layout.pixel_bool_constants, 16);
  for (uint32_t g = 0; g < 7; ++g) {
    std::memcpy(out + kRegisterGroups[g].offset, device + layout.register_groups[g], 4 * kRegisterGroups[g].count);
  }
  std::memcpy(out + device::kIndexBuffer, device + layout.index_buffer, 4);
  std::memcpy(out + device::kPixelShader, device + layout.pixel_shader, 4);
  std::memcpy(out + device::kVertexShader, device + layout.vertex_shader, 4);
}

}  // namespace xenos
