// SPDX-License-Identifier: GPL-3.0-or-later
// Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
// skate1 - video settings in the game's options
//
// The options menus ask the game for their entries: a count, then each text, for
// the page in object + 60. The options list gets a first entry, first because the
// script keeps its selection when it comes back, which opens a page with the
// settings of the RB+Start menu, changed with left and right like the game's. The
// pause menu and the main menu share the entries' code, not their input handlers.
// A text starting with '#' is shown as it is. Nothing in the game files changes.

#include "pause_settings.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>

#include "generated/default/skate1_init.h"

namespace {

constexpr uint32_t kPage = 60, kSelected = 64, kScreen = 8;
constexpr uint32_t kPauseMenu = 88, kMainOptions = 316;  // in the menus' manager
constexpr uint32_t kOptionsPage = 1;
constexpr uint32_t kVideoPage = 2;  // the audio page, which nothing else treats apart
constexpr int kBack = 3, kLeft = 14, kRight = 15;
constexpr uint32_t kBackSound = 112;
constexpr uint32_t kLanguage = 0x83096918;       // the game's language id
constexpr uint32_t kLanguages = 0x830A2928;      // 5 entries of 48 bytes, the id at +4
constexpr uint32_t kFrench = 1;                  // English, French, ...
constexpr uint32_t kTextBuffer = 0x831BA8E8;     // where the game formats its entries
constexpr uint32_t kTextBufferSize = 128;
constexpr uint32_t kScreenManager = 0x831AB1C8;  // the first argument to reload a page

std::function<rex::ReXApp::VideoSettings()> g_source;
bool g_video_page = false;

struct Row {
  std::string text;
  std::function<void(int)> change;
};

template <typename... Args>
std::string Format(const char* format, Args... args) {
  char text[kTextBufferSize];
  std::snprintf(text, sizeof(text), format, args...);
  return text;
}

void AddChoice(std::vector<Row>& rows, rex::ui::QuitDialog::FrameRate choice) {
  if (choice.rates.empty()) return;
  rows.push_back({Format(choice.format, choice.current), [choice](int direction) {
                    const int count = int(choice.rates.size());
                    int index = 0;
                    for (int i = 0; i < count; ++i) {
                      if (choice.rates[i] == choice.current) index = i;
                    }
                    choice.set(choice.rates[(index + direction + count) % count]);
                  }});
}

std::vector<Row> Rows() {
  std::vector<Row> rows;
  if (!g_source) return rows;
  auto settings = g_source();
  AddChoice(rows, std::move(settings.frame_rate));
  AddChoice(rows, std::move(settings.resolution));
  AddChoice(rows, std::move(settings.aspect));
  if (settings.vsync.set) {
    const auto vsync = std::move(settings.vsync);
    rows.push_back({vsync.enabled ? rex::ui::QuitDialog::Text("VSync: On", "VSync : Oui")
                                  : rex::ui::QuitDialog::Text("VSync: Off", "VSync : Non"),
                    [vsync](int) { vsync.set(!vsync.enabled); }});
  }
  return rows;
}

// The menu's font: capitals, no accents. '#' marks a text that is not a string id.
std::string MenuText(const std::string& text) {
  std::string menu = "#";
  for (size_t i = 0; i < text.size(); ++i) {
    if (!text.compare(i, 2, "\xC3\xA9")) {  // e acute
      menu += 'E';
      ++i;
    } else {
      menu += char(std::toupper(uint8_t(text[i])));
    }
  }
  return menu;
}

const char* VideoTitle() {
  return rex::ui::QuitDialog::Text("VIDEO SETTINGS", "PARAMETRES VIDEO");
}

// The script stays on the list: its other pages have a life of their own.
void ReloadPage(PPCContext& ctx, uint8_t* base, uint32_t object) {
  ctx.r3.u64 = REX_LOAD_U32(kScreenManager);
  ctx.r4.u64 = REX_LOAD_U32(object + kScreen);
  ctx.r5.u64 = kOptionsPage;
  __imp__sub_82464F40(ctx, base);
}

void OpenVideoPage(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  g_video_page = true;
  REX_STORE_U32(object + kSelected, 0);
  REX_STORE_U32(object + kPage, kVideoPage);
  ReloadPage(ctx, base, object);
}

bool IsOptions(const PPCContext& ctx, uint8_t* base) {
  if (REX_LOAD_U32(ctx.r3.u32 + kPage) != kOptionsPage) return false;
  PPCContext manager = ctx;
  __imp__sub_82386D28(manager, base);
  return REX_LOAD_U32(manager.r3.u32 + kPauseMenu) == ctx.r3.u32 ||
         REX_LOAD_U32(manager.r3.u32 + kMainOptions) == ctx.r3.u32;
}

}  // namespace

void SetPauseSettingsSource(std::function<rex::ReXApp::VideoSettings()> source) {
  g_source = std::move(source);
}

void UpdateSettingsLanguage(uint8_t* base) {
  const uint32_t language = REX_LOAD_U32(kLanguage);
  rex::ui::QuitDialog::SetFrench(language == REX_LOAD_U32(kLanguages + 48 * kFrench + 4));
}

// Number of entries
REX_HOOK_RAW(sub_82363B58) {
  const uint32_t page = REX_LOAD_U32(ctx.r3.u32 + kPage);
  if (page != kVideoPage) g_video_page = false;
  if (g_video_page) {
    ctx.r3.u64 = Rows().size();
    return;
  }
  const bool options = IsOptions(ctx, base);
  __imp__sub_82363B58(ctx, base);
  ctx.r3.u64 += options;
}

// Text of an entry
REX_HOOK_RAW(sub_823635D0) {
  std::string text;
  if (g_video_page) {
    const auto rows = Rows();
    if (ctx.r4.u32 < rows.size()) text = rows[ctx.r4.u32].text;
  } else if (IsOptions(ctx, base)) {
    if (ctx.r4.u32 == 0) text = VideoTitle();
    --ctx.r4.u32;
  }
  if (text.empty()) {
    __imp__sub_823635D0(ctx, base);
    return;
  }
  text = MenuText(text);
  std::strncpy(reinterpret_cast<char*>(base + kTextBuffer), text.c_str(), kTextBufferSize - 1);
  ctx.r3.u64 = kTextBuffer;
}

// Does the entry show a game setting
REX_HOOK_RAW(sub_82363D00) {
  const bool options = IsOptions(ctx, base);
  if (g_video_page || (options && ctx.r4.u32 == 0)) {
    ctx.r3.u64 = 0;
    return;
  }
  ctx.r4.u32 -= options;
  __imp__sub_82363D00(ctx, base);
}

namespace {

// The same for both menus, with their own function for the game's entries.
void OnButton(PPCContext& ctx, uint8_t* base, void (*game)(PPCContext&, uint8_t*)) {
  const int button = ctx.r5.s32;
  const uint32_t object = ctx.r3.u32;
  if (g_video_page && button == kBack) {
    g_video_page = false;
    REX_STORE_U32(object + kSelected, 0);
    REX_STORE_U32(object + kPage, kOptionsPage);
    ctx.r3.u64 = kBackSound;
    __imp__sub_82467740(ctx, base);
    ReloadPage(ctx, base, object);
    ctx.r3.u64 = 0;
    return;
  }
  if (button == kLeft || button == kRight) {
    const uint32_t selected = REX_LOAD_U32(object + kSelected);
    if (g_video_page) {
      const auto rows = Rows();
      if (selected < rows.size()) rows[selected].change(button == kLeft ? -1 : 1);
      ReloadPage(ctx, base, object);
      ctx.r3.u64 = 0;
      return;
    }
    if (IsOptions(ctx, base)) {
      if (selected) {
        REX_STORE_U32(object + kSelected, selected - 1);
        game(ctx, base);
        REX_STORE_U32(object + kSelected, selected);
      } else {
        ctx.r3.u64 = 0;
      }
      return;
    }
  }
  game(ctx, base);
}

void OnChoice(PPCContext& ctx, uint8_t* base, void (*game)(PPCContext&, uint8_t*)) {
  if (g_video_page) return;
  if (IsOptions(ctx, base)) {
    if (ctx.r5.u32 == 0) {
      OpenVideoPage(ctx, base);
      return;
    }
    --ctx.r5.u32;
  }
  game(ctx, base);
}

}  // namespace

// A button on the pause menu
REX_HOOK_RAW(sub_82364710) { OnButton(ctx, base, __imp__sub_82364710); }

// On the main menu's options
REX_HOOK_RAW(sub_82362048) { OnButton(ctx, base, __imp__sub_82362048); }

// An entry chosen on the pause menu's options page
REX_HOOK_RAW(sub_82365108) { OnChoice(ctx, base, __imp__sub_82365108); }

// In the main menu's options
REX_HOOK_RAW(sub_82362278) { OnChoice(ctx, base, __imp__sub_82362278); }

// An entry chosen in the pause menu, on any other page
REX_HOOK_RAW(sub_82364D48) {
  if (!g_video_page) __imp__sub_82364D48(ctx, base);
}
