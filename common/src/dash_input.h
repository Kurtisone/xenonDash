// dash - clavier → manette adapté au Dashboard.
//
// Le pilote "mnk" du SDK est réglé pour un jeu de tir (ZQSD = stick gauche, flèches = stick
// droit, Maj+flèches = croix). Le Dashboard se pilote surtout à la croix et avec A/B :
// on active le pilote par défaut et on remplace chaque raccourci QUI A ENCORE SA VALEUR
// D'ORIGINE (une valeur passée en ligne de commande, --keybind_a=..., est respectée).
//
// Les noms de touches désignent des positions physiques QWERTY (scancodes) : sur un clavier
// AZERTY, "Q" est la touche A, "W" la touche Z, etc.

#pragma once

#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, mnk_mode);
REXCVAR_DECLARE(std::string, keybind_a);
REXCVAR_DECLARE(std::string, keybind_b);
REXCVAR_DECLARE(std::string, keybind_x);
REXCVAR_DECLARE(std::string, keybind_y);
REXCVAR_DECLARE(std::string, keybind_left_shoulder);
REXCVAR_DECLARE(std::string, keybind_right_shoulder);
REXCVAR_DECLARE(std::string, keybind_left_trigger);
REXCVAR_DECLARE(std::string, keybind_right_trigger);
REXCVAR_DECLARE(std::string, keybind_dpad_up);
REXCVAR_DECLARE(std::string, keybind_dpad_down);
REXCVAR_DECLARE(std::string, keybind_dpad_left);
REXCVAR_DECLARE(std::string, keybind_dpad_right);
REXCVAR_DECLARE(std::string, keybind_rstick_up);
REXCVAR_DECLARE(std::string, keybind_rstick_down);
REXCVAR_DECLARE(std::string, keybind_rstick_left);
REXCVAR_DECLARE(std::string, keybind_rstick_right);
REXCVAR_DECLARE(std::string, keybind_back);
REXCVAR_DECLARE(std::string, keybind_start);

namespace dash::input {

inline void Rebind(std::string& bind, const char* sdk_default, const char* dash_value) {
  if (bind == sdk_default) bind = dash_value;
}

// Disposition Dashboard (positions QWERTY) :
//   flèches = croix            Entrée / Espace = A      Échap / Retour arrière = B
//   X = X    Y = Y             Q / E = LB / RB (changer d'onglet)
//   1 / 3 = LT / RT            Tab = Back               P = Start
//   WASD = stick gauche        Maj+flèches = stick droit
inline void ApplyDashKeyboardLayout() {
  REXCVAR_SET(mnk_mode, true);
  Rebind(REXCVAR_GET(keybind_dpad_up), "Shift+Up", "Up");
  Rebind(REXCVAR_GET(keybind_dpad_down), "Shift+Down", "Down");
  Rebind(REXCVAR_GET(keybind_dpad_left), "Shift+Left", "Left");
  Rebind(REXCVAR_GET(keybind_dpad_right), "Shift+Right", "Right");
  Rebind(REXCVAR_GET(keybind_rstick_up), "Up", "Shift+Up");
  Rebind(REXCVAR_GET(keybind_rstick_down), "Down", "Shift+Down");
  Rebind(REXCVAR_GET(keybind_rstick_left), "Left", "Shift+Left");
  Rebind(REXCVAR_GET(keybind_rstick_right), "Right", "Shift+Right");
  Rebind(REXCVAR_GET(keybind_a), "Semicolon,Space", "Return,Space");
  Rebind(REXCVAR_GET(keybind_b), "Quote,Backspace", "Escape,Backspace");
  Rebind(REXCVAR_GET(keybind_x), "L", "X");
  Rebind(REXCVAR_GET(keybind_y), "P", "Y");
  Rebind(REXCVAR_GET(keybind_left_shoulder), "1", "Q");
  Rebind(REXCVAR_GET(keybind_right_shoulder), "3", "E");
  Rebind(REXCVAR_GET(keybind_left_trigger), "Q,I", "1");
  Rebind(REXCVAR_GET(keybind_right_trigger), "E,O", "3");
  Rebind(REXCVAR_GET(keybind_back), "Z,Tab", "Tab");
  Rebind(REXCVAR_GET(keybind_start), "X,Return", "P");
}

}  // namespace dash::input
