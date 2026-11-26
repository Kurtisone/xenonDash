// xenondash common - plan overlay du contrôleur d'affichage (registres AVIVO D1OVL), suivi dans le tampon de commandes système de XAM.
#pragma once

#include <cstdint>

namespace dash::vd {

// Plan overlay matériel tel que XAM l'a programmé (D1OVL_ENABLE, SURFACE_ADDRESS, PITCH, START, END). XAM y affiche ses
// notifications (toast « a signed out »...) : une petite surface résolue, montrée telle quelle à une fenêtre du bureau, par-dessus
// l'image du titre. Coordonnées en pixels du bureau (le tampon d'affichage : display_width x display_height du mode vidéo).
struct DisplayOverlay {
  bool enabled = false;
  uint32_t surface = 0;  // adresse physique de la surface (D1OVL_SURFACE_ADDRESS)
  uint32_t pitch = 0;    // en pixels
  int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // fenêtre [x0, x1[ x [y0, y1[ (D1OVL_START, D1OVL_END)
};

// État courant (copie), lisible depuis n'importe quel fil.
DisplayOverlay GetDisplayOverlay();

}  // namespace dash::vd
