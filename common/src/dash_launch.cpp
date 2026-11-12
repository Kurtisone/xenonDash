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
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
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

REXCVAR_DECLARE(std::string, game_data_root);

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

std::vector<Game> Games() {
  std::vector<Game> out;
  toml::table root;
  try {
    root = toml::parse_file(RegistryPath().string());
  } catch (const toml::parse_error& e) {
    if (fs::exists(RegistryPath())) REXLOG_WARN("[dash] games.toml illisible : {}", e.description());
    return out;
  }
  std::string xenia_cmd = XeniaCommand(root);
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
    g.name = (*t)["name"].value_or(k);
    g.cwd = (*t)["cwd"].value_or(std::string());
    g.keep_ld = (*t)["keep_ld_library_path"].value_or(false);
    if (auto n = (*t)["native"].value<std::string>()) {
      g.command = *n;
      g.kind = "native";
    } else if (auto x = (*t)["xenia"].value<std::string>()) {
      g.command = xenia_cmd + " " + Q(*x);
      g.kind = "xenia";
    } else {
      REXLOG_WARN("[dash] games.toml : {} n'a ni `native` ni `xenia`", k);
      continue;
    }
    g.disc = (*t)["disc"].value_or(false);
    g.icon = (*t)["icon"].value_or(std::string());
    g.spa = (*t)["spa"].value_or(std::string());
    if (auto ty = (*t)["type"].value<std::string>()) g.content_type = TypeFromName(*ty);
    else if (auto ti = (*t)["type"].value<int64_t>()) g.content_type = uint32_t(*ti);
    out.push_back(std::move(g));
  }
  return out;
}

std::optional<Game> Find(uint32_t title_id) {
  for (auto& g : Games())
    if (g.title_id == title_id) return g;
  return std::nullopt;
}

// `library = true` à la racine de games.toml : les jeux apparaissent aussi comme contenu installé (voir README).
bool LibraryEnabled() {
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
  return std::nullopt;
}

namespace {

std::atomic<bool> g_launching{false};

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
    if (!d.is_directory() || !fs::exists(path, ec) || !f.Load(path)) continue;
    auto loc = f.Locate(gpd::kTitle, title_id);
    if (!loc || loc->second < 0x28) {
      REXLOG_INFO("[dash] {:08X} absent de {} : date de dernier lancement non mise à jour", title_id, path.string());
      continue;
    }
    auto& b = f.Bytes();
    for (int i = 0; i < 8; i++) b[loc->first + 32 + i] = uint8_t(ft >> (56 - 8 * i));
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
    REXLOG_INFO("[dash] {:08X} : dernier lancement mis à jour dans {}", title_id, path.string());
  }
}

bool LaunchGame(const Game& g) {
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
  // Une seule ligne de commande pour le Dashboard relancé : l'exécutable réel + arguments d'origine.
  std::string relaunch = "exec " + Q(exe);
  for (size_t i = 1; i < args.size(); i++) {
    // --dash_launch(=...) / --dash_launch_delay : lancement de test, à ne pas rejouer au retour (boucle sans fin).
    if (args[i].rfind("--dash_launch", 0) == 0) {
      if (args[i].find('=') == std::string::npos && i + 1 < args.size()) i++;
      continue;
    }
    relaunch += " " + Q(args[i]);
  }
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

void OnLaunchRequest(const char* who, const std::string& path, uint32_t flags) {
  REXLOG_INFO("[dash] {}(chemin='{}', indicateurs={:#x})", who, path, flags);
  LogLaunchData();
  auto g = Resolve(path);
  if (!g) {
    REXLOG_WARN("[dash] {} : aucun jeu correspondant dans games.toml, lancement ignoré", who);
    return;
  }
  LaunchGame(*g);
}

}  // namespace

void Init() {
  if (const char* r = std::getenv("DASH_RETURNED_FROM"))
    REXLOG_INFO("[dash] retour au Dashboard après le jeu {}", r);
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
}

REX_HOOK_RAW(__imp__XamLoaderLaunchTitleEx) {
  REXLOG_INFO("[dash] XamLoaderLaunchTitleEx(r3={:#x} r4={:#x} r5={:#x} r6={:#x})", ctx.r3.u64, ctx.r4.u64,
              ctx.r5.u64, ctx.r6.u64);
  dash::launch::OnLaunchRequest("XamLoaderLaunchTitleEx", dash::launch::GuestStr(base, ctx.r3.u32), ctx.r6.u32);
}

// NXE (13604) / Metro (17559) : « Play Game » du disque passe par XamLoaderLaunchTitleForReason(chemin ou NULL, raison...)
// (NULL = lancer le disque). Même traitement que XamLoaderLaunchTitle.
REX_HOOK_RAW(__imp__XamLoaderLaunchTitleForReason) {
  REXLOG_INFO("[dash] XamLoaderLaunchTitleForReason(r3={:#x} r4={:#x} r5={:#x} r6={:#x})", ctx.r3.u64, ctx.r4.u64,
              ctx.r5.u64, ctx.r6.u64);
  dash::launch::OnLaunchRequest("XamLoaderLaunchTitleForReason", dash::launch::GuestStr(base, ctx.r3.u32), ctx.r4.u32);
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
