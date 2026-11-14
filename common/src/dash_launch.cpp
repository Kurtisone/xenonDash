// dash - lancement des jeux depuis le Dashboard (phase P5).
//
// Registre : <game_data_root>/games.toml (modèle versionné : games.example.toml) : par TitleID, un nom et
// soit `native = "commande"` (+ `cwd`) soit `xenia = "chemin XEX/ISO"` (lancé par la commande Xenia :
// `[xenia] command`, option --dash_xenia ou variable DASH_XENIA).
//
// Ce que le Dashboard appelle (6770, relevé dans le code recompilé) :
//   XamLoaderLaunchTitle(chemin, indicateurs)  : chemin NULL = « lancer le disque » (bouton de disque
//       du Dashboard, avec des données de lancement posées par XamLoaderSetLaunchData), ou un chemin XEX ;
//       les jetons internes « XLNI_DET_MEDIA » / « XLNI_DASH_ARCADE » sont des chaînes, pas des chemins.
//   XamLoaderLaunchTitleEx(r3..r6)             : lancement avec données (relance du Dashboard, Arcade...).
//   XamLoaderGetMediaInfo, XamLoaderGetPriorTitleId : journalisés.
//
// Mécanisme (le plus robuste, sans partage de GPU/audio/fenêtre entre le Dashboard et le jeu) : le Dashboard
// ne reste pas en veille. Il lance un petit superviseur détaché (`/bin/sh`, nouvelle session) puis quitte :
//     cd <cwd> ; <commande du jeu> ; cd <répertoire initial> ; exec <le même exécutable + les mêmes arguments>
// Quand le jeu se termine (quel que soit son code de retour), le Dashboard est relancé à l'identique
// (variable DASH_RETURNED_FROM=<TitleID> dans son environnement, journalisée). LD_LIBRARY_PATH du Dashboard
// est retiré de l'environnement du jeu (il pointerait vers le SDK du Dashboard) sauf `keep_ld_library_path`.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/kernel/xam/module.h>
#include <rex/types.h>

#include "dash_gpd.h"
#include "dash_launch.h"

REXCVAR_DEFINE_STRING(dash_xenia, "", "Dash",
                      "Commande Xenia pour les jeux `xenia = \"...\"` de games.toml (défaut : [xenia] command, "
                      "variable DASH_XENIA, sinon xenia_canary)");
REXCVAR_DEFINE_STRING(dash_launch, "", "Dash",
                      "TitleID (hexadécimal) d'un jeu de games.toml à lancer tout de suite au démarrage (test)");
REXCVAR_DEFINE_STRING(dash_disc, "", "Dash",
                      "TitleID (hexadécimal) d'un jeu de games.toml présenté comme disque inséré (équivaut à `disc = true`)");
REXCVAR_DEFINE_INT32(dash_launch_delay, 5, "Dash", "Délai (s) avant le lancement demandé par --dash_launch");

REXCVAR_DEFINE_STRING(dash_library_toml, "", "Dash",
                      "games.auto.toml de la bibliothèque (défaut : $XENONDASH_LIBRARY_TOML, sinon "
                      "${XDG_DATA_HOME:-~/.local/share}/xenondash/library/games.auto.toml)");

REXCVAR_DEFINE_BOOL(dash_library_scan, false, "Dash",
                    "Crée le dossier de jeux et scanne la bibliothèque au démarrage (tools/game_library.py ; "
                    "toujours actif sous AppImage)");

REXCVAR_DEFINE_BOOL(dash_auto_disc, true, "Dash",
                    "Sans `disc = true`, le dernier jeu lancé de la bibliothèque est présenté comme disque inséré "
                    "(le hub NXE ne liste que ce jeu)");

REXCVAR_DECLARE(std::string, game_data_root);

namespace dash::achievements {
void MarkPlayed(uint32_t title_id, uint64_t filetime);
uint64_t LastPlayed(uint32_t title_id);
}

namespace dash::launch {

namespace fs = std::filesystem;

namespace {

fs::path RegistryPath() { return fs::absolute(REXCVAR_GET(game_data_root)) / "games.toml"; }

uint32_t ParseHex(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 16)); }

uint32_t TypeFromName(const std::string& s) {
  if (s == "arcade") return 0x000D0000;
  if (s == "demo") return 0x00080000;
  if (s == "original" || s == "xbox") return 0x00005000;
  if (s == "xna") return 0x000E0000;
  return uint32_t(std::strtoul(s.c_str(), nullptr, 0));
}

std::string XeniaCommand(const toml::table& root) {
  std::string c = REXCVAR_GET(dash_xenia);
  if (c.empty()) {
    if (const char* e = std::getenv("DASH_XENIA")) c = e;
  }
  if (c.empty()) c = root["xenia"]["command"].value_or(std::string());
  if (c.empty()) c = "xenia_canary";
  return c;
}

// Quote une chaîne pour /bin/sh.
std::string Q(const std::string& s) {
  std::string o = "'";
  for (char c : s) {
    if (c == '\'') o += "'\\''";
    else o += c;
  }
  return o + "'";
}

}  // namespace

namespace {

// Chemin du registre automatique de la bibliothèque (tools/game_library.py scan) : --dash_library_toml, variable
// XENONDASH_LIBRARY_TOML, sinon ${XDG_DATA_HOME:-~/.local/share}/xenondash/library/games.auto.toml.
fs::path LibraryDir() {
  if (const char* e = std::getenv("XENONDASH_LIBRARY_DIR"); e && *e) return e;
  const char* x = std::getenv("XDG_DATA_HOME");
  fs::path base = (x && *x) ? fs::path(x) : fs::path(std::getenv("HOME") ? std::getenv("HOME") : ".") / ".local" / "share";
  return base / "xenondash" / "library";
}

fs::path AutoRegistryPath() {
  if (!REXCVAR_GET(dash_library_toml).empty()) return REXCVAR_GET(dash_library_toml);
  if (const char* e = std::getenv("XENONDASH_LIBRARY_TOML"); e && *e) return e;
  return LibraryDir() / "games.auto.toml";
}

// Ajoute les jeux d'un fichier de registre (les TitleID déjà présents l'emportent : games.toml manuel prioritaire).
void ParseRegistry(const fs::path& path, bool automatic, const std::string& xenia_default, std::vector<Game>& out) {
  toml::table root;
  try {
    root = toml::parse_file(path.string());
  } catch (const toml::parse_error& e) {
    if (fs::exists(path)) REXLOG_WARN("[dash] {} illisible : {}", path.string(), e.description());
    return;
  }
  std::string xenia_cmd = automatic ? xenia_default : XeniaCommand(root);
  auto* games = root["game"].as_table();
  if (!games) games = &root;  // tolère [45410813] au niveau racine
  for (auto&& [key, node] : *games) {
    auto* t = node.as_table();
    if (!t) continue;
    std::string k(key.str());
    if (k == "xenia" || k == "disc") continue;
    Game g;
    g.title_id = ParseHex(k);
    if (!g.title_id) continue;
    if (std::any_of(out.begin(), out.end(), [&](const Game& o) { return o.title_id == g.title_id; })) continue;
    g.name = (*t)["name"].value_or(k);
    g.cwd = (*t)["cwd"].value_or(std::string());
    g.keep_ld = (*t)["keep_ld_library_path"].value_or(false);
    if (auto n = (*t)["native"].value<std::string>()) {
      g.command = *n;
      g.kind = "native";
    } else if (!(*t)["recipe"].value_or(std::string()).empty()) {
      g.kind = "recipe";  // construit à la demande par tools/recipe_build.py (cache), puis lancé comme un jeu natif
    } else if (auto x = (*t)["xenia"].value<std::string>()) {
      g.command = xenia_cmd + " " + Q(*x);
      g.kind = "xenia";
    } else {
      REXLOG_WARN("[dash] {} : {} n'a ni `native`, `recipe` ni `xenia`", path.string(), k);
      continue;
    }
    g.recipe = (*t)["recipe"].value_or(std::string());
    g.disc = !automatic && (*t)["disc"].value_or(false);
    g.icon = (*t)["icon"].value_or(std::string());
    g.spa = (*t)["spa"].value_or(std::string());
    g.from_library = automatic;
    if (auto ty = (*t)["type"].value<std::string>()) g.content_type = TypeFromName(*ty);
    else if (auto ti = (*t)["type"].value<int64_t>()) g.content_type = uint32_t(*ti);
    out.push_back(std::move(g));
  }
}

}  // namespace

// games.toml (manuel) puis games.auto.toml de la bibliothèque. Relus seulement si le fichier a changé (date, taille).
std::vector<Game> Games() {
  static std::mutex m;
  static std::vector<Game> cache;
  static std::string key;
  auto stamp = [](const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    auto sz = fs::file_size(p, ec);
    return p.string() + ":" + std::to_string(ec ? 0 : t.time_since_epoch().count()) + ":" + std::to_string(ec ? 0 : sz);
  };
  const fs::path manual = RegistryPath(), automatic = AutoRegistryPath();
  std::lock_guard lock(m);
  std::string k = stamp(manual) + "|" + stamp(automatic);
  if (k == key) return cache;
  key = k;
  cache.clear();
  std::string xenia_default = "xenia_canary";
  ParseRegistry(manual, false, xenia_default, cache);
  ParseRegistry(automatic, true, xenia_default, cache);
  return cache;
}

std::optional<Game> Find(uint32_t title_id) {
  for (auto& g : Games())
    if (g.title_id == title_id) return g;
  return std::nullopt;
}

// `library = true` à la racine de games.toml : les jeux apparaissent aussi comme contenu installé (voir docs/technique.md).
bool LibraryEnabled() {
  std::error_code ec;
  if (fs::exists(AutoRegistryPath(), ec)) return true;  // bibliothèque automatique : toujours listée
  try {
    return toml::parse_file(RegistryPath().string())["library"].value_or(false);
  } catch (...) {
    return false;
  }
}

// Titre « dans le lecteur » : `disc = "45410813"` à la racine de games.toml (sinon le seul jeu déclaré).
std::optional<Game> DiscGame() {
  if (auto d = InsertedDisc()) return d;
  auto g = Games();
  if (g.size() == 1) return g[0];
  return std::nullopt;
}

// Disque simulé : --dash_disc=<TitleID>, sinon le premier jeu avec `disc = true`, sinon `disc = "<TitleID>"` à la racine.
std::optional<Game> InsertedDisc() {
  if (!REXCVAR_GET(dash_disc).empty()) return Find(ParseHex(REXCVAR_GET(dash_disc)));
  for (auto& g : Games())
    if (g.disc) return g;
  try {
    auto root = toml::parse_file(RegistryPath().string());
    if (auto d = root["disc"].value<std::string>()) return Find(ParseHex(*d));
  } catch (...) {
  }
  // Bibliothèque automatique : le hub NXE ne liste (Recent Games, tuile « Play ») que le jeu « dans le plateau » (voir
  // docs/nxe_jeux.md) ; on y met le dernier jeu lancé de la bibliothèque, sinon le premier par ordre alphabétique.
  if (REXCVAR_GET(dash_auto_disc)) {
    std::optional<Game> best;
    uint64_t best_t = 0;
    for (auto& g : Games()) {
      if (!g.from_library) continue;
      uint64_t t = achievements::LastPlayed(g.title_id);
      if (!best || t > best_t || (t == best_t && g.name < best->name)) best = g, best_t = t;
    }
    return best;
  }
  return std::nullopt;
}

namespace {

std::atomic<bool> g_launching{false};

// Commande qui relance le Dashboard après un jeu. Sous AppImage, /proc/self/exe est dans un montage qui disparaît dès que le
// Dashboard quitte : on relance l'AppImage ($APPIMAGE), dont AppRun redonne ses propres options (celles de l'utilisateur
// ne sont pas rejouées).
std::string RelaunchCommand(const std::string& exe, const std::vector<std::string>& args) {
  if (const char* ai = std::getenv("APPIMAGE"); ai && *ai && fs::exists(ai)) return "exec " + Q(ai);
  std::string relaunch = "exec " + Q(exe);
  for (size_t i = 1; i < args.size(); i++) {
    // --dash_launch(=...) / --dash_launch_delay : lancement de test, à ne pas rejouer au retour (boucle sans fin).
    if (args[i].rfind("--dash_launch", 0) == 0) {
      if (args[i].find('=') == std::string::npos && i + 1 < args.size()) i++;
      continue;
    }
    relaunch += " " + Q(args[i]);
  }
  return relaunch;
}

std::string SelfExe() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  return n > 0 ? std::string(buf, size_t(n)) : std::string();
}

std::vector<std::string> SelfArgs() {
  std::vector<std::string> a;
  std::ifstream f("/proc/self/cmdline", std::ios::binary);
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t p = 0;
  while (p < s.size()) {
    size_t e = s.find('\0', p);
    if (e == std::string::npos) e = s.size();
    a.push_back(s.substr(p, e - p));
    p = e + 1;
  }
  return a;
}

}  // namespace

// Met à jour la date « dernier joué » du jeu dans FFFE07D1.gpd des profils (entrée espace 4 « titres » : FILETIME
// au décalage 0x20) ; le jeu doit déjà y figurer (les entrées ne sont pas créées : taille fixe du fichier).
void TouchLastPlayed(uint32_t title_id) {
  std::error_code ec;
  auto profiles = fs::absolute(REXCVAR_GET(game_data_root)) / "profiles";
  if (!fs::is_directory(profiles, ec)) return;
  uint64_t ft = (uint64_t(std::time(nullptr)) + 11644473600ull) * 10000000ull;
  for (auto& d : fs::directory_iterator(profiles, ec)) {
    auto path = d.path() / "FFFE07D1.gpd";
    gpd::File f;
    if (!d.is_directory()) continue;
    if (!fs::exists(path, ec) || !f.Load(path)) {  // profil neuf : pas de GPD du Dashboard
      achievements::MarkPlayed(title_id, ft);
      continue;
    }
    auto loc = f.Locate(gpd::kTitle, title_id);
    if (!loc || loc->second < 0x28) {
      // Jeu jamais joué (absent du GPD du Dashboard) : date gardée à part, lue par la liste des jeux joués.
      achievements::MarkPlayed(title_id, ft);
      REXLOG_INFO("[dash] {:08X} absent de {} : dernier lancement noté dans library_played.txt", title_id, path.string());
      continue;
    }
    auto& b = f.Bytes();
    for (int i = 0; i < 8; i++) b[loc->first + 32 + i] = uint8_t(ft >> (56 - 8 * i));
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    REXLOG_INFO("[dash] {:08X} : dernier lancement mis à jour dans {}", title_id, path.string());
  }
}

// ---- Bibliothèque de jeux : init + scan au démarrage (tools/game_library.py) ------------------------------------------
// Le dossier de jeux est créé au premier lancement (`init`), puis `scan` met à jour games.auto.toml (cache incrémental : une
// fraction de seconde si rien n'a changé). Au démarrage on attend au plus quelques secondes ; au-delà le scan continue en
// arrière-plan et le registre est relu à sa fin (Games() surveille la date du fichier). F6 relance un scan.
namespace {

std::string JsonFieldFwd(const std::string& j, const std::string& key);
std::string ToolsRootStr();

std::mutex g_lib_mutex;
LibraryStatus g_lib;

// Exécute une commande sans shell ; arrête-la après timeout_s secondes (0 = pas de limite). Renvoie vrai si elle a fini
// avant le délai, code de retour dans *rc, sortie standard (tronquée) dans *out.
bool RunWithTimeout(const std::vector<std::string>& argv, int timeout_s, int* rc, std::string* out = nullptr) {
  int fds[2];
  if (pipe(fds) != 0) return false;
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    dup2(fds[1], 1);
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 2);
    close(fds[0]);
    close(fds[1]);
    std::vector<char*> a;
    for (auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  close(fds[1]);
  fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
  auto t0 = std::chrono::steady_clock::now();
  std::string buf;
  int st = 0;
  bool done = false;
  for (;;) {
    char tmp[4096];
    ssize_t n;
    while ((n = read(fds[0], tmp, sizeof(tmp))) > 0) buf.append(tmp, size_t(n));
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
      done = true;
      while ((n = read(fds[0], tmp, sizeof(tmp))) > 0) buf.append(tmp, size_t(n));
      break;
    }
    if (timeout_s > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(timeout_s)) {
      kill(-pid, SIGKILL);
      waitpid(pid, &st, 0);
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  close(fds[0]);
  if (rc) *rc = done && WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  if (out) *out = buf;
  return done;
}

int CountLibraryGames() {
  int n = 0;
  for (auto& g : Games())
    if (g.from_library) n++;
  return n;
}

void ScanWorker(int timeout_s, bool init_first) {
  const std::string tool = (fs::path(ToolsRootStr()) / "tools" / "game_library.py").string();
  const std::string out_dir = AutoRegistryPath().parent_path().string();
  int rc = 0;
  std::string out;
  if (init_first) {
    RunWithTimeout({"python3", "-I", tool, "init", "--json"}, 10, &rc, &out);
    std::string dir = JsonFieldFwd(out, "games_dir");
    if (!dir.empty()) {
      std::lock_guard lock(g_lib_mutex);
      g_lib.games_dir = dir;
    }
  }
  bool done = RunWithTimeout({"python3", "-I", tool, "scan", "--out", out_dir}, timeout_s, &rc);
  std::lock_guard lock(g_lib_mutex);
  g_lib.scanning = false;
  g_lib.done_at = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  g_lib.count = CountLibraryGames();
  g_lib.message = !done ? "délai dépassé" : rc != 0 ? "échec du scan (code " + std::to_string(rc) + ")" : "terminé";
  REXLOG_INFO("[dash] bibliothèque : scan {} ({} jeux, dossier {})", g_lib.message, g_lib.count, g_lib.games_dir);
}

}  // namespace

LibraryStatus GetLibraryStatus() {
  std::lock_guard lock(g_lib_mutex);
  return g_lib;
}

void RescanLibrary(bool wait, int timeout_s) {
  {
    std::lock_guard lock(g_lib_mutex);
    if (g_lib.scanning) return;
    g_lib.scanning = true;
    g_lib.message = "scan en cours";
  }
  if (!fs::exists(fs::path(ToolsRootStr()) / "tools" / "game_library.py")) {
    std::lock_guard lock(g_lib_mutex);
    g_lib.scanning = false;
    g_lib.message = "game_library.py introuvable (XENONDASH_ROOT)";
    REXLOG_WARN("[dash] bibliothèque : {}", g_lib.message);
    return;
  }
  const bool first = g_lib.games_dir.empty();
  if (wait) {
    // Démarrage : délai court ; au-delà, le scan se termine en arrière-plan sans limite.
    std::thread([first, timeout_s] {
      ScanWorker(timeout_s, first);
    }).join();
    {
      std::lock_guard lock(g_lib_mutex);
      if (g_lib.message == "délai dépassé") {
        g_lib.scanning = true;
        std::thread([] { ScanWorker(0, false); }).detach();
      }
    }
  } else {
    std::thread([first] { ScanWorker(0, first); }).detach();
  }
}

// ---- Jeux à recette : construction à la demande (docs/recettes.md) ----------------------------------------------------
// `recipe = "<source>"` dans games.toml : au premier lancement, tools/recipe_build.py identifie le XEX, le recompile en natif
// (cache ~/.local/share/xenondash/games/<TitleID>/<clé>/) puis le Dashboard lance le binaire du cache comme un jeu natif.
// Le Dashboard reste vivant pendant la construction (écran de progression ImGui, journal, notify-send s'il existe) ;
// la construction est un processus détaché (elle survit à une fermeture du Dashboard) ; à sa fin le jeu est lancé.
namespace {

std::mutex g_build_mutex;
BuildStatus g_build;

std::string ToolsRoot() {
  if (const char* e = std::getenv("XENONDASH_ROOT")) return e;
#ifdef XENONDASH_ROOT_DIR
  return XENONDASH_ROOT_DIR;
#else
  return ".";
#endif
}

std::string ToolsRootStr() { return ToolsRoot(); }

std::string RecipeTool() { return (fs::path(ToolsRoot()) / "tools" / "recipe_build.py").string(); }

// Champ d'un JSON plat produit par recipe_build.py (chaîne, nombre ou booléen) ; vide s'il est absent.
std::string JsonField(const std::string& j, const std::string& key) {
  auto p = j.find("\"" + key + "\"");
  if (p == std::string::npos) return {};
  p = j.find(':', p);
  if (p == std::string::npos) return {};
  p++;
  while (p < j.size() && (j[p] == ' ' || j[p] == '\n')) p++;
  if (p < j.size() && j[p] == '"') {
    std::string v;
    for (p++; p < j.size() && j[p] != '"'; p++) {
      if (j[p] == '\\' && p + 1 < j.size()) {
        p++;
        v += j[p] == 'n' ? '\n' : j[p];
      } else {
        v += j[p];
      }
    }
    return v;
  }
  size_t e = p;
  while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != '\n') e++;
  return j.substr(p, e - p);
}

std::string JsonFieldFwd(const std::string& j, const std::string& key) { return JsonField(j, key); }

void Notify(const std::string& title, const std::string& body) {
  REXLOG_INFO("[dash] {} : {}", title, body);
  // notify-send s'il existe (sans shell : arguments séparés) ; échec silencieux
  if (pid_t p = fork(); p == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    dup2(devnull, 1);
    dup2(devnull, 2);
    execlp("notify-send", "notify-send", "-a", "XenonDash", title.c_str(), body.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  } else if (p > 0) {
    std::thread([p] { int st; waitpid(p, &st, 0); }).detach();
  }
}

// Pendant une construction (ou après un refus) le hub NXE reste noir : il a pris l'appel de lancement pour un lancement réel (il
// n'attend pas de retour). Le Dashboard est donc relancé tout de suite ; l'état est passé à la nouvelle instance par
// <données>/current_build.json (construction en cours : pid, cache ; ou message d'échec à afficher une fois).
fs::path BuildStatePath() { return AutoRegistryPath().parent_path().parent_path() / "current_build.json"; }

std::string JsonEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else o += c;
  }
  return o;
}

void SaveBuildState(const std::string& state, const Game& g, long pid, const std::string& cache, const std::string& text,
                    const std::string& log_path) {
  std::error_code ec;
  fs::create_directories(BuildStatePath().parent_path(), ec);
  std::ofstream f(BuildStatePath(), std::ios::trunc);
  f << "{\"state\": \"" << state << "\", \"title_id\": \"" << fmt::format("{:08X}", g.title_id) << "\", \"name\": \""
    << JsonEsc(g.name) << "\", \"recipe\": \"" << JsonEsc(g.recipe) << "\", \"cache\": \"" << JsonEsc(cache)
    << "\", \"pid\": " << pid << ", \"error\": \"" << JsonEsc(text) << "\", \"log\": \"" << JsonEsc(log_path) << "\"}\n";
}

// Relance le Dashboard à l'identique (superviseur détaché puis sortie), sans jeu.
void RestartDashboard(const char* why) {
  std::string exe = SelfExe();
  auto args = SelfArgs();
  if (exe.empty() || args.empty()) return;
  std::string cwd0 = fs::current_path().string();
  std::string script = "sleep 1; cd " + Q(cwd0) + "; DASH_RETURNED_FROM=" + why + "; export DASH_RETURNED_FROM; " +
                       RelaunchCommand(exe, args);
  REXLOG_INFO("[dash] relance du Dashboard ({})", why);
  rex::FlushLogging();
  if (fork() == 0) {
    setsid();
    long maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (maxfd > 4096 ? 4096 : maxfd); fd++) close(fd);
    execl("/bin/sh", "sh", "-c", script.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  rex::FlushLogging();
  std::_Exit(0);
}

thread_local bool tl_hub_call = false;  // vrai pendant un appel de lancement venu du hub (écran noir ensuite)

// Refus ou échec : message à l'écran (fenêtre de progression, Échap la ferme), le Dashboard reste utilisable (les fonctions de
// lancement renvoient une erreur au Dashboard).
void FailBuild(const Game& g, const std::string& msg, const std::string& log_path, bool restart = true) {
  REXLOG_WARN("[dash] {} : {}", g.name, msg);
  {
    std::lock_guard<std::mutex> lock(g_build_mutex);
    g_build = BuildStatus{};
    g_build.failed = true;
    g_build.title_id = g.title_id;
    g_build.name = g.name;
    g_build.error = msg;
    g_build.log_path = log_path;
  }
  if (restart) {  // le hub est resté noir : nouvelle instance, qui affiche ce message
    SaveBuildState("failed", g, 0, "", msg, log_path);
    RestartDashboard("build_failed");
  } else {
    std::error_code ec;
    fs::remove(BuildStatePath(), ec);
  }
}

// Outils de compilation de l'hôte absents (clang, cmake, ninja) : noms des paquets à installer.
std::string MissingToolchain() {
  auto have = [](const char* exe) {
    const char* path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin";
    size_t i = 0;
    while (i <= p.size()) {
      size_t e = p.find(':', i);
      if (e == std::string::npos) e = p.size();
      fs::path c = fs::path(p.substr(i, e - i)) / exe;
      if (access(c.c_str(), X_OK) == 0) return true;
      i = e + 1;
    }
    return false;
  };
  std::string missing;
  for (const char* t : {"clang", "cmake", "ninja"})
    if (!have(t)) missing += std::string(missing.empty() ? "" : ", ") + t;
  if (missing.empty()) return {};
  return "Outils de compilation manquants : " + missing +
         ". Installer : Arch : pacman -S clang cmake ninja | Debian/Ubuntu : apt install clang cmake ninja-build | "
         "Fedora : dnf install clang cmake ninja-build.";
}

// Suit la construction (processus enfant de ce Dashboard, ou orphelin repris après une relance) jusqu'à sa fin.
void BuildMonitor(Game g, long pid, std::string cache, bool restart_on_fail) {
  const fs::path progress = fs::path(cache) / "progress.json";
  auto last_log = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  for (;;) {
    int st = 0;
    pid_t r = waitpid(pid_t(pid), &st, WNOHANG);  // enfant : le récolte ; sinon -1
    const bool alive = r == 0 || (r < 0 && kill(pid_t(pid), 0) == 0);
    std::string j;
    {
      std::ifstream f(progress);
      j.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    {
      std::lock_guard<std::mutex> lock(g_build_mutex);
      g_build.stage = JsonField(j, "stage");
      g_build.message = JsonField(j, "message");
      g_build.percent = std::atof(JsonField(j, "percent").c_str());
      g_build.elapsed = std::atof(JsonField(j, "elapsed_s").c_str());
      g_build.estimate = std::atof(JsonField(j, "estimate_s").c_str());
      g_build.index = std::atoi(JsonField(j, "index").c_str());
      g_build.total = std::atoi(JsonField(j, "total").c_str());
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(5)) {
      last_log = now;
      REXLOG_INFO("[dash] construction de {} : {:.0f} %  [{}] {}  ({:.0f} s)", g.name, g_build.percent, g_build.stage,
                  g_build.message, g_build.elapsed);
    }
    if (!alive) break;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  std::string j;
  {
    std::ifstream f(progress);
    j.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  std::string err = JsonField(j, "error");
  const bool ok = JsonField(j, "done") == "true" && err.empty();
  if (!ok) {
    if (err.empty()) err = "la construction s'est arrêtée avant la fin ; voir le journal";
    Notify("Construction de " + g.name + " échouée", err);
    FailBuild(g, err, cache + "/build.log", restart_on_fail);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_build_mutex);
    g_build.active = false;
    g_build.percent = 100.0;
  }
  std::error_code ec;
  fs::remove(BuildStatePath(), ec);
  Notify(g.name + " est prêt", "lancement du jeu");
  LaunchGame(g);  // le cache est construit : lancement normal (le Dashboard cède la place, puis revient)
}

// Prépare un jeu à recette : renvoie vrai si son binaire est prêt (g.command/g.cwd remplis), faux si une construction a
// été lancée (ou refusée/échouée : message à l'écran, le Dashboard continue).
bool PrepareRecipeGame(Game& g) {
  {
    std::lock_guard<std::mutex> lock(g_build_mutex);
    if (g_build.active) {
      REXLOG_INFO("[dash] une construction est déjà en cours ({})", g_build.name);
      return false;
    }
  }
  int rc = 0;
  const std::string tool = RecipeTool();
  std::string info;
  RunWithTimeout({"python3", "-I", tool, "launch-info", g.recipe}, 120, &rc, &info);
  const std::string supported = JsonField(info, "supported");
  if (rc != 0 || info.empty() || supported == "false") {
    std::string reason = JsonField(info, "reason");
    if (reason.empty()) reason = "source ou recette inutilisable (" + g.recipe + ")";
    else if (supported == "false") reason = "Ce jeu n'est pas pris en charge : " + reason;
    Notify("Impossible de lancer " + g.name, reason);
    FailBuild(g, reason, "", tl_hub_call);
    return false;
  }
  const std::string cache = JsonField(info, "cache");
  if (JsonField(info, "built") == "true") {
    g.command = JsonField(info, "native");
    g.cwd = JsonField(info, "cwd");
    g.kind = "native";
    REXLOG_INFO("[dash] {} : binaire du cache {}", g.name, g.cwd);
    return true;
  }
  if (std::string miss = MissingToolchain(); !miss.empty()) {
    Notify("Impossible de recompiler " + g.name, miss);
    FailBuild(g, miss, "", tl_hub_call);
    return false;
  }
  REXLOG_INFO("[dash] {} : pas de binaire en cache, construction (recette) : {}", g.name, cache);
  std::error_code ec;
  fs::create_directories(cache, ec);
  const std::string log_path = cache + "/build.log";
  pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int devnull = open("/dev/null", O_RDONLY);
    dup2(devnull, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    long maxfd = sysconf(_SC_OPEN_MAX);
    for (int f = 3; f < (maxfd > 4096 ? 4096 : maxfd); f++) close(f);
    execlp("python3", "python3", "-I", tool.c_str(), "build", g.recipe.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  {
    std::lock_guard<std::mutex> lock(g_build_mutex);
    g_build = BuildStatus{};
    g_build.active = true;
    g_build.title_id = g.title_id;
    g_build.name = g.name;
    g_build.log_path = log_path;
    g_build.message = "démarrage";
    g_build.stage = "identify";
  }
  Notify("Construction de " + g.name, "première exécution : recompilation du jeu en natif (plusieurs dizaines de minutes)");
  SaveBuildState("running", g, pid, cache, "", log_path);
  if (tl_hub_call) {
    // Le hub est noir : nouvelle instance du Dashboard, qui reprend le suivi de la construction (ResumeBuild).
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    RestartDashboard("build_started");
  }
  std::thread(BuildMonitor, g, long(pid), cache, false).detach();
  return false;
}

}  // namespace

BuildStatus GetBuildStatus() {
  std::lock_guard<std::mutex> lock(g_build_mutex);
  return g_build;
}

bool LaunchGame(const Game& g0) {
  Game g = g0;
  if (!g.recipe.empty() && g.command.empty() && !PrepareRecipeGame(g)) return true;  // construction en cours : le Dashboard continue
  if (g_launching.exchange(true)) return true;  // déjà en cours
  TouchLastPlayed(g.title_id);
  std::string exe = SelfExe();
  auto args = SelfArgs();
  if (exe.empty() || args.empty()) {
    REXLOG_ERROR("[dash] lancement impossible : exécutable du Dashboard introuvable");
    g_launching = false;
    return false;
  }
  std::string cwd0 = fs::current_path().string();
  std::string relaunch = RelaunchCommand(exe, args);
  std::string game_cwd = g.cwd.empty() ? cwd0 : g.cwd;
  // Le jeu s'exécute dans un sous-shell (son environnement, son répertoire) ; le Dashboard est relancé
  // ensuite avec l'environnement d'origine (LD_LIBRARY_PATH compris).
  std::string script = fmt::format(
      "sleep 1; ( {}cd {} || cd {}; {} ) ; rc=$? ; echo \"[dash] jeu {:08X} termine (code $rc), retour au Dashboard\" >&2; "
      "cd {}; DASH_RETURNED_FROM={:08X}; export DASH_RETURNED_FROM; {}",
      g.keep_ld ? "" : "unset LD_LIBRARY_PATH; ", Q(game_cwd), Q(cwd0), g.command, g.title_id, cwd0, g.title_id,
      relaunch);
  REXLOG_INFO("[dash] LANCEMENT de {:08X} « {} » ({}) : {}  [cwd={}]", g.title_id, g.name, g.kind, g.command, game_cwd);
  REXLOG_INFO("[dash] superviseur : sh -c \"{}\"", script);
  rex::FlushLogging();

  pid_t pid = fork();
  if (pid < 0) {
    REXLOG_ERROR("[dash] fork a échoué : {}", std::strerror(errno));
    g_launching = false;
    return false;
  }
  if (pid == 0) {
    setsid();
    // Fermer tous les descripteurs hérités (GPU, audio, sockets) sauf 0..2.
    long maxfd = sysconf(_SC_OPEN_MAX);
    for (int fd = 3; fd < (maxfd > 4096 ? 4096 : maxfd); fd++) close(fd);
    execl("/bin/sh", "sh", "-c", script.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  // Le Dashboard cède la place (libère GPU/audio/fenêtre) : le superviseur le relancera.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  rex::FlushLogging();
  std::_Exit(0);
}

namespace {

std::string GuestStr(uint8_t* base, uint32_t addr) {
  if (!addr) return "<NULL>";
  std::string s;
  for (int i = 0; i < 260 && base[addr + i]; i++) s.push_back(char(base[addr + i]));
  return s;
}

// Titre à lancer pour un chemin de lancement du Dashboard : chemin vide/NULL = le disque, sinon on cherche
// un TitleID (8 chiffres hexadécimaux) dans le chemin, puis un jeu dont le nom figure dans le chemin.
std::optional<Game> Resolve(const std::string& path) {
  if (path.empty() || path == "<NULL>") return DiscGame();
  for (size_t i = 0; i + 8 <= path.size(); i++) {
    bool hex = true;
    for (size_t j = 0; j < 8 && hex; j++) hex = std::isxdigit(static_cast<unsigned char>(path[i + j])) != 0;
    if (hex && (i == 0 || !std::isxdigit(static_cast<unsigned char>(path[i - 1]))) &&
        (i + 8 == path.size() || !std::isxdigit(static_cast<unsigned char>(path[i + 8])))) {
      if (auto g = Find(ParseHex(path.substr(i, 8)))) return g;
    }
  }
  return DiscGame();
}

std::string HexDump(const std::vector<uint8_t>& v, size_t n = 64) {
  std::string s;
  for (size_t i = 0; i < v.size() && i < n; i++) s += fmt::format("{:02x}", v[i]);
  return s;
}

void LogLaunchData() {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex");
  if (!xam) return;
  auto& ld = xam->loader_data();
  REXLOG_INFO("[dash] données de lancement ({} octets) : {}", ld.launch_data.size(), HexDump(ld.launch_data));
}

bool OnLaunchRequest(const char* who, const std::string& path, uint32_t flags) {
  REXLOG_INFO("[dash] {}(chemin='{}', indicateurs={:#x})", who, path, flags);
  LogLaunchData();
  auto g = Resolve(path);
  if (!g) {
    REXLOG_WARN("[dash] {} : aucun jeu correspondant dans la bibliothèque, lancement ignoré", who);
    return false;
  }
  tl_hub_call = true;
  LaunchGame(*g);  // ne revient pas si le jeu est lancé ; sinon (construction, refus) le Dashboard reçoit une erreur
  tl_hub_call = false;
  return false;
}

}  // namespace

// Reprise après la relance du Dashboard (hub noir pendant une construction, ou refus à afficher) : relit current_build.json.
void ResumeBuild() {
  std::string j;
  {
    std::ifstream f(BuildStatePath());
    if (!f) return;
    j.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  Game g;
  g.title_id = ParseHex(JsonField(j, "title_id"));
  g.name = JsonField(j, "name");
  g.recipe = JsonField(j, "recipe");
  g.kind = "recipe";
  for (auto& x : Games())
    if (x.title_id == g.title_id) g = x;  // champs complets du registre (icône, type...)
  const std::string state = JsonField(j, "state"), cache = JsonField(j, "cache");
  const long pid = std::atol(JsonField(j, "pid").c_str());
  std::error_code ec;
  if (state == "failed") {
    FailBuild(g, JsonField(j, "error"), JsonField(j, "log"), false);  // supprime le fichier : message affiché une fois
  } else if (state == "running" && pid > 0 && kill(pid_t(pid), 0) == 0) {
    {
      std::lock_guard<std::mutex> lock(g_build_mutex);
      g_build = BuildStatus{};
      g_build.active = true;
      g_build.title_id = g.title_id;
      g_build.name = g.name;
      g_build.log_path = cache + "/build.log";
      g_build.message = "reprise du suivi";
    }
    REXLOG_INFO("[dash] reprise du suivi de la construction de {} (pid {})", g.name, pid);
    std::thread(BuildMonitor, g, pid, cache, false).detach();
  } else {
    fs::remove(BuildStatePath(), ec);  // construction terminée ou disparue : le lancement normal se charge du reste
  }
}

void Init() {
  ResumeBuild();
  if (const char* r = std::getenv("DASH_RETURNED_FROM"))
    REXLOG_INFO("[dash] retour au Dashboard après le jeu {}", r);
  // Bibliothèque : actif sous AppImage (XENONDASH_AUTOSCAN=1, posé par AppRun) ou avec --dash_library_scan ; sans cela un
  // lancement de développement ne crée ni dossier de jeux ni données utilisateur.
  const char* as = std::getenv("XENONDASH_AUTOSCAN");
  if (REXCVAR_GET(dash_library_scan) || (as && std::string(as) == "1")) RescanLibrary(true, 4);
  std::string t = REXCVAR_GET(dash_launch);
  if (t.empty()) return;
  uint32_t tid = ParseHex(t);
  std::thread([tid] {
    std::this_thread::sleep_for(std::chrono::seconds(std::max(0, REXCVAR_GET(dash_launch_delay))));
    if (auto g = Find(tid)) LaunchGame(*g);
    else REXLOG_ERROR("[dash] --dash_launch : TitleID {:08X} absent de games.toml", tid);
  }).detach();
}

}  // namespace dash::launch

// Les trois fonctions de lancement du Dashboard. Le Dashboard attend que XAM ne revienne pas : le processus
// quitte dans LaunchGame ; si aucun jeu ne correspond, on revient (le Dashboard continue).
REX_HOOK_RAW(__imp__XamLoaderLaunchTitle) {
  uint32_t path = ctx.r3.u32;
  dash::launch::OnLaunchRequest("XamLoaderLaunchTitle", dash::launch::GuestStr(base, path), ctx.r4.u32);
  ctx.r3.u64 = 0x80004005u;  // E_FAIL : le jeu n'a pas été lancé (construction en cours, refus ou jeu inconnu)
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitleEx) {
  REXLOG_INFO("[dash] XamLoaderLaunchTitleEx(r3={:#x} r4={:#x} r5={:#x} r6={:#x})", ctx.r3.u64, ctx.r4.u64,
              ctx.r5.u64, ctx.r6.u64);
  dash::launch::OnLaunchRequest("XamLoaderLaunchTitleEx", dash::launch::GuestStr(base, ctx.r3.u32), ctx.r6.u32);
  ctx.r3.u64 = 0x80004005u;  // E_FAIL : le jeu n'a pas été lancé (construction en cours, refus ou jeu inconnu)
}

// NXE (13604) / Metro (17559) : « Play Game » du disque passe par XamLoaderLaunchTitleForReason(chemin ou NULL, raison...)
// (NULL = lancer le disque). Même traitement que XamLoaderLaunchTitle.
REX_HOOK_RAW(__imp__XamLoaderLaunchTitleForReason) {
  REXLOG_INFO("[dash] XamLoaderLaunchTitleForReason(r3={:#x} r4={:#x} r5={:#x} r6={:#x})", ctx.r3.u64, ctx.r4.u64,
              ctx.r5.u64, ctx.r6.u64);
  dash::launch::OnLaunchRequest("XamLoaderLaunchTitleForReason", dash::launch::GuestStr(base, ctx.r3.u32), ctx.r4.u32);
  ctx.r3.u64 = 0x80004005u;  // E_FAIL : le jeu n'a pas été lancé (construction en cours, refus ou jeu inconnu)
}

// XamLoaderGetMediaInfo(&type, &titre) : type de média (0 aucun, 1 disque de jeu Xbox 360, 2 Xbox d'origine...) et
// TitleID du disque (le Dashboard l'appelle en démarrant et pour remplir le bouton de disque).
REX_HOOK_RAW(__imp__XamLoaderGetMediaInfo) {
  auto d = dash::launch::InsertedDisc();
  uint32_t type = d ? 1 : 0, title = d ? d->title_id : 0;
  auto put = [&](uint32_t addr, uint32_t v) {
    if (addr) *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(v);
  };
  put(ctx.r3.u32, type);
  put(ctx.r4.u32, title);
  REXLOG_INFO("[dash] XamLoaderGetMediaInfo -> type={} titre={:08X}", type, title);
  ctx.r3.u64 = 0;
}

// NXE : XamLoaderGetMediaInfoEx(&a, &b, &c) était un stub (sorties laissées à la valeur de la pile). Le hub l'interroge avant
// toute action sur le disque (tuile « Play », A, Eject) : la 3e sortie est comparée à 0 et -2 (« pas de média »). Ici : média
// présent (a = 1 : disque de jeu Xbox 360, b = TitleID du disque) et c = 0 (prêt) ; sans disque a = b = 0 (c = 0 aussi : 0xFFFFFFFE faisait traiter la Games Library comme vide).
REX_HOOK_RAW(__imp__XamLoaderGetMediaInfoEx) {
  auto d = dash::launch::InsertedDisc();
  auto put = [&](uint32_t addr, uint32_t v) {
    if (addr) *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(v);
  };
  put(ctx.r3.u32, d ? 1 : 0);
  put(ctx.r4.u32, d ? d->title_id : 0);
  put(ctx.r5.u32, 0);
  ctx.r3.u64 = 0;
}
