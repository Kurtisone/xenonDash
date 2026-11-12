// dash - remplaçant de l'interface « Succès » de XAM (XamShowAchievementsUI).
//
// Sur la console, Game Summary > Achievements ne fait pas partie du Dashboard : il appelle
// XamShowAchievementsUI(utilisateur, titre) et c'est XAM qui affiche sa propre interface
// (celle du Guide). ReXGlue émule XAM en haut niveau, sans ses interfaces : on dessine ici
// l'équivalent avec ImGui, à partir des GPD du profil.
//
// Comme XAM : notification XN_SYS_UI (1 à l'ouverture, 0 à la fermeture) et entrées du
// Dashboard suspendues tant que l'interface est affichée. Haut/Bas : défiler, Échap/B : fermer.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>
#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/windowed_app_context.h>

#include "dash_gpd.h"
#include "dash_launch.h"

namespace dash::achievements {
struct TitleView {
  std::u16string name;
  std::vector<std::pair<gpd::Achievement, std::vector<uint8_t>>> items;
};
TitleView GetTitleView(uint32_t title_id);
}  // namespace dash::achievements

namespace dash::xamui {

using namespace rex;

constexpr uint32_t kXnSysUi = 0x00000009;  // XN_SYS_UI : interface système affichée/masquée

struct Context {
  ui::WindowedAppContext* app = nullptr;
  ui::ImGuiDrawer* imgui = nullptr;
  ui::ImmediateDrawer* immediate = nullptr;
};
Context g_ctx;
std::atomic<bool> g_open{false};
// Textures des écrans fermés, libérées à l'ouverture suivante (thread d'interface).
std::vector<std::unique_ptr<ui::ImmediateTexture>> g_retired;

u32 XamShowAchievementsUI_entry(u32 user_index, u32 title_id);
void OpenLauncherHotkey();
void OpenLauncher();

// DASH_TEST_XAMUI=<TitleID hexa> : ouvre l'écran de succès 20 s après le démarrage (test).
void Init(ui::WindowedAppContext* app, ui::ImGuiDrawer* imgui, ui::ImmediateDrawer* immediate) {
  g_ctx = {app, imgui, immediate};
  OpenLauncherHotkey();
  if (const char* test = std::getenv("DASH_TEST_XAMUI")) {
    uint32_t title = uint32_t(std::strtoul(test, nullptr, 16));
    std::thread([title] {
      std::this_thread::sleep_for(std::chrono::seconds(20));
      XamShowAchievementsUI_entry(0, title);
    }).detach();
  }
}
bool IsOpen() { return g_open.load(); }

std::string Utf8(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) {
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6)); out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xE0 | (c >> 12)); out += char(0x80 | ((c >> 6) & 0x3F)); out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

class AchievementsDialog : public ui::ImGuiDialog {
 public:
  AchievementsDialog(ui::ImGuiDrawer* drawer, ui::ImmediateDrawer* immediate,
                     achievements::TitleView view)
      : ImGuiDialog(drawer), title_(Utf8(view.name)) {
    for (auto& [a, png] : view.items) {
      Row r;
      r.name = Utf8(a.name);
      bool secret = !a.earned() && !(a.flags & 0x8);  // non obtenu et « non affiché »
      r.desc = secret ? "Succès secret" : Utf8(a.earned() ? a.unlocked_desc : a.locked_desc);
      if (r.desc.empty()) r.desc = Utf8(a.unlocked_desc);
      r.gamerscore = a.gamerscore;
      r.earned = a.earned();
      int w = 0, h = 0;
      auto rgba = png.empty() ? std::vector<uint8_t>{} : ui::DecodeImageRGBA(png.data(), png.size(), w, h);
      if (!rgba.empty() && immediate) {
        r.icon = immediate->CreateTexture(uint32_t(w), uint32_t(h), ui::ImmediateTextureFilter::kLinear,
                                          false, rgba.data());
      }
      total_gs_ += r.gamerscore;
      if (r.earned) { earned_++; earned_gs_ += r.gamerscore; }
      rows_.push_back(std::move(r));
    }
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const ImVec2 size(780.0f, 560.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::SetNextWindowFocus();
    std::string window = (title_.empty() ? std::string("Succès") : title_) + "##dash_xam_achievements";
    if (ImGui::Begin(window.c_str(), nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::Text("%d / %d succès obtenus", earned_, int(rows_.size()));
      ImGui::SameLine(size.x - 170.0f);
      ImGui::Text("%d / %d G", earned_gs_, total_gs_);
      ImGui::ProgressBar(rows_.empty() ? 0.0f : float(earned_) / float(rows_.size()), ImVec2(-1, 6), "");
      ImGui::Separator();

      // Navigation au clavier / à la manette (les entrées du Dashboard sont suspendues).
      int prev = selected_;
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) selected_++;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) selected_--;
      if (!rows_.empty()) selected_ = std::clamp(selected_, 0, int(rows_.size()) - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        Close();
      }

      ImGui::BeginChild("liste", ImVec2(0, -28), false);
      for (int i = 0; i < int(rows_.size()); i++) {
        const Row& r = rows_[i];
        ImGui::PushID(i);
        ImVec2 start = ImGui::GetCursorScreenPos();
        ImGui::Selectable("##ligne", selected_ == i, 0, ImVec2(0, 70));
        if (selected_ == i && prev != selected_) ImGui::SetScrollHereY(0.5f);
        ImGui::SetCursorScreenPos(ImVec2(start.x + 4, start.y + 3));
        if (!r.earned) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
        if (r.icon) {
          ImGui::Image(reinterpret_cast<ImTextureID>(r.icon.get()), ImVec2(64, 64));
        } else {
          ImGui::Dummy(ImVec2(64, 64));
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(r.name.c_str());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x - 220.0f);
        ImGui::TextDisabled("%s", r.desc.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::SameLine(size.x - 110.0f);
        ImGui::Text("%d G", r.gamerscore);
        if (!r.earned) ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + 74));
        ImGui::Dummy(ImVec2(0, 0));  // ImGui exige un élément après un déplacement du curseur
        ImGui::PopID();
      }
      if (rows_.empty()) ImGui::TextDisabled("Aucun succès pour ce jeu dans le profil.");
      ImGui::EndChild();
      ImGui::TextDisabled("Haut/Bas : parcourir    Échap / B : fermer");
    }
    ImGui::End();
  }

  void OnClose() override {
    // Textures : le rendu de l'image en cours peut encore les utiliser ; on les garde jusqu'à
    // la prochaine ouverture. Notification : envoyée hors du thread d'interface (verrou noyau).
    for (auto& r : rows_) {
      if (r.icon) g_retired.push_back(std::move(r.icon));
    }
    std::thread([] {
      if (auto* ks = system::kernel_state()) ks->BroadcastNotification(kXnSysUi, 0);
      g_open = false;
    }).detach();
  }

 private:
  struct Row {
    std::string name, desc;
    int gamerscore = 0;
    bool earned = false;
    std::unique_ptr<ui::ImmediateTexture> icon;
  };
  std::string title_;
  std::vector<Row> rows_;
  int selected_ = 0, earned_ = 0, earned_gs_ = 0, total_gs_ = 0;
};

// --- Lanceur de jeux (phase P5) --------------------------------------------------------------------------
// F5 (ou Back + Y sur manette) ouvre la liste des jeux de games.toml : Haut/Bas, Entrée / A pour lancer
// (voir dash_launch.cpp : le Dashboard cède la place au jeu puis est relancé), Échap / B pour fermer.
// Complète le chemin natif du Dashboard (XamLoaderLaunchTitle), que la 6770 n'emprunte que pour un disque.
class LauncherDialog : public ui::ImGuiDialog {
 public:
  explicit LauncherDialog(ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer), games_(launch::Games()) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const ImVec2 size(620.0f, 360.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Jeux##dash_launcher", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings)) {
      if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown)) selected_++;
      if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp)) selected_--;
      if (!games_.empty()) selected_ = std::clamp(selected_, 0, int(games_.size()) - 1);
      if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Backspace) ||
          ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
        Close();
      } else if (!games_.empty() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Space) ||
                                     ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown))) {
        auto g = games_[selected_];
        std::thread([g] { launch::LaunchGame(g); }).detach();  // ne revient pas : le processus quitte
      }
      for (int i = 0; i < int(games_.size()); i++) {
        const auto& g = games_[i];
        ImGui::PushID(i);
        ImGui::Selectable(fmt::format("{}   ({:08X}, {})", g.name, g.title_id, g.kind).c_str(), selected_ == i, 0,
                          ImVec2(0, 40));
        ImGui::PopID();
      }
      if (games_.empty()) ImGui::TextWrapped("Aucun jeu : créer %s (modèle games.example.toml).", "<game_data_root>/games.toml");
      ImGui::Separator();
      ImGui::TextDisabled("Haut/Bas : choisir    Entrée / A : lancer    Échap / B : fermer");
    }
    ImGui::End();
  }
  void OnClose() override {
    std::thread([] {
      if (auto* ks = system::kernel_state()) ks->BroadcastNotification(kXnSysUi, 0);
      g_open = false;
    }).detach();
  }

 private:
  std::vector<launch::Game> games_;
  int selected_ = 0;
};

void OpenLauncher() {
  if (!g_ctx.app || !g_ctx.imgui || g_open.exchange(true)) return;
  std::thread([] { system::kernel_state()->BroadcastNotification(kXnSysUi, 1); }).detach();
  g_ctx.app->CallInUIThread([] { new LauncherDialog(g_ctx.imgui); });
}

// Écran de progression de la construction d'un jeu par le moteur de recettes (dash_launch.cpp, tools/recipe_build.py) :
// fenêtre en bas de l'écran tant que la construction est active (ou a échoué : Échap la ferme). Le Dashboard reste utilisable.
void DrawBuildProgress(ImGuiIO& io) {
  static std::string dismissed;  // construction échouée déjà fermée par l'utilisateur
  const launch::BuildStatus st = launch::GetBuildStatus();
  if (!st.active && !st.failed) return;
  if (st.failed && dismissed == st.error + st.name) return;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, io.DisplaySize.x - 40.0f), 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.92f);
  if (ImGui::Begin("Construction du jeu##dash_build", nullptr,
                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                       ImGuiWindowFlags_AlwaysAutoResize)) {
    if (st.active) {
      ImGui::Text("Construction de %s (recompilation native, première exécution)", st.name.c_str());
      ImGui::ProgressBar(float(st.percent / 100.0), ImVec2(-1.0f, 0.0f), fmt::format("{:.0f} %", st.percent).c_str());
      ImGui::TextWrapped("%s : %s", st.stage.c_str(), st.message.c_str());
      ImGui::TextDisabled("%.0f s écoulées (estimation de la recette : %.0f s). Journal : %s", st.elapsed, st.estimate,
                          st.log_path.c_str());
      ImGui::TextDisabled("Le jeu se lancera tout seul à la fin (échec : retour au Dashboard).");
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Construction de %s échouée", st.name.c_str());
      ImGui::TextWrapped("%s", st.error.c_str());
      ImGui::TextDisabled("Retour au Dashboard dans quelques secondes. Journal : %s", st.log_path.c_str());
      if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) dismissed = st.error + st.name;
    }
  }
  ImGui::End();
}

// Boîte de dialogue invisible, toujours ouverte : surveille F5 (ou Back + Y).
class HotkeyDialog : public ui::ImGuiDialog {
 public:
  explicit HotkeyDialog(ui::ImGuiDrawer* d) : ImGuiDialog(d) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    DrawBuildProgress(io);
    if (g_open.load()) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F5, false) ||
        (ImGui::IsKeyDown(ImGuiKey_GamepadBack) && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false))) {
      OpenLauncher();
    }
  }
};

void OpenLauncherHotkey() {
  if (!g_ctx.app || !g_ctx.imgui) return;
  g_ctx.app->CallInUIThread([] { new HotkeyDialog(g_ctx.imgui); });
}

u32 XamShowAchievementsUI_entry(u32 user_index, u32 title_id) {
  if (user_index >= 4) return X_ERROR_INVALID_PARAMETER;
  if (!g_ctx.app || !g_ctx.imgui || g_open.exchange(true)) return X_ERROR_FUNCTION_FAILED;
  auto view = achievements::GetTitleView(title_id);
  REXLOG_INFO("[dash] XamShowAchievementsUI(titre={:08X}) : {} succès", uint32_t(title_id),
              view.items.size());
  std::thread([] { system::kernel_state()->BroadcastNotification(kXnSysUi, 1); }).detach();
  g_ctx.app->CallInUIThread([view = std::move(view)]() mutable {
    g_retired.clear();
    new AchievementsDialog(g_ctx.imgui, g_ctx.immediate, std::move(view));  // se libère à Close()
  });
  return X_ERROR_SUCCESS;
}

}  // namespace dash::xamui

REX_HOOK(__imp__XamShowAchievementsUI, dash::xamui::XamShowAchievementsUI_entry)
