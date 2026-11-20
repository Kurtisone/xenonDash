// dash - fenêtres du HUD de XAM appelées depuis l'hôte (voir dash_xam_hud_ui.h).

#include "dash_xam_hud_ui.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#include "dash_launch.h"
#include "dash_xam_redirect.h"

namespace dash::hudui {
namespace {

// UTF-8 vers UTF-16 (noms de jeux, chemins et messages : tout ce qui vient de games.toml / dash_launch.cpp).
std::u16string Wide(const std::string& s) {
  std::u16string out;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = c;
    size_t n = 1;
    if (c >= 0xF0) {
      cp = c & 0x07u;
      n = 4;
    } else if (c >= 0xE0) {
      cp = c & 0x1Fu;
      n = 3;
    } else if (c >= 0xC0) {
      cp = c & 0x3Fu;
      n = 2;
    }
    if (i + n > s.size()) {
      out.push_back(u'?');
      break;
    }
    for (size_t k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    if (cp >= 0x10000) {  // hors plan multilingue : paire de substitution
      cp -= 0x10000;
      out.push_back(static_cast<char16_t>(0xD800 | (cp >> 10)));
      out.push_back(static_cast<char16_t>(0xDC00 | (cp & 0x3FF)));
    } else {
      out.push_back(static_cast<char16_t>(cp));
    }
    i += n;
  }
  return out;
}

// UTF-16 vers ASCII approximatif pour le journal (comme ReadWide, dash_xam_redirect.cpp).
std::string Narrow(const std::u16string& s) {
  std::string out;
  for (char16_t c : s) {
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
    } else {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04X", c);
      out += b;
    }
  }
  return out;
}

// Demande de boîte de message, remplie par les threads hôtes (watcher, tests) et servie dans l'ordre par l'unique
// thread invité : une seule fenêtre à la fois, et l'appel XAM est bloquant côté invité.
struct BoxRequest {
  std::u16string title, text;
  std::vector<std::u16string> buttons;
  int wait_ms = 600000;  // borne d'attente de la fermeture, côté thread invité
};

std::mutex g_mu;
std::condition_variable g_cv;
std::deque<BoxRequest> g_queue;

void Push(BoxRequest req) {
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_queue.push_back(std::move(req));
  }
  g_cv.notify_one();
}

// Corps du thread invité (motif XHostThread des tests de dash_xam_redirect.cpp, StartTestThread) : sert les demandes
// les unes après les autres. Vit jusqu'à la fin du processus (object_ref retenu par Init).
// Demande de Guide en attente (posée par le rendu, servie par le thread invité).
std::atomic<bool> g_guide_requested{false};

void GuestLoop() {
  for (;;) {
    BoxRequest req;
    bool guide = false;
    {
      std::unique_lock<std::mutex> lock(g_mu);
      g_cv.wait(lock, [] { return !g_queue.empty() || g_guide_requested.load(); });
      if (g_queue.empty()) {
        guide = true;
        g_guide_requested.store(false);
      } else {
        req = std::move(g_queue.front());
        g_queue.pop_front();
      }
    }
    if (guide) {
      // Guide de la console, pas la lame maison : XAM recompilé l'affiche (ou dit pourquoi il ne peut pas).
      const bool ok = dash::xamredir::ShowXamGuide(0, 0);
      if (!ok) {
        REXLOG_WARN("[guide] XamShowGuideUI n'a pas abouti (voir la ligne [guide] ci-dessus : XAM non chargé, "
                    "non redirigé, ou appel refusé)");
      }
      continue;
    }
    std::vector<const char16_t*> buttons;
    for (const std::u16string& b : req.buttons) buttons.push_back(b.c_str());
    uint32_t choice = 0xFFFFFFFF;
    if (dash::xamredir::ShowXamMessageBox(req.title.c_str(), req.text.c_str(), buttons.data(), buttons.size(), 0, 0,
                                          &choice, req.wait_ms)) {
      REXLOG_INFO("[hudui] boîte « {} » fermée (bouton {})", Narrow(req.title), choice);
    }
  }
}

// Palier N5 : la fenêtre ImGui de progression (dash_xam_ui.cpp, invisible en rendu natif) devient la boîte de message
// Xbox du HUD. Une boîte au démarrage de la construction (nom du jeu, durée estimée, journal ; « Masquer » la ferme
// sans toucher à la construction), une à l'échec (erreur + journal) ; rien au succès (le jeu se lance tout seul,
// BuildMonitor de dash_launch.cpp).
void ShowStartBox(const dash::launch::BuildStatus& st) {
  BoxRequest req;
  req.title = Wide(st.name);
  char mins[16];
  std::snprintf(mins, sizeof mins, "%.0f", st.estimate / 60.0);
  std::string text = "Première exécution : recompilation du jeu en natif. ";
  if (st.estimate > 0) {
    text += "Durée estimée : environ " + std::string(mins) + " min. ";
  } else {
    text += "Cela peut prendre plusieurs dizaines de minutes. ";
  }
  text += "Le jeu se lancera tout seul à la fin.";
  if (!st.log_path.empty()) text += "\nJournal : " + st.log_path;
  req.text = Wide(text);
  req.buttons = {u"Masquer"};
  Push(std::move(req));
}

void ShowFailBox(const dash::launch::BuildStatus& st) {
  BoxRequest req;
  req.title = Wide(st.name);
  std::string text = "Impossible de lancer le jeu : " + (st.error.empty() ? "erreur de construction" : st.error);
  if (!st.log_path.empty()) text += "\nJournal : " + st.log_path;
  req.text = Wide(text);
  req.buttons = {u"OK"};
  Push(std::move(req));
}

// Surveille l'état de la construction (GetBuildStatus, mis à jour par BuildMonitor/ResumeBuild de dash_launch.cpp, y
// compris dans l'instance relancée après un lancement par le hub) et affiche les boîtes aux transitions. Côté hôte,
// 500 ms ; le statut failed reste jusqu'au lancement suivant (g_build réinitialisé), d'où une boîte par transition.
void BuildWatcher() {
  bool was_active = false, was_failed = false;
  for (;;) {
    const dash::launch::BuildStatus st = dash::launch::GetBuildStatus();
    if (st.active && !was_active) ShowStartBox(st);
    if (st.failed && !was_failed) ShowFailBox(st);
    was_active = st.active;
    was_failed = st.failed;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

// DASH_TEST_BUILDBOX=start|fail : transition simulée 10 s après le démarrage (sans vraie recompilation).
void TestThread(const char* scenario) {
  std::this_thread::sleep_for(std::chrono::seconds(10));
  dash::launch::BuildStatus st;
  st.name = "Jeu de test (HUD)";
  st.log_path = "/tmp/xenondash-test/build.log";
  if (std::string_view(scenario) == "fail") {
    st.error = "erreur simulée par DASH_TEST_BUILDBOX";
    ShowFailBox(st);
  } else {
    st.estimate = 900;
    ShowStartBox(st);
  }
}

}  // namespace

void RequestGuide() {
  if (!std::getenv("DASH_XAM_MODULE")) {
    REXLOG_WARN("[guide] demande de Guide ignorée : DASH_XAM_MODULE absent (pas de XAM recompilé)");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_guide_requested.store(true);
  }
  g_cv.notify_one();
  REXLOG_INFO("[guide] bouton Guide : demande déposée pour le thread invité");
}

void Init(rex::system::KernelState* ks, bool imgui_available) {
  // Builds avec plugin xenos : la superposition ImGui existe, les fenêtres de dash_xam_ui.cpp restent seules.
  if (imgui_available) return;
  if (!ks || !std::getenv("DASH_XAM_MODULE")) return;  // sans XAM recompilé : ni ImGui ni HUD (journal de dash_xam_ui.cpp)
  // L'unique thread invité des fenêtres (l'object_ref vit jusqu'à la fin du processus, comme les tests de
  // dash_xam_redirect.cpp). Le corps attend la première demande ; une demande antérieure au chargement de XAM échoue
  // avec un avertissement (ShowXamMessageBox).
  static rex::system::object_ref<rex::system::XHostThread> host;
  host = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(ks, 256 * 1024, 0, [] {
    GuestLoop();
    return 0;
  }));
  host->set_name("XAM hudui");
  host->Create();
  std::thread(BuildWatcher).detach();
  if (const char* test = std::getenv("DASH_TEST_BUILDBOX"); test && *test) {
    const std::string scenario = test;
    std::thread([scenario] { TestThread(scenario.c_str()); }).detach();
  }
  REXLOG_INFO("[hudui] fenêtres du HUD de XAM : thread invité prêt (progression de construction par boîte de message Xbox)");
}

}  // namespace dash::hudui
