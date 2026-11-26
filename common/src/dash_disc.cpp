// dash - icônes de la bibliothèque et notification « état du média ».
//
// La simulation de disque est SUPPRIMÉE (demande de l'utilisateur, 2026-11-21) : le lecteur ne réagira qu'à un
// véritable lecteur optique (à câbler plus tard sur \Device\CdRom0). Aucun jeu ne peut plus être présenté comme
// disque inséré (`disc = true` de games.toml et --dash_disc retirés ; XamLoaderGetMediaInfo/Ex renvoient
// « aucun média », XamLoaderGetDvdTrayState un plateau fermé sans disque). Ce qui reste ici :
//   - les icônes de la bibliothèque (IconFor : PNG `icon` ou image 0x8000 d'un GPD/SPA du profil local) ;
//   - la notification système 0x8000000D (type de média) rediffusée à chaque création d'écouteur, paramètre 0
//     (« aucun média ») — sans elle, l'état de XAM reste indéterminé (« Unplayable Disc » relevé dans le code
//     recompilé, voir docs/technique.md).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "dash_gpd.h"
#include "dash_launch.h"

namespace rex::kernel::xam {
uint32_t xeXamNotifyCreateListener(uint64_t mask, uint32_t is_system, uint32_t max_version);
}

REXCVAR_DECLARE(std::string, game_data_root);

namespace dash::disc {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kXnMediaChanged = 0x8000000D;  // paramètre : type de média (0 = aucun)

// Icône : `icon` (PNG) ou image 0x8000 d'un GPD/SPA (`spa`, sinon <TitleID>.gpd du profil local).
std::vector<uint8_t> LoadIcon(const launch::Game& g) {
  auto rel = [](const std::string& p) {
    fs::path x(p);
    return x.is_absolute() ? x : fs::absolute(REXCVAR_GET(game_data_root)) / x;
  };
  if (!g.icon.empty()) {
    std::ifstream f(rel(g.icon), std::ios::binary);
    if (f) return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  }
  std::vector<fs::path> cand;
  if (!g.spa.empty()) cand.push_back(rel(g.spa));
  std::error_code dir_ec;  // sans dossier « profiles » (premier lancement sans profil) : aucun candidat, pas d'exception
  for (auto& e : fs::directory_iterator(fs::absolute(REXCVAR_GET(game_data_root)) / "profiles", fs::directory_options::skip_permission_denied, dir_ec)) {
    if (e.is_directory()) cand.push_back(e.path() / fmt::format("{:08X}.gpd", g.title_id));
  }
  for (auto& c : cand) {
    gpd::File f;
    std::error_code ec;
    if (fs::exists(c, ec) && f.Load(c))
      if (auto img = f.Get(gpd::kImage, gpd::kTitleImageId)) return *img;
  }
  return {};
}

std::atomic<uint64_t> g_generation{0};

}  // namespace

// Une seule diffusion par exécution : sur la console, la notification accompagne l'insertion du disque ; la
// rediffuser plus tard (écouteur créé par la Games Library...) serait prise pour une nouvelle insertion.
std::atomic<bool> g_sent{false};

void ScheduleMediaNotification() {
  if (g_sent.load()) return;
  uint64_t gen = ++g_generation;
  std::thread([gen] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    if (gen != g_generation.load() || g_sent.exchange(true)) return;  // un écouteur plus récent reprogramme
    uint32_t id = kXnMediaChanged, param = 0;  // 0 = aucun média : le lecteur est vide (pas de disque simulé)
    if (const char* e = std::getenv("DASH_MEDIA_NOTIFY")) std::sscanf(e, "%x:%u", &id, &param);  // essais : id:param
    REXLOG_INFO("[dash] notification de média (0x{:08X}, paramètre {})", id, param);
    rex::system::kernel_state()->BroadcastNotification(rex::system::XNotificationID(id), param);
  }).detach();
}

std::vector<uint8_t> IconFor(const launch::Game& g) { return LoadIcon(g); }

void Init() {
  // Plus rien à monter : la simulation de disque est supprimée (voir l'en-tête). La notification « aucun média »
  // part via ScheduleMediaNotification quand XAM crée ses écouteurs.
}

}  // namespace dash::disc

// XexTransformImageKey : déchiffrement de la clé de fichier du XEX ; les XEX non chiffrés (jeux des recettes)
// réussissent sans déchiffrement.
REX_HOOK_RAW(__imp__XexTransformImageKey) {
  (void)base;
  ctx.r3.u64 = 0;
}

// Un écouteur système vient d'être créé : il n'a pas vu la notification de média -> la rediffuser.
REX_HOOK_RAW(__imp__XamNotifyCreateListenerInternal) {
  (void)base;
  uint64_t mask = ctx.r3.u64;
  { static int _a=0; if (++_a<=4) REXLOG_WARN("[gate] XamNotifyCreateListenerInternal(mask=0x{:016X}, is_system={}, max_version={})", mask, ctx.r4.u32, ctx.r5.u32); }
  ctx.r3.u64 = rex::kernel::xam::xeXamNotifyCreateListener(mask, ctx.r4.u32, ctx.r5.u32);
  if ((mask & 0x23) == 0x23) dash::disc::ScheduleMediaNotification();
}

REX_HOOK_RAW(__imp__XamNotifyCreateListener) {
  (void)base;
  uint64_t mask = ctx.r3.u64;
  ctx.r3.u64 = rex::kernel::xam::xeXamNotifyCreateListener(mask, 0, ctx.r4.u32);
  if ((mask & 0x23) == 0x23) dash::disc::ScheduleMediaNotification();
}

// XMsgStartIORequest(Ex) : routés par dash_xam_redirect.cpp (XMP vers XAM recompilé, le reste au HLE du SDK). L'ancienne
// réponse « nom + icône du disque » de l'application 0xFB a disparu avec la simulation de disque.
