// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - profil de rendu de Skate (EA Black Box, 2007, XDK 2.0.5632) : ce qui etait en dur dans le renderer de
// Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51, src/native_gpu/renderer.cpp : kInterfaceVertexShaders
// et les cles de la minicarte). Ce sont des cles de microcode (XXH3), jamais du contenu du jeu.
// A terme : dans la recette games/<TitleID>/d3d_profile.toml (docs/ngpu_core.md).

#pragma once

#include "profile.h"

namespace native_gpu {

inline Profile Skate1Profile() {
  Profile profile;
  profile.interface_vertex_shaders = {
      0x9984423845937B08ull,  // simpledraw_p_sc_uv
      0xD61C8067CBAB6D32ull,  // simpledraw_p_n_sc
      0xF4C9E67C53263D9Dull,  // simpledraw_p_sc_uv
      0xBC3E27BBE36EEF1Aull,  // simpledraw_p_c_uv
      0xD949C2119E3C383Dull,  // simpledraw_p_n_sc_uv
      0x3EAE4C5DE646DF7Full,  // simpledraw_p_n_c_uv
      0x0A718F34D79ED591ull,  // minimap
      0xFE833B61BCE7FABCull,  // simpledraw_p_n_sc_uv
      0x0190382CCA5FEA26ull,  // simpledraw_p_n_c
      0x3654AF42C7D5A775ull,  // font
      0xD5D9BB34F6616C4Eull,  // simpledraw_p_c
      0x69CDC5D8FD2961B4ull,  // simpledraw_p_n_c_uv
      0x2F42BE8AE7BE99EAull,  // simpledraw_p_sc
      0xDED9E9293C2BE681ull,  // simpledraw_p_c_uv
      0x1CDF21FD205A4EAAull,  // simpledraw_p_sc
      0x26D45C70F251147Dull,  // font
      0x63D4FB864AF8378Full,  // simpledraw_p_c
      0x336B6CF2172DD2FFull,  // simpledraw_p_n_c
      0x7F31D2881987B4F3ull,  // minimap
      0x0FFD46E9EB7E10B6ull,  // simpledraw_p_n_sc
  };
  profile.corner_vertex_shaders = {0x0A718F34D79ED591ull, 0x7F31D2881987B4F3ull};  // minicarte
  profile.scene_pitch_min = 1024;
  profile.scene_pitch_max = 1280;
  return profile;
}

}  // namespace native_gpu
