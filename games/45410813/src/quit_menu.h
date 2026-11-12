// SPDX-License-Identifier: GPL-3.0-or-later
// Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
// skate1 - quit entry in the main menu

#pragma once

#include <functional>

// `quit` runs when the player chooses the entry, on the game's thread.
void SetMainMenuQuitHandler(std::function<void()> quit);
