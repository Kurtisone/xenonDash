// dash - fenêtres du HUD de XAM appelées depuis l'hôte (palier N5, docs/xam_recompile.md).
//
// Le rendu est natif : le présentateur SDK est détaché, aucune superposition ImGui (les fenêtres ImGui de
// dash_xam_ui.cpp ont été retirées). Ce fichier fournit les vraies fenêtres Xbox du HUD de XAM (boîte de message, palier N2 : rendue nativement par-dessus le
// hub, entrées du Dashboard suspendues pendant l'affichage) :
// - progression de construction d'un jeu (dash_launch.cpp) : boîte au démarrage, boîte à l'échec ; au succès le jeu se
//   lance tout seul (BuildMonitor), rien à afficher.
// Sans XAM recompilé (DASH_XAM_MODULE absent), Init ne fait rien.

#pragma once

#include <cstdint>

namespace rex::system {
class KernelState;
}

namespace dash::hudui {

// À appeler depuis OnPostSetup. Sans effet si DASH_XAM_MODULE n'est pas défini.
void Init(rex::system::KernelState* ks);

// Ouvre le Guide de la console : appelle XamShowGuideUI de XAM recompilé depuis un thread invité, comme les
// boîtes de message. C'est ce que fait le bouton Guide (0x0400) depuis que la lame maison a été retirée.
// Peut être appelé depuis n'importe quel fil (le rendu) : la demande est déposée et servie par le thread
// invité du HUD. Sans effet (et journalisé) si XAM n'est pas chargé. `user` : utilisateur (emplacement de manette 0 à 3) dont le
// bouton Guide a été pressé, comme XamShowGuideUI(dwUserIndex) sur la console.
void RequestGuide(uint32_t user = 0);

}  // namespace dash::hudui
