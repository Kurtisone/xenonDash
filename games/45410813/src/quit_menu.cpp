// SPDX-License-Identifier: GPL-3.0-or-later
// Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
// skate1 - quit entry in the main menu
//
// The console game leaves through the dashboard, so its main menu has no way
// out. main_menu_quit adds one, with the game files untouched: mainmenu.apt is
// patched in memory once loaded, before its script runs.
//
// The script fills the list with mMenuPicker.AddText(id) calls, at its first run
// and in mainmenu::SetState, each with a branch for the demo build that is never run. Code cannot move (the script holds
// relocated pointers), so each demo branch's two calls give the room: the
// OPTIONS call becomes a jump there, the branch adds OPTIONS and EXTRAS, and its
// own jump comes back to where EXTRAS was, which now adds the demo's "exit"
// entry, translated in the game's languages. The list shows 9 items instead of 8.
//
// The picker reports a choice with Game.OnMenuSelect(controller, index, id).
// Choosing the entry closes the window like the quit dialog, the shutdown that
// cannot hang: XamLoaderTerminateTitle leaves a process without a window behind.

#include "quit_menu.h"

#include <cstring>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/skate1_init.h"
#include "pause_settings.h"

REXCVAR_DEFINE_BOOL(main_menu_quit, true, "Skate1", "Add a quit entry to the main menu");

namespace {

std::function<void()> g_quit;

constexpr char kQuitId[] = "ID_DEMO_MAIN_MENU_EXIT_DEMO";
// Screen load entry: the name and the loaded script.
constexpr uint32_t kEntryName = 264, kEntryApt = 528;
// mainmenu.apt: the same list is built by the script's first run and by SetState.
struct MenuList {
  uint32_t demo_block, demo_jump_target, options, extras;
  uint8_t to_extras;     // jump from demo_jump_target + 4
  uint8_t to_demo_block; // jump from extras
};
constexpr MenuList kLists[] = {{0xD1A30, 0xD1A40, 0xD1A99, 0xD1AA0, 0x5C, 0x90},
                               {0xD1F0F, 0xD1F20, 0xD1F7D, 0xD1F84, 0x60, 0x8B}};
constexpr uint32_t kNumVisibleItems = 0xD2D80;  // push byte 8, in the picker's setup

constexpr uint8_t kOptionsExtras[14] = {0xA2, 0x1E, 0x5A, 0xAE, 0x11, 0xB2, 0x12,
                                        0xA2, 0x1F, 0x5A, 0xAE, 0x11, 0xB2, 0x12};
constexpr uint8_t kDemoEntries[14] = {0xA2, 0x10, 0x5A, 0xAE, 0x11, 0xB2, 0x12,
                                      0xA2, 0x13, 0x5A, 0xAE, 0x11, 0xB2, 0x12};

void PatchMainMenu(uint8_t* apt) {
  // Another version of the game, or a mod: leave the menu alone.
  bool expected = apt[kNumVisibleItems] == 0xB5 && apt[kNumVisibleItems + 1] == 8;
  for (const MenuList& list : kLists) {
    expected = expected && !std::memcmp(apt + list.demo_block, kDemoEntries, 14) &&
               !std::memcmp(apt + list.options, kOptionsExtras, 14) && apt[list.demo_jump_target + 2] == 0;
  }
  if (!expected) {
    REXLOG_WARN("main_menu_quit: mainmenu.apt is not the expected one, no quit entry");
    return;
  }
  for (const MenuList& list : kLists) {
    const uint8_t to_extras[4] = {0, 0, 0, list.to_extras};
    const uint8_t to_demo_block[7] = {0x99, 0, 0, 0xFF, 0xFF, 0xFF, list.to_demo_block};
    std::memcpy(apt + list.demo_block, kOptionsExtras, 14);
    std::memcpy(apt + list.demo_jump_target, to_extras, 4);
    std::memcpy(apt + list.options, to_demo_block, 7);
    std::memcpy(apt + list.extras, kDemoEntries + 7, 7);
  }
  apt[kNumVisibleItems + 1] = 9;
  REXLOG_INFO("main_menu_quit: quit entry added");
}

}  // namespace

void SetMainMenuQuitHandler(std::function<void()> quit) { g_quit = std::move(quit); }

// Runs once a screen's files are loaded, before its script.
REX_HOOK_RAW(sub_82426FB0) {
  UpdateSettingsLanguage(base);
  const uint32_t entry = ctx.r4.u32;
  if (REXCVAR_GET(main_menu_quit) &&
      !std::strcmp(reinterpret_cast<const char*>(base + entry + kEntryName), "mainmenu")) {
    const uint32_t apt = REX_LOAD_U32(entry + kEntryApt);
    if (apt) PatchMainMenu(base + apt);
  }
  __imp__sub_82426FB0(ctx, base);
}

// Game.OnMenuSelect
REX_HOOK_RAW(sub_824359E0) {
  if (REXCVAR_GET(main_menu_quit) && g_quit) {
    PPCContext id = ctx;
    id.r3.u64 = 2;
    __imp__sub_8242E500(id, base);  // the third argument, as a string object
    const uint32_t text = REX_LOAD_U32(id.r3.u32);
    if (text && !std::strcmp(reinterpret_cast<const char*>(base + text), kQuitId)) {
      REXLOG_INFO("main_menu_quit: leaving the game");
      g_quit();
      return;
    }
  }
  __imp__sub_824359E0(ctx, base);
}
