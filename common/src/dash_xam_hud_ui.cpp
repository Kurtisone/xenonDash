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

// Notifications (toasts) de progression : file propre et second thread invité, car le premier reste bloqué tant que la
// boîte de démarrage est ouverte et XNotifyQueueUI ne bloque pas (XAM met le message en file).
std::mutex g_toast_mu;
std::condition_variable g_toast_cv;
std::deque<std::u16string> g_toasts;

// Corps du thread invité (motif XHostThread des tests de dash_xam_redirect.cpp, StartTestThread) : sert les demandes
// les unes après les autres. Vit jusqu'à la fin du processus (object_ref retenu par Init).
// Demande de Guide en attente (posée par le rendu, servie par le thread invité).
std::atomic<bool> g_guide_requested{false};
std::atomic<uint32_t> g_guide_user{0};

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
      const bool ok = dash::xamredir::ShowXamGuide(g_guide_user.load(), 0);
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

// Palier N5 : la fenêtre ImGui de progression (ImGui, retirée) devient la boîte de message
// Xbox du HUD. Une boîte au démarrage de la construction (nom du jeu, durée estimée, journal ; « Masquer » la ferme
// sans toucher à la construction), une à l'échec (erreur + journal) ; rien au succès (le jeu se lance tout seul,
// BuildMonitor de dash_launch.cpp).
// Ligne d'avancement (déjà dans g_build) : pourcentage, étape courante (index/total, nom, message).
std::string ProgressLine(const dash::launch::BuildStatus& st) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.0f %%", st.percent);
  std::string line = "Avancement : " + std::string(buf);
  if (st.total > 0) line += "  (étape " + std::to_string(st.index) + "/" + std::to_string(st.total) + ")";
  if (!st.stage.empty()) line += "  " + st.stage;
  if (!st.message.empty()) line += " : " + st.message;
  return line;
}

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
  text += "Le jeu se lancera tout seul à la fin. L'avancement s'affiche en notifications.";
  text += "\n" + ProgressLine(st);
  if (!st.log_path.empty()) text += "\nJournal : " + st.log_path;
  req.text = Wide(text);
  req.buttons = {u"Masquer"};
  Push(std::move(req));
}

void ShowFailBox(const dash::launch::BuildStatus& st) {
  BoxRequest req;
  req.title = Wide(st.name);
  // Le message complet (liste des fonctions à faible confiance...) est trop long pour la boîte Xbox : motif seul à
  // l'écran (jusqu'au premier « : »), détail dans le journal et failure.json.
  std::string why = st.error.empty() ? "erreur de construction" : st.error;
  bool cut = false;
  if (size_t p = why.find(" : "); p != std::string::npos && p >= 20) why.resize(p), cut = true;
  if (why.size() > 200) {
    size_t n = 200;
    while (n > 0 && (static_cast<unsigned char>(why[n]) & 0xC0) == 0x80) n--;
    why.resize(n);
    cut = true;
  }
  std::string text = "Impossible de lancer le jeu : " + why;
  if (cut) text += " (détail dans le journal)";
  if (!st.stage.empty()) text += "\n" + ProgressLine(st);
  if (!st.log_path.empty()) text += "\nJournal : " + st.log_path;
  req.text = Wide(text);
  req.buttons = {u"OK"};
  Push(std::move(req));
}

void PushToast(const std::string& text) {
  {
    std::lock_guard<std::mutex> lock(g_toast_mu);
    g_toasts.push_back(Wide(text));
  }
  g_toast_cv.notify_one();
}

void ToastLoop() {
  for (;;) {
    std::u16string t;
    {
      std::unique_lock<std::mutex> lock(g_toast_mu);
      g_toast_cv.wait(lock, [] { return !g_toasts.empty(); });
      t = std::move(g_toasts.front());
      g_toasts.pop_front();
    }
    if (dash::xamredir::ShowXamToast(t.c_str())) REXLOG_INFO("[hudui] notification : {}", Narrow(t));
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // laisse la notification en cours s'afficher
  }
}

// Simulation de progression (DASH_TEST_BUILDBOX=progress) : remplace GetBuildStatus par un état fictif qui avance par
// étapes, pour exercer le chemin réel (boîte de démarrage puis notifications) sans recompiler de jeu ni toucher au
// disque. Jamais active hors de cette variable.
std::mutex g_sim_mu;
bool g_sim = false;
dash::launch::BuildStatus g_sim_status;

dash::launch::BuildStatus CurrentStatus() {
  {
    std::lock_guard<std::mutex> lock(g_sim_mu);
    if (g_sim) return g_sim_status;
  }
  return dash::launch::GetBuildStatus();
}

// Clé d'une étape : une notification par changement d'étape ou par tranche de 10 % (évite d'en empiler à chaque
// lecture de progress.json).
std::string StageKey(const dash::launch::BuildStatus& st) {
  return std::to_string(st.index) + "/" + st.stage + "/" + std::to_string(static_cast<int>(st.percent / 10.0));
}

std::string ToastText(const dash::launch::BuildStatus& st) {
  char pc[16];
  std::snprintf(pc, sizeof pc, "%.0f %%", st.percent);
  std::string t = st.name + " : " + pc;
  if (st.total > 0) t += " (étape " + std::to_string(st.index) + "/" + std::to_string(st.total) + ")";
  if (!st.stage.empty()) t += " " + st.stage;
  return t;
}

// Surveille l'état de la construction (CurrentStatus = GetBuildStatus, mis à jour par BuildMonitor/ResumeBuild de
// dash_launch.cpp, y compris dans l'instance relancée après un lancement par le hub) et affiche aux transitions : la
// boîte de démarrage, une notification Xbox à chaque étape ou tranche de 10 %, la boîte d'échec. Le texte d'une boîte
// ouverte ne se met pas à jour (la boîte de XAM est figée à sa création) : l'avancement passe donc par les
// notifications. Côté hôte, 500 ms ; le statut failed reste jusqu'au lancement suivant (g_build réinitialisé), d'où
// une boîte par transition.
void BuildWatcher() {
  bool was_active = false, was_failed = false;
  std::string last_key;
  for (;;) {
    const dash::launch::BuildStatus st = CurrentStatus();
    if (st.active && !was_active) {
      // Laisse le moniteur lire progress.json (étape et pourcentage réels) avant d'ouvrir la boîte, 3 s au plus.
      dash::launch::BuildStatus cur = st;
      for (int i = 0; i < 6 && cur.stage.empty() && cur.active; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        cur = CurrentStatus();
      }
      ShowStartBox(cur);
      last_key = StageKey(cur);
    } else if (st.active && !st.stage.empty()) {
      const std::string key = StageKey(st);
      if (key != last_key) {
        last_key = key;
        PushToast(ToastText(st));
      }
    }
    if (st.failed && !was_failed) ShowFailBox(st);
    was_active = st.active;
    was_failed = st.failed;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

// DASH_TEST_BUILDBOX=start|fail|progress : transition simulée 10 s après le démarrage (sans vraie recompilation).
void TestThread(const char* scenario) {
  std::this_thread::sleep_for(std::chrono::seconds(10));
  if (std::string_view(scenario) == "progress") {
    // Construction fictive : 6 étapes, 4 s chacune ; le BuildWatcher réel affiche la boîte puis les notifications.
    static const char* const kStages[] = {"analyse", "décodage", "recompilation", "assemblage", "liens", "vérification"};
    for (int i = 0; i <= 6; i++) {
      {
        std::lock_guard<std::mutex> lock(g_sim_mu);
        g_sim = true;
        g_sim_status = dash::launch::BuildStatus();
        g_sim_status.active = i < 6;
        g_sim_status.name = "Jeu de test (HUD)";
        g_sim_status.estimate = 900;
        g_sim_status.log_path = "/tmp/xenondash-test/build.log";
        g_sim_status.total = 6;
        g_sim_status.index = i < 6 ? i + 1 : 6;
        g_sim_status.stage = kStages[i < 6 ? i : 5];
        g_sim_status.percent = i * 100.0 / 6.0;
      }
      std::this_thread::sleep_for(std::chrono::seconds(4));
    }
    std::lock_guard<std::mutex> lock(g_sim_mu);
    g_sim = false;
    return;
  }
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

void RequestGuide(uint32_t user) {
  if (!std::getenv("DASH_XAM_MODULE")) {
    REXLOG_WARN("[guide] demande de Guide ignorée : DASH_XAM_MODULE absent (pas de XAM recompilé)");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_guide_user.store(user < 4 ? user : 0);
    g_guide_requested.store(true);
  }
  g_cv.notify_one();
  REXLOG_INFO("[guide] bouton Guide (utilisateur {}) : demande déposée pour le thread invité", user);
}

void Init(rex::system::KernelState* ks) {
  if (!ks || !std::getenv("DASH_XAM_MODULE")) return;  // sans XAM recompilé : pas de HUD
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
  static rex::system::object_ref<rex::system::XHostThread> toast_host;
  toast_host = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(ks, 256 * 1024, 0, [] {
    ToastLoop();
    return 0;
  }));
  toast_host->set_name("XAM hudui toast");
  toast_host->Create();
  std::thread(BuildWatcher).detach();
  if (const char* test = std::getenv("DASH_TEST_BUILDBOX"); test && *test) {
    const std::string scenario = test;
    std::thread([scenario] { TestThread(scenario.c_str()); }).detach();
  }
  REXLOG_INFO("[hudui] fenêtres du HUD de XAM : thread invité prêt (progression de construction par boîte de message Xbox)");
}

}  // namespace dash::hudui
