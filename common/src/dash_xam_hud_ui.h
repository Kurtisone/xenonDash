// dash - fenêtres du HUD de XAM appelées depuis l'hôte (palier N5, docs/xam_recompile.md).
//
// Avec le rendu natif (-DDASH_NATIVE_GPU=ON) le présentateur SDK est détaché : aucune superposition ImGui
// (docs/ngpu_core.md, « Non fait »), donc les fenêtres de dash_xam_ui.cpp ne sont plus dessinées. Ce fichier les
// remplace par les vraies fenêtres Xbox du HUD de XAM (boîte de message, palier N2 : rendue nativement par-dessus le
// hub, entrées du Dashboard suspendues pendant l'affichage) :
// - progression de construction d'un jeu (dash_launch.cpp) : boîte au démarrage, boîte à l'échec ; au succès le jeu se
//   lance tout seul (BuildMonitor), rien à afficher.
// Sans XAM recompilé (DASH_XAM_MODULE absent : builds avec plugin xenos), Init ne fait rien — ces builds gardent leurs
// fenêtres ImGui (dash_xam_ui.cpp).

#pragma once

namespace rex::system {
class KernelState;
}

namespace dash::hudui {

// À appeler depuis OnPostSetup. `imgui_available` : imgui_drawer() du présentateur SDK. Sans effet si DASH_XAM_MODULE n'est pas défini.
void Init(rex::system::KernelState* ks, bool imgui_available);

// Ouvre le Guide de la console : appelle XamShowGuideUI de XAM recompilé depuis un thread invité, comme les
// boîtes de message. C'est ce que fait le bouton Guide (0x0400) depuis que la lame maison a été retirée.
// Peut être appelé depuis n'importe quel fil (le rendu) : la demande est déposée et servie par le thread
// invité du HUD. Sans effet (et journalisé) si XAM n'est pas chargé.
void RequestGuide();

}  // namespace dash::hudui
