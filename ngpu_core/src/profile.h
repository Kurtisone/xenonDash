// SPDX-License-Identifier: GPL-3.0-or-later
// ngpu_core - ce que le noyau de rendu doit savoir d'un jeu (le « profil de rendu » d'une recette).
//
// Dans Skate1Native ces valeurs etaient des constantes du renderer (liste de vertex shaders d'interface, plage de pitch
// de la scene) ; elles sont ici des donnees fournies par le profil du jeu. Un profil vide convient a un jeu qui n'a
// ni image elargie ni echelle de rendu (Dashboard).

#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace native_gpu {

struct Profile {
  // Cles de microcode des vertex shaders de l'interface : leur place 16:9 est gardee sur une image plus large.
  std::vector<uint64_t> interface_vertex_shaders;
  // Parmi eux, ceux dont l'element vit dans le coin haut droit (minicarte) : ils suivent le bord de l'ecran.
  std::vector<uint64_t> corner_vertex_shaders;
  // Les surfaces dont le pitch est dans cette plage sont « la scene » : echelle de rendu et elargissement
  // (SetRenderScale / SetWidth) ne s'appliquent qu'a elles ; les passes plus petites gardent leur taille.
  uint32_t scene_pitch_min = 1024;
  uint32_t scene_pitch_max = 1280;

  bool IsInterface(uint64_t key) const {
    return std::find(interface_vertex_shaders.begin(), interface_vertex_shaders.end(), key) !=
           interface_vertex_shaders.end();
  }
  bool IsCornerWidget(uint64_t key) const {
    return std::find(corner_vertex_shaders.begin(), corner_vertex_shaders.end(), key) != corner_vertex_shaders.end();
  }
};

}  // namespace native_gpu
