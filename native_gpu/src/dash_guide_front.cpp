// xenondash native_gpu - pont entre la lame Guide et la presentation.
//
// front_profile.cpp appelle GuideComposite() juste apres HudComposite(), sur le tampon
// R10G10B10A2 de l'image qui part a l'ecran. Ce fichier fait trois choses :
//
//   1. il lit la manette une fois par image (bouton Guide 0x0400, croix, A, B) ;
//   2. il tient l'etat de la lame (ouverte/fermee, selection, animation) ;
//   3. il la dessine dans le tampon, via dash::guide::Draw().
//
// Le module dash::guide ne connait ni Vulkan ni D3D : il ecrit des pixels. C'est ce qui rend la
// lame utilisable en rendu natif, ou ImGui n'est pas disponible.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include "dash_guide.h"
#include "dash_xam_hud_ui.h"
#include "ngpu.h"

namespace dash::guide {

// g_card_source (carte du joueur, remplie par dash_guide_actions.cpp) est défini dans common/src/dash_guide.cpp : les
// variantes sans rendu natif (6770, 17559...) lient common sans cette bibliothèque.

// Traite une action du volet. Implemente dans dash_guide_actions.cpp.
void PerformAction(int action);

namespace {

State g_state;
std::vector<Item> g_items;
bool g_items_built = false;

double NowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Front montant, une instance par bouton.
struct Edge {
  bool prev = false;
  bool Rising(bool now) {
    const bool r = now && !prev;
    prev = now;
    return r;
  }
};
Edge e_guide, e_up, e_down, e_accept, e_back;

struct Buttons {
  bool guide = false, up = false, down = false, accept = false, back = false;
};

// GetStateForUI : lecture cote hote, sans toucher au suivi des fronts du code invite.
Buttons ReadButtons() {
  Buttons b;
  auto* rt = rex::Runtime::instance();
  if (!rt) return b;
  auto* is = static_cast<rex::input::InputSystem*>(rt->input_system());
  if (!is) return b;
  rex::input::X_INPUT_STATE st{};
  if (is->GetStateForUI(0, &st) != 0) return b;
  const uint32_t btn = uint32_t(uint16_t(st.gamepad.buttons));
  b.guide = (btn & 0x0400u) != 0;   // X_INPUT_GAMEPAD_GUIDE
  b.up = (btn & 0x0001u) != 0;      // DPAD_UP
  b.down = (btn & 0x0002u) != 0;    // DPAD_DOWN
  b.accept = (btn & 0x1000u) != 0;  // A
  b.back = (btn & 0x2000u) != 0;    // B
  return b;
}

// La liste depend de l'existence d'un profil : on la construit a l'ouverture.
void BuildItems(const PlayerCard& card) {
  g_items.clear();
  if (!card.has_profile) g_items.push_back({"Creer un profil", kActionProfile, true});
  g_items.push_back({"Succes", kActionAchievements, true});
  g_items.push_back({"Retour au tableau de bord", kActionHome, true});
  g_items.push_back({"Fermer le Guide", kActionClose, true});
  g_items_built = true;
}

}  // namespace

bool GuideFrame(uint32_t* pixels, uint32_t w, uint32_t h) {
  static double last = NowSeconds();
  const double now = NowSeconds();
  const float dt = float(std::min(0.1, now - last));
  last = now;

  const Buttons b = ReadButtons();
  // DASH_GUIDE_OPEN=1 : ouvre la lame au demarrage (diagnostic et captures). Sans cette variable,
  // seule la pression du bouton Guide l'ouvre.
  static const bool auto_open = std::getenv("DASH_GUIDE_OPEN") != nullptr;
  if (auto_open && !g_items_built && !g_state.open) Toggle(g_state);
  // Le bouton Guide ouvre le Guide DE LA CONSOLE (XamShowGuideUI de XAM recompilé), pas la lame maison.
  // La lame reste atteignable par DASH_GUIDE_LEGACY=1 (diagnostic), jamais par le bouton.
  static const bool legacy = std::getenv("DASH_GUIDE_LEGACY") != nullptr;
  // DASH_TEST_GUIDE=<secondes> : déclenche le Guide depuis l'hôte après ce délai, sans manette.
  // Sert à vérifier le branchement (XamShowGuideUI) sur une machine sans bouton Guide.
  static const double test_after = [] {
    const char* e = std::getenv("DASH_TEST_GUIDE");
    return e && *e ? std::atof(e) : -1.0;
  }();
  static bool test_fired = false;
  if (!test_fired && test_after >= 0.0 && now >= test_after) {
    test_fired = true;
    REXLOG_INFO("[guide] DASH_TEST_GUIDE : déclenchement après {:.1f} s", test_after);
    if (!legacy) dash::hudui::RequestGuide();
  }
  if (e_guide.Rising(b.guide)) {
    if (legacy) {
      Toggle(g_state);
      REXLOG_INFO("[guide] lame (mode diagnostic) {}", g_state.open ? "ouverte" : "fermee");
    } else {
      dash::hudui::RequestGuide();
    }
  }

  PlayerCard card{};
  if (g_card_source) g_card_source(&card);

  if (g_state.open) {
    if (!g_items_built) BuildItems(card);
    if (e_up.Rising(b.up)) MoveSelection(g_state, -1, g_items);
    if (e_down.Rising(b.down)) MoveSelection(g_state, +1, g_items);
    if (e_back.Rising(b.back)) Close(g_state);
    if (e_accept.Rising(b.accept)) {
      const Item* it = SelectedItem(g_state, g_items);
      if (it) {
        const int action = it->action;
        if (action == kActionClose) {
          Close(g_state);
        } else {
          Close(g_state);  // la lame se referme, puis l'action s'execute
          PerformAction(action);
        }
        g_items_built = false;
      }
    }
  } else {
    g_items_built = false;
  }

  return Draw(g_state, pixels, w, h, dt, card, g_items);
}

}  // namespace dash::guide

namespace ngpu {

bool GuideComposite(uint32_t* pixels, uint32_t w, uint32_t h) __attribute__((weak));

bool GuideComposite(uint32_t* pixels, uint32_t w, uint32_t h) {
  return dash::guide::GuideFrame(pixels, w, h);
}

}  // namespace ngpu
