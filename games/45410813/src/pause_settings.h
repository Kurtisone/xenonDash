// SPDX-License-Identifier: GPL-3.0-or-later
// Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
// skate1 - video settings in the game's options

#pragma once

#include <functional>

#include <rex/rex_app.h>

// The settings to offer, read each time the menu needs them.
void SetPauseSettingsSource(std::function<rex::ReXApp::VideoSettings()> source);
// Follows the game's language, from its menus' thread.
void UpdateSettingsLanguage(uint8_t* base);
