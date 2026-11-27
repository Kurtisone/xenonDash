// xenondash common - redirection des imports XAM du Dashboard vers XAM recompilé (voir dash_xam_redirect.h).

#include "dash_launch.h"
#include "dash_session.h"
#include "dash_xam_redirect.h"
#include "dash_xam_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#if !defined(_WIN32)
#include <dlfcn.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xex_module.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

namespace ngpu {
// Vrai tant que le HUD de XAM (boite de message, clavier...) a quelque chose a montrer (native_gpu/src/hud_front.cpp, palier N2) ;
// symbole faible : absent des executables sans rendu natif du HUD.
bool HudActive() __attribute__((weak));
}  // namespace ngpu

namespace dash::xamredir {

// Chaîne UTF-16 big-endian dans le tas système invité ; renvoie son adresse invitée (0 si le noyau n'est pas prêt).
uint32_t PutWide(const char16_t* s) {
  const size_t n = std::char_traits<char16_t>::length(s) + 1;  // avec le zéro final
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(n * 2));
  auto* dst = reinterpret_cast<uint16_t*>(REX_KERNEL_MEMORY()->virtual_membase() + a);
  for (size_t i = 0; i < n; ++i) dst[i] = __builtin_bswap16(static_cast<uint16_t>(s[i]));
  return a;
}

namespace {

// Un import : nom sans préfixe, HLE du SDK (résolu à la demande), export de XAM recompilé (si redirigé).
struct Slot {
  explicit Slot(const char* n);

  const char* name;
  std::atomic<PPCFunc*> hle{nullptr};
  std::atomic<PPCFunc*> guest{nullptr};
  std::atomic<uint32_t> guest_addr{0};
  std::atomic<uint64_t> calls{0};
};

std::vector<Slot*>& Slots() {
  static std::vector<Slot*> slots;
  return slots;
}

Slot::Slot(const char* n) : name(n) { Slots().push_back(this); }

Slot* FindSlot(std::string_view name) {
  for (Slot* s : Slots()) {
    if (name == s->name) return s;
  }
  return nullptr;
}

// HLE du SDK pour `sym` (ex. __imp__XGetLanguage). Sous Linux, la bibliothèque du SDK référence ses propres exports
// par la GOT : sa table de registre (rex::ppc::FindPPCFuncByName) pointe alors sur le hook de l'exécutable (interposition
// ELF), et le repli s'appellerait lui-même. dlsym(RTLD_NEXT) depuis l'exécutable renvoie la définition suivante, celle
// du SDK. Sous Windows, les DLL ne s'interposent pas : la table de registre suffit.
PPCFunc* FindHle(const std::string& sym) {
#if defined(_WIN32)
  return rex::ppc::FindPPCFuncByName(sym.c_str());
#else
  return reinterpret_cast<PPCFunc*>(dlsym(RTLD_NEXT, sym.c_str()));
#endif
}

void DispatchImpl(Slot& s, PPCContext& ctx, uint8_t* base);
void SyncHleProfileWithXam(const PPCContext& ctx, uint8_t* base, uint64_t wanted);
void SyncHleAvatarManifest(const PPCContext& ctx, uint8_t* base);
void NoteAvatarManifestChanged(uint32_t hr);
std::atomic<int> g_manifest_refresh{0};  // lectures du manifeste restant à tenter après XamAvatarSetManifest
std::atomic<rex::system::KernelState*> g_ks{nullptr};

// Exécute l'import : XAM recompilé si redirigé, sinon le HLE du SDK (même fonction que sans ce fichier).
// Trace : première exécution de chaque import (voie et compteur), puis tous les 1000 appels.
void Dispatch(Slot& s, PPCContext& ctx, uint8_t* base) {
  // Entrees : tant que le HUD est affiche (et jusqu'au relachement de tous les boutons ensuite), le Dashboard ne recoit plus rien, comme un
  // titre sous une interface systeme (sinon l'appui qui ferme la boite declenche aussi une action du Dashboard).
  static std::atomic<bool> wait_release{false};
  const bool is_state = std::string_view(s.name) == "XamInputGetState";
  const bool is_key = std::string_view(s.name) == "XamInputGetKeystrokeEx";
  if ((is_state || is_key) && ngpu::HudActive) {
    // Scene XAM ouverte = overlay visible ET masque « interfaces systeme actives » de XAM non nul (0x81BFCAE8 +0 : le portier
    // sub_8189F0D0 refuse toute ouverture tant qu'il est non nul, cf. ShowXamGuide). Le chrome permanent du HUD rend l'overlay
    // visible au repos : la visibilite seule ne suffit pas (mesure du 2026-11-22).
    // Même critère que le compositeur (hud_front.cpp, PublishOverlay) : masque +0 non nul (ouverture) ou pile de scènes +36
    // au-dessus du repos (XamSceneAboveRest : Guide ou boîte empilé sur la scène de boot ; le masque retombe à 0 Guide affiché).
    const uint32_t ui_mask = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + 0x81BFCAE8u));
    const bool visible = ngpu::HudActive();
    const bool hud = visible && (ui_mask != 0 || XamSceneAboveRest(base));
    if (hud) wait_release.store(true);
    if (std::getenv("DASH_HUD_INPUT_LOG")) {
      static std::atomic<int> prev{-1};
      const int now = (hud ? 1 : 0) | (wait_release.load() ? 2 : 0);
      if (prev.exchange(now) != now)
        std::fprintf(stderr, "[hud] entrees %s (overlay visible=%d masque UI=%08X attente relachement=%d, via %s)\n",
                     (now & 3) ? "CAPTEES" : "libres", int(visible), ui_mask, int(wait_release.load()), s.name);
    }
    if (hud || wait_release.load()) {
      if (is_key) {
        if (hud) {
          // La file de frappes du HLE (pilote clavier du SDK, jusqu'à 256 frappes) est vidée et jetée : XAM lit ses touches par
          // xinputd, pas par cette file. Laissées en attente, les frappes faites dans le Guide étaient livrées au Dashboard à sa
          // fermeture, dès que la première frappe relâchée levait l'attente : le A qui connecte le profil ouvrait ensuite la tuile
          // sélectionnée (System Settings, Getting Started ; mesure du 2026-11-25).
          const uint64_t user = ctx.r3.u64, flags = ctx.r4.u64, out = ctx.r5.u64;
          for (int i = 0; i < 256; ++i) {
            ctx.r3.u64 = user;
            ctx.r4.u64 = flags;
            ctx.r5.u64 = out;
            DispatchImpl(s, ctx, base);
            if (ctx.r3.u32 != 0) break;  // ERROR_EMPTY (ou manette absente) : file vide
          }
          if (out) std::memset(base + uint32_t(out), 0, 8);  // X_INPUT_KEYSTROKE : rien de lisible pour le Dashboard
          ctx.r3.u64 = 0x10D2;  // ERROR_EMPTY
          return;
        }
        // Apres la scene : la file de frappes du Dashboard est lue mais jetee jusqu'au relachement (KEYUP) ou file vide. Sans ce
        // chemin, l'attente ne se levait que par XamInputGetState, que le hub n'appelle presque pas : le hub restait sourd.
        DispatchImpl(s, ctx, base);
        const uint32_t out = ctx.r5.u32;  // XamInputGetKeystrokeEx(r3 = &utilisateur, r4 = drapeaux, r5 = X_INPUT_KEYSTROKE*)
        const bool empty = ctx.r3.u32 != 0;
        const bool keyup = !empty && out && (((base[out + 4] << 8) | base[out + 5]) & 0x0002);
        if (empty || keyup) wait_release.store(false);
        ctx.r3.u64 = 0x10D2;
        return;
      }
      const uint32_t out = ctx.r4.u32;
      DispatchImpl(s, ctx, base);  // le HLE remplit l'etat ; on le garde pour detecter le relachement
      if (out && ctx.r3.u32 == 0) {
        uint8_t* st = base + out;
        bool down = false;
        down = st[4] || st[5] || st[6] > 30 || st[7] > 30;  // boutons et gachettes (les sticks, qui derivent, ne comptent pas)
        if (!down && !hud) wait_release.store(false);
        std::memset(st + 4, 0, 12);
      }
      return;
    }
  }
  // Connexion/déconnexion d'un utilisateur dans XAM recompilé : XAM diffuse XN_SYS_SIGNINCHANGED dans ses propres files de
  // notification ; le Dashboard écoute celles du HLE (XamNotifyCreateListener/XNotifyGetNext du SDK). La notification est donc
  // rediffusée côté HLE quand l'état de connexion d'un utilisateur change (palier N4, mode profil XAM).
  // Diagnostic : XAM interroge XamUserGetXUID en boucle (1000+ appels mesures le 2026-11-20) quand
  // aucun profil n'existe : il attend un XUID qui ne vient jamais. On journalise la valeur rendue.
  if (std::string_view(s.name) == "XamUserGetXUID") {
    // Mesure du 2026-11-20 : XAM enumere normalement les 4 utilisateurs (index 0..3) a chaque
    // rafraichissement de session. Le 0x80070525 observe dans les journaux precedents etait le r3
    // APRES l'appel (statut d'erreur rendu), pas l'argument — d'ou une fausse piste de « boucle ».
    const uint32_t index_avant = static_cast<uint32_t>(ctx.r3.u32);
    DispatchImpl(s, ctx, base);
    static std::atomic<int> _n{0};
    const int n = ++_n;
    if (n <= 8 || n % 5000 == 0)
      REXLOG_INFO("[xuid] GetXUID(index={} mask={}) -> 0x{:08X} (appel {})",
                  index_avant, static_cast<uint32_t>(ctx.r4.u32),
                  static_cast<uint32_t>(ctx.r3.u64), n);
    return;
  }
  // Journal ciblé (une ligne par appel, rares) : « Profile » du menu principal (sub_923190D8 du Dashboard) demande la carte de joueur
  // à XAM ; si l'éditeur de profil ne s'ouvre pas chez l'utilisateur, la ligne dit si le Dashboard a bien appelé et ce que XAM a rendu.
  if (std::string_view(s.name) == "XamShowGamerCardUIForXUID" && s.guest.load()) {
    const uint32_t user = ctx.r3.u32;
    const uint64_t xuid = ctx.r4.u64;
    DispatchImpl(s, ctx, base);
    REXLOG_INFO("[xamredir] XamShowGamerCardUIForXUID(utilisateur {}, xuid {:016X}) -> 0x{:08X}", user, xuid, ctx.r3.u32);
    return;
  }
  if (std::string_view(s.name) == "XamUserGetSigninState" && s.guest.load()) {
    static std::atomic<int> last[4] = {-1, -1, -1, -1};
    const uint32_t user = ctx.r3.u32;
    DispatchImpl(s, ctx, base);
    if (user < 4) {
      const int state = static_cast<int>(ctx.r3.u32);
      const int before = last[user].exchange(state);
      if (before != -1 && before != state) {
        uint32_t mask = 0;
        for (uint32_t i = 0; i < 4; ++i) mask |= (last[i].load() > 0 ? 1u : 0u) << i;
        REXLOG_INFO("[xamredir] état de connexion de l'utilisateur {} : {} -> {} : XN_SYS_SIGNINCHANGED (masque {:X}) rediffusé", user,
                    before, state, mask);
        if (auto* ks = g_ks.load()) ks->BroadcastNotification(rex::system::XNotificationID(0x0000000A), mask);
      }
    }
    return;
  }
  // Mode profil XAM : les réglages de profil restent servis par le HLE (voir dash_app_base.cpp, ApplyProfileSettings), mais le
  // Dashboard les demande pour le XUID du compte connecté dans XAM recompilé. Le HLE refusait ce XUID (le sien est celui de
  // l'utilisateur fixe du runtime) : X_E_NO_SUCH_USER, carte joueur vide (ni gamertag ni avatar, « 0 » au lieu du Gamerscore).
  // On aligne donc l'identité du profil HLE sur le compte connecté (XUID et gamertag lus dans XAM) avant de servir la requête.
  if (ProfileModeXam() && !s.guest.load() &&
      (std::string_view(s.name) == "XamUserReadProfileSettings" || std::string_view(s.name) == "XamUserReadProfileSettingsEx")) {
    const uint32_t xuid_count = ctx.r5.u32, xuids = ctx.r6.u32, count = ctx.r7.u32, ids = ctx.r8.u32;
    const uint64_t wanted = (xuid_count && xuids) ? __builtin_bswap64(*reinterpret_cast<const uint64_t*>(base + xuids)) : 0;
    SyncHleProfileWithXam(ctx, base, wanted);
    // Le hub lit le manifeste d'avatar du profil par ce réglage (0x63E80044, avec la carte joueur) : on le donne au HLE depuis XAM.
    if (ids && count < 64)
      for (uint32_t i = 0; i < count; ++i)
        if (__builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ids + 4 * i)) == 0x63E80044u) {
          SyncHleAvatarManifest(ctx, base);
          break;
        }
    DispatchImpl(s, ctx, base);
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1) < 4)
      REXLOG_INFO("[xamredir] {}(xuid {:016X}) par HLE -> 0x{:08X}", s.name, wanted, ctx.r3.u32);
    // DASH_TRACE_SETTINGS=1 : identifiants des réglages demandés (ex. 0x63E80044, manifeste d'avatar du profil).
    if (std::getenv("DASH_TRACE_SETTINGS") && ids && count && count < 64) {
      std::string list;
      for (uint32_t i = 0; i < count; ++i)
        list += fmt::format(" {:08X}", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + ids + 4 * i)));
      REXLOG_INFO("[xamredir] {} : réglages{}", s.name, list);
    }
    return;
  }
  DispatchImpl(s, ctx, base);
}

// XUID (masque 3 : hors ligne ou en ligne) de l'utilisateur `user` dans XAM recompilé, 0 si l'emplacement est vide. Appel invité,
// tampon pris sous la pile de l'appelant.
uint64_t XamXuidOfUser(const PPCContext& ctx, uint8_t* base, uint32_t user) {
  const uint32_t xuid_buf = ctx.r1.u32 - 0x40;  // entre la pile de l'appelant et celle de l'appel (tmp.r1)
  PPCContext tmp = ctx;
  tmp.r1.u64 = ctx.r1.u32 - 0x400;
  std::memset(base + xuid_buf, 0, 8);
  tmp.r3.u64 = user;
  tmp.r4.u64 = 3;
  tmp.r5.u64 = xuid_buf;
  if (!CallXamExport("XamUserGetXUID", tmp, base) || tmp.r3.u32 != 0) return 0;
  return __builtin_bswap64(*reinterpret_cast<const uint64_t*>(base + xuid_buf));
}

// Emplacement (0 à 3) du compte connecté dans XAM recompilé : celui de XUID `xuid`, ou le premier occupé si `xuid` = 0 ; -1 sinon.
// Le profil HLE est unique, mais le compte peut être connecté à n'importe quel emplacement : celui de sa manette (mesure du Steam
// Deck du 2026-11-27, seconde manette, emplacement 1 : XamAvatarGetManifestLocalUser(0) -> 0x80070525, avatar choisi jamais
// donné au hub).
int XamUserIndex(const PPCContext& ctx, uint8_t* base, uint64_t xuid, uint64_t* found = nullptr) {
  for (uint32_t user = 0; user < 4; ++user) {
    const uint64_t x = XamXuidOfUser(ctx, base, user);
    if (x && (!xuid || x == xuid)) {
      if (found) *found = x;
      return static_cast<int>(user);
    }
  }
  return -1;
}

// Lit dans XAM recompilé le XUID et le gamertag du compte connecté (`wanted`, sinon le premier emplacement occupé ; appels invités,
// tampons pris sous la pile de l'appelant) et les donne au profil HLE. Un XUID étranger aux comptes connectés n'est pas adopté.
void SyncHleProfileWithXam(const PPCContext& ctx, uint8_t* base, uint64_t wanted) {
  rex::system::KernelState* ks = g_ks.load();
  auto* profile = ks ? ks->user_profile() : nullptr;
  if (!profile) return;
  const uint32_t sp = ctx.r1.u32;
  const uint32_t tag_buf = sp - 0x80;
  uint64_t xuid = 0;
  const int user = XamUserIndex(ctx, base, wanted, &xuid);
  if (user < 0 || xuid == profile->xuid()) return;
  std::memset(base + tag_buf, 0, 32);
  PPCContext tmp = ctx;
  tmp.r1.u64 = sp - 0x400;
  tmp.r3.u64 = static_cast<uint32_t>(user);
  tmp.r4.u64 = tag_buf;
  tmp.r5.u64 = 16;
  std::string gamertag;
  if (CallXamExport("XamUserGetGamerTag", tmp, base) && tmp.r3.u32 == 0) {
    for (uint32_t i = 0; i < 15; ++i) {
      const uint16_t c = static_cast<uint16_t>(base[tag_buf + 2 * i] << 8 | base[tag_buf + 2 * i + 1]);
      if (!c) break;
      gamertag += c < 0x80 ? static_cast<char>(c) : '?';
    }
  }
  REXLOG_INFO("[xamredir] profil HLE aligné sur le compte XAM connecté (emplacement {}) : XUID {:016X} -> {:016X}, gamertag '{}'",
              user, profile->xuid(), xuid, gamertag);
  profile->SetIdentity(xuid, gamertag);
}

// Manifeste d'avatar (réglage 0x63E80044, 1000 octets) du compte connecté dans XAM recompilé, lu par XamAvatarGetManifestLocalUser
// (XAM le lit dans le GPD du profil, écrit par XamAvatarSetManifest) et donné au profil HLE. Sans lui, le HLE ne connaît pas ce
// réglage et le hub dessine la silhouette grise par défaut. Une fois par XUID ; rien n'est donné si XAM échoue ou si le manifeste
// est vide (profil sans avatar : le Dashboard ouvre alors le choix d'avatar).
void SyncHleAvatarManifest(const PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kSetting = 0x63E80044u, kSize = 1000;
  rex::system::KernelState* ks = g_ks.load();
  auto* profile = ks ? ks->user_profile() : nullptr;
  if (!profile || !profile->xuid()) return;
  static std::mutex mu;
  static uint64_t synced = 0;
  std::lock_guard<std::mutex> lock(mu);
  const bool refresh = g_manifest_refresh.load() > 0;  // XamAvatarSetManifest vient de réussir : relire jusqu'à voir le nouveau
  if (synced == profile->xuid() && !refresh) return;
  const int user = XamUserIndex(ctx, base, profile->xuid());  // emplacement du compte dont le HLE a le XUID
  if (user < 0) {
    if (refresh) g_manifest_refresh.fetch_sub(1);
    return;
  }
  const uint32_t buf = REX_KERNEL_MEMORY()->SystemHeapAlloc(kSize);
  if (!buf) return;
  std::memset(base + buf, 0, kSize);
  PPCContext tmp = ctx;
  tmp.r1.u64 = ctx.r1.u32 - 0x400;
  tmp.r3.u64 = static_cast<uint32_t>(user);
  tmp.r4.u64 = buf;
  tmp.r5.u64 = 0;  // sans OVERLAPPED : appel synchrone
  const bool called = CallXamExport("XamAvatarGetManifestLocalUser", tmp, base);
  const uint32_t hr = tmp.r3.u32;
  std::vector<uint8_t> manifest(base + buf, base + buf + kSize);
  REX_KERNEL_MEMORY()->SystemHeapFree(buf);
  const bool empty = std::all_of(manifest.begin(), manifest.end(), [](uint8_t b) { return b == 0; });
  std::string head;
  for (int i = 0; i < 16; ++i) head += fmt::format("{:02X}", manifest[i]);
  REXLOG_INFO("[avatar] manifeste du profil {:016X} (emplacement {}) : XamAvatarGetManifestLocalUser -> 0x{:08X}{} ; {} ; début {}",
              profile->xuid(), user, hr, called ? "" : " (export absent)", empty ? "vide" : "donné au HLE (réglage 0x63E80044)", head);
  synced = profile->xuid();
  static std::vector<uint8_t> given;  // dernier manifeste donné au HLE
  if (!called || hr != 0 || empty || (refresh && manifest == given)) {
    if (refresh) g_manifest_refresh.fetch_sub(1);  // pas encore écrit : nouvelle tentative à la prochaine lecture (bornée)
    return;
  }
  profile->AddSetting(std::make_unique<rex::system::xam::UserProfile::BinarySetting>(kSetting, manifest));
  given = manifest;
  if (refresh) g_manifest_refresh.store(0);
}

void NoteAvatarManifestChanged(uint32_t hr) {
  if (static_cast<int32_t>(hr) < 0) return;
  g_manifest_refresh.store(20);
  // Les notifications de XAM recompilé n'atteignent pas l'écouteur du Dashboard (même cause que XN_SYS_SIGNINCHANGED plus haut) :
  // XN_SYS_PROFILESETTINGCHANGED (0xE), utilisateur 0, pour que le hub relise le manifeste.
  if (auto* ks = g_ks.load()) {
    ks->BroadcastNotification(rex::system::XNotificationID(0x0000000E), 1);
  }
}

void DispatchImpl(Slot& s, PPCContext& ctx, uint8_t* base) {
  PPCFunc* fn = s.guest.load();
  const uint64_t n = s.calls.fetch_add(1) + 1;
  if (n == 1 || n % 1000 == 0) {
    REXLOG_INFO("[xamredir] {} : appel {} par {}", s.name, n, fn ? "XAM recompilé" : "HLE");
  }
  // Connexion/déconnexion demandée par le Dashboard (XamUserLogon(XUID[4], drapeaux, overlapped)) : chaque appel, avec l'appelant et
  // les XUID (tous nuls = déconnexion de tous). Rare ; sert à remonter au chemin du hub qui déconnecte un profil.
  if (std::string_view(s.name) == "XamUserLogon" || std::string_view(s.name) == "XamUserLogonEx") {
    const uint32_t xuids = ctx.r3.u32;
    std::string list;
    char b[24];
    for (uint32_t i = 0; xuids && i < 4; ++i) {
      std::snprintf(b, sizeof b, "%s%016llX", i ? " " : "",
                    (unsigned long long)__builtin_bswap64(*reinterpret_cast<const uint64_t*>(base + xuids + 8 * i)));
      list += b;
    }
    REXLOG_INFO("[xamredir] {}([{}], drapeaux {:08X}, overlapped {:08X}) lr={:08X}", s.name, list, ctx.r4.u32, ctx.r5.u32,
                static_cast<uint32_t>(ctx.lr));
  }
  if (fn && std::string_view(s.name) == "XNotifyQueueUI") {  // message système posté à la file de tâches de XAM (SetTitleCall)
    dash::xamrt::SetTitleCall(true);
    fn(ctx, base);
    dash::xamrt::SetTitleCall(false);
    return;
  }
  if (fn) {
    fn(ctx, base);
    return;
  }
  PPCFunc* hle = s.hle.load();
  if (!hle) {
    const std::string sym = std::string("__imp__") + s.name;
    hle = FindHle(sym);
    if (!hle) {
      REXLOG_WARN("[xamredir] {} : ni HLE ni XAM recompilé, retour 0", s.name);
      ctx.r3.u64 = 0;
      return;
    }
    s.hle.store(hle);
  }
  hle(ctx, base);
}

// XAM recompilé (Configure) : sert aux imports des modules recompilés (CallModuleImport).
std::atomic<rex::system::UserModule*> g_xam{nullptr};
std::atomic<bool> g_configured{false};  // Configure a lié les imports redirigés (XAM recompilé chargé de façon asynchrone)
std::atomic<rex::runtime::ExportResolver*> g_resolver{nullptr};

// Ordinal d'un export de XAM (table « xam.xex » du SDK, mêmes ordinaux que la console). 0 si absent.
uint16_t FindOrdinal(rex::runtime::ExportResolver* resolver, std::string_view name) {
  if (!resolver) return 0;
  for (const auto& table : resolver->tables()) {
    if (table.module_name() != "xam.xex" && table.module_name() != "xam") continue;
    for (const rex::runtime::Export* e : table.exports_by_name()) {
      if (e->type == rex::runtime::Export::Type::kFunction && name == e->name) return e->ordinal;
    }
  }
  return 0;
}

std::vector<std::string> SplitList(const char* text) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    size_t b = cur.find_first_not_of(" \t"), e = cur.find_last_not_of(" \t");
    if (b != std::string::npos) out.push_back(cur.substr(b, e - b + 1));
    cur.clear();
  };
  for (const char* p = text; *p; ++p) {
    if (*p == ',') flush();
    else cur.push_back(*p);
  }
  flush();
  return out;
}

// DASH_TEST_PEEK=<adresse invitée hexadécimale> : journalise 64 mots de la mémoire invitée avant l'appel de test (diagnostic).
void PeekGuest(uint8_t* base) {
  if (const char* d = std::getenv("DASH_TEST_PEEK"); d && *d) {
    const uint32_t ea = static_cast<uint32_t>(std::strtoul(d, nullptr, 16));
    std::string line;
    for (uint32_t k = 0; k < 64; ++k) {
      char b[16];
      std::snprintf(b, sizeof b, " %08X", __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ea + k * 4)));
      line += b;
      if (k % 8 == 7) { REXLOG_INFO("[xamredir] peek {:08X}:{}", ea + (k - 7) * 4, line); line.clear(); }
    }
  }
}

// Test : XamShowMessageBoxUI appelé comme le ferait le Dashboard (hook -> Dispatch), depuis un thread titre.
// Appel asynchrone (XOVERLAPPED fourni) : le retour est immédiat, XAM complète l'overlapped plus tard.
// DASH_TEST_MSGBOX_WAIT = durée d'attente de la fermeture en secondes (60).
void RunMessageBoxTest(rex::system::KernelState* ks) {
  const char* w = std::getenv("DASH_TEST_MSGBOX_WAIT");
  const int wait_ms = (w && *w ? std::atoi(w) : 60) * 1000;
  const char16_t* buttons[] = {u"OK", u"Annuler"};
  uint32_t choice = 0xFFFFFFFF;
  REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : appel");
  if (ShowXamMessageBox(u"Xenondash", u"XamShowMessageBoxUI : test de redirection vers XAM recompilé", buttons, 2, 0, 0,
                        &choice, wait_ms)) {
    REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : TERMINÉ, bouton choisi={} (0xFFFFFFFF = aucun)", choice);
  }
}

// Attend la fin d'une requête asynchrone (XOVERLAPPED : InternalLow à +0 vaut 0x3E5 tant qu'elle est en cours). Renvoie vrai si
// elle s'est terminée avant `wait_ms`.
bool WaitOverlapped(uint8_t* base, uint32_t overlapped, int wait_ms, const char* label = "test") {
  auto word = [&](uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + ea)); };
  for (int t = 0; t < wait_ms; t += 50) {
    if (word(overlapped) != 0x3E5) {
      REXLOG_INFO("[xamredir] {} : TERMINÉ après {} ms, overlapped.InternalLow=0x{:08X}, erreur étendue=0x{:08X}", label,
                  t, word(overlapped), word(overlapped + 0x18));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REXLOG_WARN("[xamredir] {} : NON terminé après {} ms (overlapped.InternalLow=0x{:08X})", label, wait_ms,
              word(overlapped));
  return false;
}

// Texte UTF-16 big-endian de la mémoire invitée, en UTF-8 approximatif (ASCII ; les autres caractères en \uXXXX).
std::string ReadWide(uint8_t* base, uint32_t ea, uint32_t max_chars) {
  std::string out;
  for (uint32_t i = 0; i < max_chars; ++i) {
    const uint16_t c = __builtin_bswap16(*reinterpret_cast<volatile uint16_t*>(base + ea + i * 2));
    if (!c) break;
    if (c < 0x80) out.push_back(static_cast<char>(c));
    else { char b[8]; std::snprintf(b, sizeof b, "\\u%04X", c); out += b; }
  }
  return out;
}

// Test : XamShowKeyboardUI (clavier virtuel du HUD, vk.xex) appelé comme le ferait un titre, depuis un thread titre.
// Huit arguments, tous en registres : utilisateur, drapeaux, texte par défaut, titre, description, tampon résultat, taille
// du tampon (en caractères), XOVERLAPPED. DASH_TEST_KEYBOARD_WAIT = durée d'attente en secondes (60). Le texte saisi est
// journalisé à la fin.
void RunKeyboardTest(rex::system::KernelState* ks) {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_WARN("[xamredir] test XamShowKeyboardUI : pas de thread invité");
    return;
  }
  Slot* slot = FindSlot("XamShowKeyboardUI");
  if (!slot) return;
  uint8_t* base = ks->memory()->virtual_membase();
  PPCContext& tc = *ts->context();

  const uint32_t def_text = PutWide(u"Xenondash");
  const uint32_t title = PutWide(u"Clavier virtuel");
  const uint32_t desc = PutWide(u"Saisissez un texte de test");
  constexpr uint32_t kChars = 64;
  const uint32_t result = REX_KERNEL_MEMORY()->SystemHeapAlloc(kChars * 2);
  std::memset(base + result, 0, kChars * 2);
  const uint32_t overlapped = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x20);
  std::memset(base + overlapped, 0, 0x20);

  PPCContext ctx = tc;
  ctx.r1.u64 = static_cast<uint32_t>(tc.r1.u64) - 0x100;
  ctx.r3.u64 = 0;           // utilisateur 0
  ctx.r4.u64 = 0;           // drapeaux : clavier par défaut
  ctx.r5.u64 = def_text;
  ctx.r6.u64 = title;
  ctx.r7.u64 = desc;
  ctx.r8.u64 = result;
  ctx.r9.u64 = kChars;
  ctx.r10.u64 = overlapped;
  REXLOG_INFO("[xamredir] test XamShowKeyboardUI : appel");
  Dispatch(*slot, ctx, base);
  REXLOG_INFO("[xamredir] test XamShowKeyboardUI : retour r3=0x{:08X}", static_cast<uint32_t>(ctx.r3.u64));
  if (static_cast<uint32_t>(ctx.r3.u64) != 0x3E5) return;
  const char* w = std::getenv("DASH_TEST_KEYBOARD_WAIT");
  const int wait_ms = (w && *w ? std::atoi(w) : 60) * 1000;
  if (WaitOverlapped(base, overlapped, wait_ms)) {
    REXLOG_INFO("[xamredir] test XamShowKeyboardUI : texte renvoyé = \"{}\"", ReadWide(base, result, kChars));
  }
}

// Test : appel direct (arguments en registres, sans XOVERLAPPED) d'une interface du HUD par un thread titre, comme le ferait le
// Dashboard : XamShowSigninUI(nombre de volets, drapeaux), XamShowCreateProfileUI(utilisateur, drapeaux). La requête est
// envoyée à l'application XAM 254 (HUD) qui charge le module XUI correspondant ; le retour n'attend pas la fin de l'interface.
void RunUiCallTest(rex::system::KernelState* ks, const char* name, uint32_t a3, uint32_t a4) {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_WARN("[xamredir] test {} : pas de thread invité", name);
    return;
  }
  Slot* slot = FindSlot(name);
  if (!slot) return;
  uint8_t* base = ks->memory()->virtual_membase();
  PPCContext ctx = *ts->context();
  ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
  ctx.r3.u64 = a3;
  ctx.r4.u64 = a4;
  PeekGuest(base);
  REXLOG_INFO("[xamredir] test {}({}, {}) : appel", name, a3, a4);
  Dispatch(*slot, ctx, base);
  REXLOG_INFO("[xamredir] test {} : retour r3=0x{:08X}", name, static_cast<uint32_t>(ctx.r3.u64));
}

// DASH_TEST_UI=<secondes> + DASH_TEST_UI_CALL="<Import>:<arg>,<arg>,..." : appelle un import d'interface du Dashboard (hook de ce fichier)
// depuis un thread titre, comme le ferait dash.xex ; chaque argument (r3, r4...) est un entier (0x… accepté) ou « w:texte » (chaîne UTF-16
// invitée). Plusieurs appels : séparés par « ; » (pause d'une seconde entre deux).
void RunUiCallEnv(rex::system::KernelState* ks) {
  const char* spec = std::getenv("DASH_TEST_UI_CALL");
  auto* ts = rex::runtime::ThreadState::Get();
  if (!spec || !ts) return;
  uint8_t* base = ks->memory()->virtual_membase();
  std::string all = spec;
  size_t pos = 0;
  while (pos <= all.size()) {
    size_t end = all.find(';', pos);
    if (end == std::string::npos) end = all.size();
    const std::string call = all.substr(pos, end - pos);
    pos = end + 1;
    if (call.empty()) continue;
    const size_t colon = call.find(':');
    const std::string name = call.substr(0, colon);
    Slot* slot = FindSlot(name);
    if (!slot) {
      REXLOG_WARN("[xamredir] test {} : pas de hook", name);
      continue;
    }
    PPCContext ctx = *ts->context();
    ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
    PPCRegister* regs[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t n = 0;
    if (colon != std::string::npos) {
      const std::string args = call.substr(colon + 1);
      size_t a = 0;
      while (a <= args.size() && n < 8) {
        size_t b = args.find(',', a);
        if (b == std::string::npos) b = args.size();
        const std::string tok = args.substr(a, b - a);
        a = b + 1;
        if (tok.rfind("w:", 0) == 0) {
          std::u16string w;
          for (char c : tok.substr(2)) w.push_back(static_cast<char16_t>(static_cast<unsigned char>(c)));
          regs[n++]->u64 = PutWide(w.c_str());
        } else {
          regs[n++]->u64 = std::strtoull(tok.c_str(), nullptr, 0);
        }
      }
    }
    REXLOG_INFO("[xamredir] test {} : appel ({} arguments)", name, n);
    Dispatch(*slot, ctx, base);
    REXLOG_INFO("[xamredir] test {} : retour r3=0x{:08X}", name, static_cast<uint32_t>(ctx.r3.u64));
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}

void StartTestThread(rex::system::KernelState* ks, const char* env, const char* thread_name,
                     std::function<void(rex::system::KernelState*)> body) {
  const char* test = std::getenv(env);
  if (!test || !*test) return;
  const int seconds = std::atoi(test);
  std::thread([ks, seconds, thread_name, body] {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    static std::vector<rex::system::object_ref<rex::system::XHostThread>> hosts;  // tests : vivent jusqu'à la fin du processus
    hosts.emplace_back(new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, body] {
      body(ks);
      return 0;
    }));
    hosts.back()->set_name(thread_name);
    hosts.back()->Create();
  }).detach();
}

}  // namespace

// Palier N5 : boîte de message Xbox du HUD de XAM (XamShowMessageBoxUI redirigé), appelée depuis l'hôte — socle de
// dash_xam_hud_ui.cpp (progression de construction). À appeler depuis un thread invité (XHostThread) : la fonction
// invité lit le 9e argument sur la pile de l'appelant et XAM reçoit les frappes de ce thread. Renvoie faux si XAM
// recompilé n'est pas chargé/redirigé ou si la requête est refusée. wait_ms > 0 : attend la fermeture (borne) et écrit
// le bouton choisi dans *choice (0xFFFFFFFF = aucun). Mémoire invitée non libérée : les boîtes sont rares (comme les
// tests ci-dessus).
bool ShowXamMessageBox(const char16_t* title, const char16_t* text, const char16_t* const* buttons, size_t button_count,
                       uint32_t active_button, uint32_t flags, uint32_t* choice, int wait_ms) {
  Slot* slot = FindSlot("XamShowMessageBoxUI");
  if (!slot || !slot->guest.load()) {
    REXLOG_WARN("[xamredir] boîte de message : XAM recompilé non chargé ou non redirigé");
    return false;
  }
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_WARN("[xamredir] boîte de message : pas de thread invité");
    return false;
  }
  auto* ks = g_ks.load();
  if (!ks) return false;
  uint8_t* base = ks->memory()->virtual_membase();

  const uint32_t t = PutWide(title), x = PutWide(text);
  const uint32_t button_array = REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(button_count * 4));
  for (size_t i = 0; i < button_count; ++i) {
    *reinterpret_cast<uint32_t*>(base + button_array + i * 4) = __builtin_bswap32(PutWide(buttons[i]));
  }
  const uint32_t result = REX_KERNEL_MEMORY()->SystemHeapAlloc(4);
  *reinterpret_cast<uint32_t*>(base + result) = __builtin_bswap32(0xFFFFFFFFu);
  // XOVERLAPPED (0x20 octets, à zéro) : XAM refuse overlapped nul avec pResult non nul (chemin d'erreur 87).
  const uint32_t overlapped = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x20);
  std::memset(base + overlapped, 0, 0x20);

  // Neuf arguments : le 9e (overlapped) est sur la pile, à r1 + 0x54 de l'appelant. XAM le lit à 1412(r1) après un
  // stwu de 1328 octets : 1412 - 1328 = 0x54 (convention du SDK, FunctionDispatcher::Execute).
  PPCContext ctx = *ts->context();
  const uint32_t sp = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
  *reinterpret_cast<uint32_t*>(base + sp + 0x54) = __builtin_bswap32(overlapped);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;  // utilisateur 0
  ctx.r4.u64 = t;
  ctx.r5.u64 = x;
  ctx.r6.u64 = button_count;
  ctx.r7.u64 = button_array;
  ctx.r8.u64 = active_button;
  ctx.r9.u64 = flags;
  ctx.r10.u64 = result;

  Dispatch(*slot, ctx, base);
  if (static_cast<uint32_t>(ctx.r3.u64) != 0x3E5) {
    REXLOG_WARN("[xamredir] boîte de message : requête refusée, r3=0x{:08X}", static_cast<uint32_t>(ctx.r3.u64));
    return false;
  }
  if (wait_ms > 0 && WaitOverlapped(base, overlapped, wait_ms, "boîte de message")) {
    if (choice) *choice = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + result));
  }
  return true;
}

// Notification (toast) Xbox du HUD : XNotifyQueueUI de XAM recompilé, appelée depuis l'hôte (même contrat de thread que
// ShowXamMessageBox). Non bloquante : XAM met le message en file pour la fenêtre de notification. Mêmes arguments que
// le test de DASH_TEST_UI qui l'a validée (type 34, tous utilisateurs, zone 2, texte, sans paramètre).
bool ShowXamToast(const char16_t* text) {
  Slot* slot = FindSlot("XNotifyQueueUI");
  if (!slot || !slot->guest.load()) {
    REXLOG_WARN("[xamredir] notification : XAM recompilé non chargé ou XNotifyQueueUI non redirigé");
    return false;
  }
  auto* ts = rex::runtime::ThreadState::Get();
  auto* ks = g_ks.load();
  if (!ts || !ks) return false;
  uint8_t* base = ks->memory()->virtual_membase();
  PPCContext ctx = *ts->context();
  ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
  ctx.r3.u64 = 34;
  ctx.r4.u64 = 255;
  ctx.r5.u64 = 2;
  ctx.r6.u64 = PutWide(text);
  ctx.r7.u64 = 0;
  Dispatch(*slot, ctx, base);
  return static_cast<uint32_t>(ctx.r3.u64) == 0;
}

// Guide de la console : XamShowGuideUI(utilisateur, overlapped). Signature XDK : la fonction prend l'index
// d'utilisateur et un XOVERLAPPED optionnel ; elle ne rend la main qu'à la fermeture quand l'overlapped est nul.
// On lui en fournit toujours un, pour pouvoir borner l'attente sans bloquer le fil appelant indéfiniment.
bool ShowXamGuide(uint32_t user_index, int wait_ms) {
  Slot* slot = FindSlot("XamShowGuideUI");
  if (!slot || !slot->guest.load()) {
    REXLOG_WARN("[guide] XamShowGuideUI : XAM recompilé non chargé ou non redirigé (redirections : "
                "mode profil XAM, ou DASH_XAM_REDIRECT=XamShowGuideUI)");
    return false;
  }
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_WARN("[guide] XamShowGuideUI : pas de thread invité");
    return false;
  }
  auto* ks = g_ks.load();
  if (!ks) return false;
  uint8_t* base = ks->memory()->virtual_membase();

  // XOVERLAPPED à zéro (0x20 octets) : XAM refuse un overlapped nul quand il doit rendre un résultat.
  const uint32_t overlapped = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x20);
  std::memset(base + overlapped, 0, 0x20);

  // XamShowGuideUI(DWORD dwUserIndex) : un SEUL argument. Etabli en desassemblant la fonction de
  // XAM recompilé (ordinal 772 -> 0x81898630) : elle lit r3, le pose dans une structure locale de
  // 12 octets, appelle le portier sub_8189F0D0, et renvoie 5 (ERROR_ACCESS_DENIED) si ce portier
  // échoue. Rien d'autre n'est lu : ni drapeaux, ni overlapped. Les deux essais précédents
  // (overlapped en r4, puis drapeaux en r4) recevaient donc 5 pour la même raison — le portier,
  // pas la signature.
  PPCContext ctx = *ts->context();
  ctx.r3.u64 = user_index;
  ctx.r4.u64 = 0;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = 0;

  // Diagnostic : le portier de XamShowGuideUI (sub_8189F0D0) teste un drapeau global a
  // 0x81BFCB8C (lis 0xFFFF81C0, -13592 + 164). Sans lui, XAM renvoie 5 (ERROR_ACCESS_DENIED).
  // Cause etablie par la mesure (2026-11-20) : le portier sub_8189F0D0 de XamShowGuideUI teste le
  // mot a 0x81BFCAE8 (registre r31 du code genere) et refuse (r3=5, ERROR_ACCESS_DENIED) si ce mot
  // est non nul. Sur notre execution il vaut 4 — un compteur d'« interfaces systeme actives » que
  // XAM incremente pour chaque interface qu'il croit affichee, et que rien ne decremente ici (les
  // interfaces qui l'incrementent ne sont jamais fermees dans notre environnement).
  // Le Guide ne peut donc pas s'ouvrir : XAM le refuse, en toute logique, parce qu'il croit
  // qu'une interface systeme est deja a l'ecran.
  //
  // On remet ce compteur a zero le temps de l'appel, puis on restaure la valeur : c'est declarer
  // a XAM qu'aucune interface systeme n'est affichee, ce qui est le cas devant le Dashboard.
  // (Ne pas confondre avec 0x81BFCB8C, le mot passe dans le message : deux mots distincts, et mes
  // premiers essais portaient sur le mauvais.)
  const uint32_t counter_addr = 0x81BFCAE8u;
  auto* counter = reinterpret_cast<uint32_t*>(base + counter_addr);
  const uint32_t counter_before = __builtin_bswap32(*counter);
  // Toujours nécessaire (mesure du 2026-11-23, banc Xvfb, DASH_TEST_GUIDE=65) : compteur à 4 au moment de l'appel ;
  // remis à 0, XamShowGuideUI renvoie 0 et le Guide s'affiche sur le XMB ; laissé à 4, le portier refuse (r3=5) et le
  // XMB reste seul à l'écran.
  *counter = 0;
  REXLOG_INFO("[guide] compteur « interfaces systeme actives » (0x{:08X}) : {} -> 0 le temps de l'appel",
              counter_addr, counter_before);
  // Plus de message 0x80000012 envoyé à la main ni de +172 forcé : avec XeKeysGetStatus=0x4, le fil du HUD de XAM
  // (sub_818A2F90 -> sub_818A2C98) pose lui-même +172/+176 (mesure du 2026-11-22).
  REXLOG_INFO("[guide] XamShowGuideUI appelle (utilisateur {})", user_index);
  Dispatch(*slot, ctx, base);
  if (counter_before != 0) *counter = __builtin_bswap32(counter_before);
  // NOTE (banc 2026-11-21, nuit) : après le chargement réussi de la scène du Guide, la scène ne
  // prend pas la présentation. Deux pistes ÉCARTÉES par la mesure : (a) le rappel de l'objet de
  // requête ([0x402E9B80+48], appelé par la pompe UI sub_818905B0) n'est JAMAIS écrit — mais la
  // pompe SAUTE ces items (état 2, zéro [trap]) : comportement normal, pas le blocage ; (b) forcer
  // la cellule « app active » (0x817E4B64) à 254 ne change rien à l'affichage — ce n'est pas le
  // sélecteur de rendu. Le prochain chantier : qui fait présenter la scène chargée par XAM (boucle
  // de rendu des apps UI, +8=3002B000) et sur quoi le compositeur base la bascule.
  const uint32_t r = static_cast<uint32_t>(ctx.r3.u64);
  REXLOG_INFO("[guide] XamShowGuideUI : r3=0x{:08X} (compteur restaure a {})", r, counter_before);
  if (r != 0 && r != 0x3E5) {
    REXLOG_WARN("[guide] XamShowGuideUI : refusé, r3=0x{:08X}", r);
    return false;
  }
  REXLOG_INFO("[guide] XamShowGuideUI : accepté (r3=0x{:08X})", r);
  // DASH_GUIDE_TRACE=1 : diagnostic table des apps système (sub_81893EE0 : ctx(app) = 0x81C06F78 - app*184 ; validé par
  // sub_818941B0 qui enregistre l'app 254 avec la magie 0xAAAABBBB à +8 et son id à +4). Le message
  // du Guide part vers l'app 254 ; le répartiteur du système UI ne délivre qu'à l'app ACTIVE
  // (cellule 0x817E4B64). Le champ +4 de chaque contexte est l'id auto-déclaré à l'enregistrement.
  static const bool trace = [] {
    const char* e = std::getenv("DASH_GUIDE_TRACE");
    return e && *e && *e != '0';
  }();
  if (trace) {
    const uint32_t aa = __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + 0x817E4B64));
    for (int app = 246; app <= 258; ++app) {
      const uint32_t ctxa = 0x81C06F78u - static_cast<uint32_t>(app) * 184u;
      auto* c = reinterpret_cast<const uint32_t*>(base + ctxa);
      const uint32_t w4 = __builtin_bswap32(c[1]), w8 = __builtin_bswap32(c[2]), w16 = __builtin_bswap32(c[4]);
      REXLOG_INFO("[guide] app {:3d} ctx={:08X} +4(id)={:08X} +8={:08X} +16={:08X}{}",
                  app, ctxa, w4, w8, w16, app == static_cast<int>(aa) ? "  <- app active" : "");
    }
    REXLOG_INFO("[guide] cellule app active (0x817E4B64) = {}", aa);
    // Champs de la structure d'etat UI testes par le chargeur de scenes (sub_818A21B0) :
    // gardes sur +164/+172/+176 (+164 = le mot que lit aussi le portier) et +216.
    auto* st = reinterpret_cast<const uint32_t*>(base + 0x81BFCAE8u);
    REXLOG_INFO("[guide] etat UI 0x81BFCAE8 : +0={:08X} +56={:08X} +164={:08X} +172={:08X} +176={:08X} +216={:08X}",
                __builtin_bswap32(st[0]), __builtin_bswap32(st[14]), __builtin_bswap32(st[41]),
                __builtin_bswap32(st[43]), __builtin_bswap32(st[44]), __builtin_bswap32(st[54]));
  }
  (void)overlapped;  // la fonction ne le prend pas ; gardé pour un usage futur (attente bornée)
  if (wait_ms > 0) {
    WaitOverlapped(base, overlapped, wait_ms, "guide");
  }
  return true;
}

// Mesure du 2026-11-23 (profil sans avatar, Sign In depuis le XMB) : avant XamAppUnloadStack la pile vaut 2 (boot + signin) ;
// juste après, 0 ; le Guide ouvert ensuite n'en fait que 1. Avec un repos figé à 1, le Guide restait masqué par le compositeur.
bool XamSceneAboveRest(const uint8_t* base) {
  static std::atomic<bool> seen_scene{false};
  static std::atomic<uint32_t> rest{1};
  const uint32_t depth = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + 0x81BFCAE8u + 36));
  if (depth) seen_scene.store(true);
  else if (seen_scene.load()) rest.store(0);
  return depth > rest.load();
}

// XamIsUIActive du Dashboard : le HLE ne compte que ses propres boîtes (xam_dialogs_shown_), jamais les scènes de XAM recompilé.
// Après XamShowSigninUIp, dash.xex attend « interface active » puis « interface fermée » (boucles sur XamIsUIActive en
// 0x923165D4 et 0x923165F0, puis XNotifyBroadcast) : avec 0 en permanence, il tournait sans fin dans la première boucle — plus de
// lecture des touches, image figée sur « Sign In » (mesure du 2026-11-23). On ajoute donc l'état de XAM : scène au-dessus du
// repos. L'export de XAM (+164 de l'état UI) ne convient pas tel quel : il vaut 1 au repos à cause de la scène de boot.
void IsUIActive(Slot& s, PPCContext& ctx, uint8_t* base) {
  Dispatch(s, ctx, base);
  if (ctx.r3.u32 == 0 && g_xam.load() && XamSceneAboveRest(base)) ctx.r3.u64 = 1;
}

// Avatar du hub : dash.xex lie sa propre bibliothèque d'avatar (rendu 3D par le Dashboard) et ne demande à XAM que l'état
// (XamAvatarInitialize, sub_923C4A4C) puis manifeste, assets et animations. Le HLE du SDK fait échouer XamAvatarInitialize
// (~0, xam_avatar.cpp) : l'octet « avatar prêt » 0x929BBAE4 reste nul, XAvatarGetManifestLocalUser (sub_923C5890) renvoie
// 0x8000FFFF sans appeler XAM, et le hub affiche l'avatar gris par défaut au lieu de l'avatar du profil. En mode profil XAM, ces
// imports sont servis par XAM recompilé (kAvatarImports, liste de base de Configure) : XAM lit les paquets système d'avatar
// (FFFE07DF0000000x, kernel_stfs.cpp) et le manifeste du profil (réglage 0x63E80044 du GPD). Hors mode profil XAM : HLE.
// Journal borné des appels, avec l'octet « avatar prêt » et, pour un manifeste, ses 16 premiers octets.
constexpr const char* kAvatarImports =
    "XamAvatarInitialize,XamAvatarShutdown,XamAvatarGetManifestLocalUser,XamAvatarGetManifestsByXuid,"
    "XamAvatarGetAssetsResultSize,XamAvatarGetAssets,XamAvatarGenerateMipMaps,XamAvatarSetCustomAsset,"
    "XamAvatarLoadAnimation,XamAvatarManifestGetBodyType,XamAvatarGetMetadataRandom";
bool ProfileModeXam();
void AvatarCall(Slot& s, PPCContext& ctx, uint8_t* base) {
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32;
  // XAM recompilé se charge en parallèle du Dashboard (dash_app_base.cpp) : un XamAvatarInitialize appelé avant Configure partait
  // au HLE (échec, puis XamAvatarShutdown ; mesuré au redémarrage, 2026-11-23). En mode profil XAM, on attend la liaison (15 s au plus,
  // une seule fois : si XAM n'est toujours pas lié, les appels suivants partent au HLE sans attendre).
  static std::atomic<bool> gave_up{false};
  if (!g_configured.load() && !gave_up.load() && ProfileModeXam()) {
    const auto t0 = std::chrono::steady_clock::now();
    while (!g_configured.load() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(15))
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!g_configured.load()) gave_up.store(true);
    REXLOG_INFO("[avatar] {} : attente du chargement de XAM recompilé ({} ms)", s.name,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  }
  const PPCContext in = ctx;
  Dispatch(s, ctx, base);
  // XamAvatarInitialize de XAM recompilé envoie le message 0x600001 à l'application système d'avatar (242, sub_8187B648) ; tant que
  // XAM n'a pas fini d'installer cette application, il renvoie E_INVALIDARG (0x80070057) et libère l'application (sub_81894AC8) : le
  // Dashboard appelle alors XamAvatarShutdown et garde l'avatar gris (2 démarrages sur 6, 2026-11-23). On redemande, 5 s au plus.
  if (std::string_view(s.name) == "XamAvatarInitialize" && s.guest.load() && ctx.r3.u32 == 0x80070057u) {
    const auto t0 = std::chrono::steady_clock::now();
    int tries = 1;
    while (ctx.r3.u32 == 0x80070057u && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      ctx = in;
      Dispatch(s, ctx, base);
      ++tries;
    }
    REXLOG_INFO("[avatar] XamAvatarInitialize : E_INVALIDARG (application d'avatar de XAM pas prête), {} essais en {} ms -> 0x{:08X}",
                tries,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), ctx.r3.u32);
  }
  // Journal borné : 8 XamAvatarLoadAnimation (une trentaine au démarrage), 128 appels des autres imports.
  static std::atomic<int> logged_anim{0}, logged{0};
  const bool anim = std::string_view(s.name) == "XamAvatarLoadAnimation";
  if (anim ? logged_anim.fetch_add(1) < 8 : logged.fetch_add(1) < 128) {
    std::string manifest;
    if (std::string_view(s.name) == "XamAvatarGetManifestLocalUser" && a4)
      for (int i = 0; i < 16; ++i) manifest += fmt::format("{:02X}", base[a4 + i]);
    REXLOG_INFO("[avatar] {}({:08X}, {:08X}, {:08X}) -> 0x{:08X} par {} ; avatar prêt (0x929BBAE4) = {}{}{}", s.name, a3, a4, a5,
                ctx.r3.u32, s.guest.load() ? "XAM recompilé" : "HLE", base[0x929BBAE4u], manifest.empty() ? "" : " ; manifeste ",
                manifest);
  }
}

bool ProfileModeXam() {
  static const bool on = [] {
    const char* m = std::getenv("DASH_PROFILE_MODE");
    return m && std::string_view(m) == "xam" && std::getenv("DASH_XAM_MODULE");
  }();
  return on;
}

bool CallXamExport(const char* name, PPCContext& ctx, uint8_t* base) {
  static std::mutex mu;
  static std::unordered_map<std::string, PPCFunc*> cache;
  PPCFunc* fn = nullptr;
  {
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(name);
    if (it == cache.end()) {
      rex::system::KernelState* ks = g_ks.load();
      rex::system::UserModule* xam = g_xam.load();
      const uint16_t ordinal = FindOrdinal(g_resolver.load(), name);
      const uint32_t addr = (ks && xam && ordinal) ? xam->GetProcAddressByOrdinal(ordinal) : 0;
      fn = addr ? ks->function_dispatcher()->GetFunction(addr) : nullptr;
      if (fn) REXLOG_INFO("[xamredir] {} (ordinal {}) -> XAM recompilé 0x{:08X} (mode profil XAM)", name, ordinal, addr);
      else REXLOG_WARN("[xamredir] {} : export absent de XAM recompilé, HLE", name);
      it = cache.emplace(name, fn).first;
    }
    fn = it->second;
  }
  if (!fn) return false;
  fn(ctx, base);
  return true;
}

// Vrai si l'adresse invitée (lr d'un appelant) appartient au module chargé de ce nom (ex. "hud.xex").
bool CallerInModule(uint32_t address, std::string_view module_name) {
  rex::system::KernelState* ks = g_ks.load();
  if (!ks) return false;
  auto module = ks->GetModule(module_name, true);
  if (!module) return false;
  auto* user = static_cast<rex::system::UserModule*>(module.get());
  return user->xex_module() && user->xex_module()->ContainsAddress(address);
}

// Question Xbox LIVE de signin.xex à la connexion d'un profil qui a un XUID en ligne : « An update may be required to connect to
// Xbox LIVE. [...] Do you want to sign in to Xbox LIVE? » (Yes/No, chaîne 0x1D de sa table ; sub_90109110, XamShowMessageBox
// au retour 0x9010917C). signin ne la pose que si le bit 0x4 de XCONFIG_USER_RETAIL_FLAGS (3/0x000C) est nul ; Oui pose ce bit
// (et 0x10000000) et XAM tente alors une connexion Xbox LIVE, qui échoue hors ligne (« One or more profiles aren't valid »,
// mesure du banc du 2026-11-27) ; Non connecte le profil hors ligne. Retour du Steam Deck du 2026-11-27 : boîte à chaque connexion.
// Ici la boîte n'est pas affichée : son rappel (r9, sub_90109078) est appelé tout de suite avec la réponse Non (bouton 1),
// comme si l'utilisateur l'avait choisie ; signin enchaîne la connexion hors ligne. DASH_LIVE_PROMPT=1 garde la question.
bool LivePromptAnsweredNo(ModuleImport& imp, PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kSigninLivePromptLr = 0x9010917Cu;
  static const bool keep = [] {
    const char* v = std::getenv("DASH_LIVE_PROMPT");
    return v && *v && *v != '0';
  }();
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (keep || lr != kSigninLivePromptLr || std::string_view(imp.name) != "XamShowMessageBox" ||
      !CallerInModule(lr, "signin.xex"))
    return false;
  rex::system::KernelState* ks = g_ks.load();
  const uint32_t callback = ctx.r9.u32;
  PPCFunc* fn = (ks && callback) ? ks->function_dispatcher()->GetFunction(callback) : nullptr;
  if (!fn) {
    REXLOG_WARN("[xamredir] question Xbox LIVE de signin : rappel {:08X} introuvable, boîte affichée", callback);
    return false;
  }
  REXLOG_INFO("[xamredir] question Xbox LIVE de signin (lr {:08X}) : réponse Non donnée sans boîte (DASH_LIVE_PROMPT=1 la montre)", lr);
  PPCContext tmp = ctx;
  tmp.r1.u64 = ctx.r1.u32 - 0x100;
  tmp.r3.u64 = 1;  // bouton 1 = No
  tmp.lr = lr;
  fn(tmp, base);
  ctx.r3.u64 = 0;
  return true;
}

// Fermeture automatique de la boîte d'erreur de démarrage du HUD : instant d'ouverture (0 = rien en attente) et état du A.
std::atomic<int64_t> g_boot_box_ms{0};
std::atomic<bool> g_boot_box_down{false};
int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Boîtes d'erreur de démarrage du HUD (lecteur vide, statut 0xC000000F du chargeur de titres) : titre et début du texte.
bool IsBootErrorBox(uint8_t* base, uint32_t title, uint32_t text) {
  const std::string t = ReadWide(base, title, 40), x = ReadWide(base, text, 60);
  return (t == "Unrecognized Disc" && x.rfind("This disc is unreadable", 0) == 0) ||
         (t == "Game Error" && x.rfind("The game couldn't start", 0) == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// PERSONNALISATION DE L'AVATAR : OPTION 2 (point d'appel unique, à remplacer par l'option 1 plus tard).
//
// XamLaunchAvatarEditor (ordinal 2011) relance le titre AvatarEditor.xex (\sep\20352400, 15 Mo), qui n'est pas recompilé :
// XexLoadExecutable le refuse (0xC0050005) et la personnalisation n'aboutit pas. Appelants : signin.xex (sub_901047C0,
// après XamAppUnloadStack), createprofile.xex (sub_90106500, idem), gamerprofile.xex (« Avatar » d'Edit Profile,
// sub_9010D2D0), hud.xex (0x913E7A70) et le MiniCreator lui-même (touche Y, sub_90102990 puis XamAppUnloadStack).
//
// Option 2 (ici) : l'écran de choix d'avatar (Guide.AvatarMiniCreator.xex, recompilé) s'ouvre à la place, par l'export
// XamShowAvatarMiniCreatorUI (ordinal 1700) de XAM recompilé, mode 2 (celui de signin.xex). Aucun changement de titre.
// Si XAM refuse l'ouverture (ERROR_ACCESS_DENIED : une interface système se ferme encore), elle est réessayée depuis un fil
// invité de l'hôte (ReopenAvatarChooserLater). Depuis le MiniCreator (touche Y, suivie de son XamAppUnloadStack), elle
// attend en plus que le module soit déchargé : ouvert avant, le nouvel écran serait vidé avec la pile (mesure du
// 2026-11-23 : appel synchrone après XamAppUnloadStack -> 5). Pas de boucle : le MiniCreator n'appelle
// XamLaunchAvatarEditor que sur Y.
//
// Option 1 (à venir, AvatarEditor.xex recompilé) : supprimer l'appel à AvatarEditorOption2 dans CallModuleImport ;
// l'import retombe sur l'export 2011 de XAM recompilé (relance du titre AvatarEditor).
// ---------------------------------------------------------------------------------------------------------------------
constexpr std::string_view kAvatarMiniCreator = "Guide.AvatarMiniCreator.xex";
constexpr uint32_t kErrorAccessDenied = 5;

// XamShowAvatarMiniCreatorUI(utilisateur, 2) ; renvoie le code Win32 de XAM (0 : écran demandé).
uint32_t ShowAvatarChooser(uint32_t user, PPCContext& ctx, uint8_t* base) {
  static ModuleImport show{"XamShowAvatarMiniCreatorUI", 1700};
  ctx.r3.u64 = user;
  ctx.r4.u64 = 2;
  CallModuleImport(show, ctx, base);
  REXLOG_INFO("[avatar] option 2 : XamShowAvatarMiniCreatorUI(utilisateur {}, 2) -> {}", user, ctx.r3.u32);
  return ctx.r3.u32;
}

// Ouverture différée, depuis un fil invité de l'hôte : attend que le MiniCreator soit déchargé, puis réessaie tant que XAM
// répond ERROR_ACCESS_DENIED (borne : 5 s).
void ReopenAvatarChooserLater(uint32_t user) {
  rex::system::KernelState* ks = g_ks.load();
  if (!ks) return;
  static std::mutex mu;
  static std::vector<rex::system::object_ref<rex::system::XHostThread>> hosts;  // un fil par demande (rare), gardé
  std::lock_guard<std::mutex> lock(mu);
  hosts.emplace_back(new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, user] {
    auto* ts = rex::runtime::ThreadState::Get();
    if (!ts) return 0;
    uint8_t* base = ks->memory()->virtual_membase();
    for (int i = 0; i < 50; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (ks->GetModule(kAvatarMiniCreator, true)) continue;  // MiniCreator encore chargé : la pile se vide
      PPCContext ctx = *ts->context();
      ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x100;
      if (ShowAvatarChooser(user, ctx, base) != kErrorAccessDenied) return 0;
    }
    REXLOG_WARN("[avatar] option 2 : choix d'avatar non rouvert (XAM occupé après 5 s)");
    return 0;
  }));
  hosts.back()->set_name("XAM choix d'avatar");
  hosts.back()->Create();
}

// Données de lancement de XamLaunchAvatarEditor (XAM 13604, 0x818B7C20) : 268 octets (0x10C) gros-boutiste, +0 'EPIX', +4 indice de
// l'utilisateur (r3), +8 r4, +12 chaîne large optionnelle (r5, 127 caractères au plus). Elles passent à l'éditeur, qui les lit par
// XamLoaderGetLaunchData (0x920C9A88), avec les comptes connectés (session). Vrai si l'éditeur est lancé (ne revient pas).
bool LaunchAvatarEditorProcess(uint32_t user, PPCContext& ctx, uint8_t* base) {
  if (dash::launch::AvatarEditorExe().empty()) return false;
  dash::session::Session s;
  s.xuid = dash::session::SignedInXuids(ctx, base);
  s.launch.assign(0x10C, 0);
  auto put32 = [&](size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) s.launch[off + i] = uint8_t(v >> (24 - 8 * i));
  };
  put32(0, 0x45504958u);  // 'EPIX'
  put32(4, user);
  put32(8, ctx.r4.u32);
  if (ctx.r5.u32) {
    const auto* src = reinterpret_cast<const uint16_t*>(base + ctx.r5.u32);  // déjà gros-boutiste en mémoire invitée
    for (size_t i = 0; i < 127 && src[i]; ++i) std::memcpy(&s.launch[12 + 2 * i], &src[i], 2);
  }
  dash::session::Write(s);
  REXLOG_INFO("[avatar] XamLaunchAvatarEditor(utilisateur {}) : lancement de l'éditeur d'avatar (exécutable séparé)", user);
  return dash::launch::LaunchAvatarEditor();
}

// Vrai si l'import a été servi ici (XamLaunchAvatarEditor).
bool AvatarEditorOption2(ModuleImport& imp, PPCContext& ctx, uint8_t* base) {
  if (std::string_view(imp.name) != "XamLaunchAvatarEditor") return false;
  const uint32_t user = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (CallerInModule(lr, kAvatarMiniCreator)) {
    REXLOG_INFO("[avatar] XamLaunchAvatarEditor(utilisateur {}) depuis le MiniCreator lr={:08X} : choix rouvert après "
                "son déchargement (option 2)", user, lr);
    ReopenAvatarChooserLater(user);
    ctx.r3.u64 = 0;
    return true;
  }
  // Éditeur réel (exécutable séparé) pour « Customize Avatar » et « Avatar » d'Edit Profile ; les flux de signin.xex (profil sans avatar) et de
  // createprofile.xex gardent l'écran de choix du MiniCreator (option 2).
  const bool old_flow = CallerInModule(lr, "signin.xex") || CallerInModule(lr, "createprofile.xex");
  if (!old_flow && LaunchAvatarEditorProcess(user, ctx, base)) {  // ne revient pas : le Dashboard cède la place à l'éditeur
    ctx.r3.u64 = 0;
    return true;
  }
  if (old_flow)
    REXLOG_INFO("[avatar] XamLaunchAvatarEditor(utilisateur {}) lr={:08X} : flux signin/createprofile -> choix d'avatar du MiniCreator (option 2)", user, lr);
  else
    REXLOG_WARN("[avatar] XamLaunchAvatarEditor(utilisateur {}) lr={:08X} : exécutable `avatar` absent ou lancement refusé, REPLI sur le "
                "choix d'avatar du MiniCreator (option 2)", user, lr);
  const uint32_t err = ShowAvatarChooser(user, ctx, base);
  if (err == kErrorAccessDenied) {
    ReopenAvatarChooserLater(user);
    ctx.r3.u64 = 0;
    return true;
  }
  // Code Win32 positif converti en HRESULT, comme createprofile.xex (sub_90106480) : les appelants testent r3 < 0.
  if (ctx.r3.s32 > 0) ctx.r3.u64 = 0x80070000u | (err & 0xFFFFu);
  return true;
}

void CallModuleImport(ModuleImport& imp, PPCContext& ctx, uint8_t* base) {
  PPCFunc* fn = imp.fn.load();
  if (!fn) {
    rex::system::KernelState* ks = g_ks.load();
    rex::system::UserModule* xam = g_xam.load();
    uint32_t addr = 0;
    if (ks && xam) {
      addr = xam->GetProcAddressByOrdinal(imp.ordinal);
      fn = addr ? ks->function_dispatcher()->GetFunction(addr) : nullptr;
    }
    bool cacheable = true;
    if (fn) {
      imp.guest.store(true);
    } else {
      // Repli : HLE du SDK via RTLD_NEXT (FindHle). dlsym(RTLD_DEFAULT) renverrait le hook de l'exécutable lui-même
      // (récursion infinie). Tant que XAM recompilé n'est pas chargé, le repli n'est pas mémorisé (nouvelle tentative).
      cacheable = ks && xam;
      fn = FindHle(std::string("__imp__") + imp.name);
      if (cacheable) {
        REXLOG_WARN("[xamredir] module : {} (ordinal {}) absent de XAM recompilé, repli {}", imp.name, imp.ordinal,
                    fn ? "HLE" : "aucun (retour 0)");
      }
      if (!fn) {
        ctx.r3.u64 = 0;
        return;
      }
    }
    if (cacheable) {
      imp.fn.store(fn);
    }
  }
  const uint64_t n = imp.calls.fetch_add(1) + 1;
  if (n == 1) {
    REXLOG_INFO("[xamredir] module : {} (ordinal {}) : appel 1 par {}, lr={:08X}", imp.name, imp.ordinal,
                imp.guest.load() ? "XAM recompilé" : "HLE", static_cast<uint32_t>(ctx.lr));
  }
  // Boîte d'erreur (« Unrecognized Disc / This disc is unreadable » ou « Game Error / The game couldn't start » selon les
  // démarrages) que XAM fait afficher au HUD au démarrage : le chargeur de titres de XAM (sub_818765F8) signale un échec de
  // chargement (statut 0xC000000F, lecteur vide) et poste le message 0x2100D (sub_818B5A28). Elle reste cachée sous la scène
  // de repos, mais le premier toast (XNotifyQueueUI par XAM, scène empilée) la révélait. La supprimer à la source ou la
  // refuser à l'affichage ne marche pas (mesuré le 26/11) : sans la demande, hud.xex n'est pas chargé au démarrage et le
  // Guide s'ouvre trop tard (suite KO) ; refusée, la scène reste, vide, et le toast la montre quand même. On laisse donc la
  // boîte s'ouvrir et on la ferme comme le ferait l'utilisateur : un A au HUD (voir XamInputGetKeystrokeHud ci-dessous).
  // DASH_HUD_MSGBOX_KEEP=1 la laisse ouverte (diagnostic).
  if (std::string_view(imp.name) == "XamShowMessageBox" && !std::getenv("DASH_HUD_MSGBOX_KEEP") &&
      CallerInModule(static_cast<uint32_t>(ctx.lr), "hud.xex") && IsBootErrorBox(base, ctx.r4.u32, ctx.r5.u32)) {
    REXLOG_INFO("[xamredir] boîte d'erreur de démarrage du HUD (lecteur vide) : fermeture automatique par un A au HUD");
    g_boot_box_ms.store(NowMs());
  }
  if (AvatarEditorOption2(imp, ctx, base)) return;  // personnalisation de l'avatar : option 2 (voir plus haut)
  if (LivePromptAnsweredNo(imp, ctx, base)) return;  // question « sign in to Xbox LIVE? » de signin.xex : Non (voir plus haut)
  if (std::string_view(imp.name) == "XamAppUnloadSelf")
    REXLOG_INFO("[xamredir] XamAppUnloadSelf lr={:08X}", (uint32_t)ctx.lr);  // 0x913EC1A4 : sub_913EC0C8 (+92 = 0) ; sinon sub_913EA4C0
  // Frappes données au HUD (XamInputGetKeystrokeHud(utilisateur, X_INPUT_KEYSTROKE*)) : journal de chaque réponse non vide.
  if (std::string_view(imp.name) == "XamInputGetKeystrokeHud") {
    const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
    // Fermeture de la boîte de démarrage (voir plus haut) : A enfoncé puis relâché, 1,5 s après son ouverture.
    if (const int64_t at = g_boot_box_ms.load(); at != 0 && out && NowMs() - at >= 1500) {
      const bool down = !g_boot_box_down.exchange(true);
      if (!down) g_boot_box_ms.store(0);
      uint8_t* k = base + out;
      k[0] = 0x58; k[1] = 0x00; k[2] = 0; k[3] = 0;  // VK_PAD_A
      k[4] = 0; k[5] = down ? 0x01 : 0x02;           // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
      k[6] = static_cast<uint8_t>(user); k[7] = 0;
      ctx.r3.u64 = 0;
      REXLOG_INFO("[xamredir] boîte de démarrage du HUD : A {}", down ? "enfoncé" : "relâché");
      return;
    }
    fn(ctx, base);
    if (ctx.r3.u32 != 0x10D2 && out) {
      const uint8_t* k = base + out;
      REXLOG_INFO("[xamredir] XamInputGetKeystrokeHud(utilisateur {}) = 0x{:X} : VirtualKey=0x{:04X} unicode=0x{:04X} "
                  "flags=0x{:04X} utilisateur={} hid=0x{:02X}",
                  user, ctx.r3.u32, (k[0] << 8) | k[1], (k[2] << 8) | k[3], (k[4] << 8) | k[5], k[6], k[7]);
    }
    return;
  }
  // Choix d'un avatar (MiniCreator, XamAvatarSetManifest) : XAM écrit le manifeste dans le GPD du profil ; le HLE garde l'ancien
  // (ou aucun : silhouette grise dans My Xbox). La copie donnée au HLE est relue aux prochaines lectures du réglage.
  if (std::string_view(imp.name) == "XamAvatarSetManifest") {
    fn(ctx, base);
    NoteAvatarManifestChanged(ctx.r3.u32);
    return;
  }
  // DASH_XAM_TRACE_MODULE=<n> : journalise les n premiers appels de module (arguments r3..r6, retour r3), diagnostic.
  static const long trace_max = [] {
    const char* e = std::getenv("DASH_XAM_TRACE_MODULE");
    return e && *e ? std::atol(e) : 0L;
  }();
  static std::atomic<long> trace_n{0};
  if (trace_max > 0 && trace_n.fetch_add(1) < trace_max) {
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32;
    fn(ctx, base);
    REXLOG_INFO("[xamtrace] {}({:08X}, {:08X}, {:08X}, {:08X}) -> {:08X}", imp.name, a3, a4, a5, a6, ctx.r3.u32);
    return;
  }
  fn(ctx, base);
}

void Configure(rex::system::KernelState* ks, rex::runtime::ExportResolver* resolver,
               rex::system::UserModule* xam) {
  g_ks.store(ks);
  g_xam.store(xam);
  g_resolver.store(resolver);
  // Mode profil XAM : les imports de session utilisateur du Dashboard sont servis par XAM recompilé (en plus de DASH_XAM_REDIRECT).
  // Palier N5 : hors mode profil, la boîte de message du HUD est quand même redirigée dès que XAM recompilé est chargé
  // (fonction purement d'interface, sans sémantique de profil — socle de dash_xam_hud_ui.cpp).
  static std::string redirect_list;
  if (ProfileModeXam()) {
    redirect_list = "XGetVideoMode,XamUserGetXUID,XamUserGetSigninState,XamUserLogon,XamUserLogonEx,XamUserCheckPrivilege,XamUserIsOnlineEnabled,"
                    "XamShowMessageBoxUI,XamShowMessageBoxUIEx,"
                    "XamShowKeyboardUI,XamShowSigninUI,XamShowSigninUIp,XamShowCreateProfileUI,XamShowDeviceSelectorUI,"
                    "XamShowGamerCardUIForXUID,XamShowGuideUI,"
                    "XamShowAchievementsUIEx,XamShowGamerCardUI,XNotifyQueueUI,"
                    "XamAppUnloadStack,XamFormatDateString,XamFormatTimeString,";
    redirect_list += kAvatarImports;
    if (const char* e = std::getenv("DASH_XAM_REDIRECT"); e && *e) redirect_list += std::string(",") + e;
    REXLOG_INFO("[xamredir] DASH_PROFILE_MODE=xam : redirections {}", redirect_list);
  } else {
    redirect_list = "XamShowMessageBoxUI,XamShowMessageBoxUIEx";
    if (const char* e = std::getenv("DASH_XAM_REDIRECT"); e && *e) redirect_list += std::string(",") + e;
    REXLOG_INFO("[xamredir] redirections de base (boîte de message du HUD) + DASH_XAM_REDIRECT : {}", redirect_list);
  }
  // DASH_XAM_NOREDIRECT=<nom>[,<nom>...] : retire des redirections, pour mesurer l'effet d'une
  // seule d'entre elles. Sert au diagnostic (ex. XamShowMessageBoxUI, bloquante : si XAM ne
  // l'affiche pas, le Dashboard l'attend indefiniment et n'atteint jamais son propre XUI).
  if (const char* nr = std::getenv("DASH_XAM_NOREDIRECT"); nr && *nr) {
    std::string kept;
    for (const std::string& name : SplitList(redirect_list.c_str())) {
      bool drop = false;
      for (const std::string& skip : SplitList(nr))
        if (skip == name) drop = true;
      if (drop) {
        REXLOG_INFO("[xamredir] {} : redirection RETIREE (DASH_XAM_NOREDIRECT)", name);
        continue;
      }
      if (!kept.empty()) kept += ",";
      kept += name;
    }
    redirect_list = kept;
  }
  const char* env = redirect_list.c_str();
  if (!xam) {
    REXLOG_WARN("[xamredir] module XAM absent : aucune redirection");
  } else {
    for (const std::string& name : SplitList(env)) {
      Slot* s = FindSlot(name);
      if (!s) {
        REXLOG_WARN("[xamredir] {} : pas de hook dans common/src/dash_xam_redirect.cpp", name);
        continue;
      }
      const uint16_t ordinal = FindOrdinal(resolver, name);
      const uint32_t addr = ordinal ? xam->GetProcAddressByOrdinal(ordinal) : 0;
      PPCFunc* fn = addr ? ks->function_dispatcher()->GetFunction(addr) : nullptr;
      if (!fn) {
        REXLOG_WARN("[xamredir] {} (ordinal {}) : export absent de XAM recompilé", name, ordinal);
        continue;
      }
      s->guest_addr.store(addr);
      s->guest.store(fn);
      REXLOG_INFO("[xamredir] {} (ordinal {}) -> XAM recompilé 0x{:08X}", name, ordinal, addr);
    }
  }
  g_configured.store(true);

  // DASH_XAM_EXPORTS=<ordinal,...> : journalise l'adresse invitée de ces exports de XAM recompilé (diagnostic, désassemblage).
  if (const char* e = std::getenv("DASH_XAM_EXPORTS"); e && *e && xam) {
    for (const std::string& o : SplitList(e)) {
      const uint16_t ordinal = static_cast<uint16_t>(std::atoi(o.c_str()));
      REXLOG_INFO("[xamredir] export XAM ordinal {} = 0x{:08X}", ordinal, xam->GetProcAddressByOrdinal(ordinal));
    }
  }

  StartTestThread(ks, "DASH_TEST_SIGNIN", "XAM test signin",
                  [](rex::system::KernelState* k) { RunUiCallTest(k, "XamShowSigninUI", 1, 0); });
  StartTestThread(ks, "DASH_TEST_UI", "XAM test ui", [](rex::system::KernelState* k) { RunUiCallEnv(k); });
  StartTestThread(ks, "DASH_TEST_CREATEPROFILE", "XAM test createprofile",
                  [](rex::system::KernelState* k) { RunUiCallTest(k, "XamShowCreateProfileUI", 0, 0); });

  // DASH_WATCH_APPS=1 : surveille la cellule « app active » (0x817E4B64) et le contexte de l'app
  // 254 (le systeme UI, destinataire du Guide). Journalise sur changement : si l'app active ne
  // quitte jamais 252 (boot/loading), les scenes du Guide ne s'activent jamais.
  if (const char* w = std::getenv("DASH_WATCH_APPS"); w && *w && xam) {
    std::thread([ks] {
      auto* k = ks;
      uint8_t* base = k->memory()->virtual_membase();
      auto rd = [base](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + a)); };
      uint32_t prev_app = 0xFFFFFFFF, prev_8 = 0xFFFFFFFF, prev_16 = 0xFFFFFFFF, prev_12 = 0xFFFFFFFF;
      bool prev_game = true, prev_root = true;
      for (int i = 0; i < 1200; ++i) {  // 2 min a 100 ms
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint32_t app = rd(0x817E4B64);
        const uint32_t c8 = rd(0x81BFB8E8 + 8), c16 = rd(0x81BFB8E8 + 16), c12 = rd(0x81BFB8E8 + 12);
        const bool game = REX_KERNEL_FS()->ResolvePath("game:\\flash\\hud.xex") != nullptr;
        const bool root = REX_KERNEL_FS()->ResolvePath("\\SystemRoot\\hud.xex") != nullptr;
        if (app != prev_app || c8 != prev_8 || c16 != prev_16 || c12 != prev_12) {
          REXLOG_INFO("[watch] t={:4.1f}s app_active={:08X} | app254: +8={:08X} +12={:08X} +16={:08X}",
                      i * 0.1, app, c8, c12, c16);
          prev_app = app; prev_8 = c8; prev_16 = c16; prev_12 = c12;
        }
        if (game != prev_game || root != prev_root) {
          REXLOG_INFO("[watch] t={:4.1f}s VFS : game:\\flash\\hud.xex = {}, \\SystemRoot\\hud.xex = {}",
                      i * 0.1, game ? "OK" : "ECHEC", root ? "OK" : "ECHEC");
          if (!game) {
            REX_KERNEL_FS()->DumpState();
          }
          prev_game = game; prev_root = root;
        }
        // Provider de chaines localisees (0x81BFDA4C) : journaliser les changements.
        {
          const uint32_t prov = rd(0x81BFDA4C);
          static uint32_t prev_prov = 0xFFFFFFFF;
          if (prov != prev_prov) {
            REXLOG_INFO("[watch] t={:4.1f}s provider de chaines (0x81BFDA4C) = {:08X}", i * 0.1, prov);
            if (prov) {
              // Entete du provider et du bloc de langue source (0x301BC000) : le parse a-t-il
              // produit une table non vide ?
              std::string hp, hb;
              for (int k = 0; k < 8; ++k) {
                hp += fmt::format(" {:08X}", rd(prov + 4 * k));
                hb += fmt::format(" {:08X}", rd(0x301BC000u + 4 * k));
              }
              REXLOG_INFO("[watch]   provider @{:08X} :{}", prov, hp);
              REXLOG_INFO("[watch]   bloc langue 0x301BC000 :{}", hb);
            }
            prev_prov = prov;
          }
        }
        // Profondeur d'interfaces (etat UI 0x81BFCAE8+0) : chronologie — quand passe-t-elle a 4 ?
        {
          const uint32_t depth = rd(0x81BFCAE8);
          static uint32_t prev_depth = 0xFFFFFFFF;
          if (depth != prev_depth) {
            REXLOG_INFO("[watch] t={:4.1f}s profondeur interfaces (0x81BFCAE8) = {}", i * 0.1, depth);
            prev_depth = depth;
          }
        }
      }
    }).detach();
  }

  if (const char* test = std::getenv("DASH_TEST_KEYBOARD"); test && *test) {
    const int seconds = std::atoi(test);
    std::thread([ks, seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      static rex::system::object_ref<rex::system::XHostThread> host;  // test : vit jusqu'à la fin du processus
      host = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
          ks, 256 * 1024, 0, [ks] {
            RunKeyboardTest(ks);
            return 0;
          }));
      host->set_name("XAM test keyboard");
      host->Create();
    }).detach();
  }

  if (const char* test = std::getenv("DASH_TEST_MSGBOX"); test && *test) {
    const int seconds = std::atoi(test);
    std::thread([ks, seconds] {
      std::this_thread::sleep_for(std::chrono::seconds(seconds));
      static rex::system::object_ref<rex::system::XHostThread> host;  // test : vit jusqu'à la fin du processus
      host = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(
          ks, 256 * 1024, 0, [ks] {
            RunMessageBoxTest(ks);
            return 0;
          }));
      host->set_name("XAM test msgbox");
      host->Create();
    }).detach();
  }
}

uint64_t UserXuid(const PPCContext& ctx, uint8_t* base, uint32_t user) { return XamXuidOfUser(ctx, base, user); }

}  // namespace dash::xamredir

// Un hook par import XAM du Dashboard (table d'imports de dash.xex, ordinaux de work/dash_imports.tsv).
// Exclus, déjà remplacés ailleurs dans common/ : XamShowAchievementsUI (dash_xam_ui.cpp), XamLoaderLaunchTitle
// (dash_launch.cpp), XamNotifyCreateListener (dash_disc.cpp), XamContentClose (dash_library.cpp),
// XamContentCreateEnumerator (dash_content.cpp), XamUserGetName (dash_profile.cpp).
#define DASH_XAM_REDIRECT_HOOK(fn)                       \
  static dash::xamredir::Slot g_slot_##fn{#fn};          \
  REX_HOOK_RAW(__imp__##fn) { dash::xamredir::Dispatch(g_slot_##fn, ctx, base); }

DASH_XAM_REDIRECT_HOOK(XamInputGetCapabilities)
DASH_XAM_REDIRECT_HOOK(XamInputGetState)
DASH_XAM_REDIRECT_HOOK(XamInputSetState)
DASH_XAM_REDIRECT_HOOK(XamInputGetKeystrokeEx)
DASH_XAM_REDIRECT_HOOK(XamLoaderSetLaunchData)
DASH_XAM_REDIRECT_HOOK(XamLoaderGetLaunchData)
DASH_XAM_REDIRECT_HOOK(XamLoaderTerminateTitle)
DASH_XAM_REDIRECT_HOOK(XamLoaderGetDvdTrayState)
DASH_XAM_REDIRECT_HOOK(XamTaskCloseHandle)
// ---- Lecteur multimédia de XAM (application 0xFA, XMP) ----
// Les messages XMsg du Dashboard destinés à XMP vont à XAM recompilé : son application XMP est enregistrée (table des
// applications de XAM, 0x81C06F78 - 0xB8 * id) et initialisée (objet du lecteur en 0x81C07688), alors que le HLE du SDK
// (xmp_app.cpp, repris de Xenia) ne connaît que les messages des jeux. Mesure du 2026-11-26 : la Video Library
// (EcNavToVideoLibrary, scène MediaSourceSelection.xur) envoie 0x0007002B pendant le chargement de sa scène ; le HLE rendait
// X_E_FAIL, la scène échouait (0x8007065B) et le Dashboard revenait au canal Welcome. Les autres applications (0xFB, 0xFC,
// 0xFE) restent au HLE. Une requête asynchrone confiée à XAM s'annule chez XAM : le HLE d'XMsgCancelIORequest n'annule rien
// (il attend l'événement, que XAM ne signalerait peut-être jamais). Sans XAM recompilé, CallModuleImport se replie sur le
// HLE. DASH_XMP_HLE=1 : XMP au HLE (comparaison).
namespace {
constexpr uint32_t kAppXmp = 0xFA;
std::mutex g_xmp_mutex;
std::vector<uint32_t> g_xmp_requests;  // XOVERLAPPED des requêtes asynchrones confiées à XAM

bool XmpToXam(uint32_t app) {
  static const bool hle = std::getenv("DASH_XMP_HLE") != nullptr;
  return app == kAppXmp && !hle;
}

// Mémorise (xam = true) ou oublie la requête asynchrone de ce XOVERLAPPED : une adresse réutilisée pour une requête du HLE
// ne doit plus être annulée chez XAM. La voie est relevée après l'appel (imp.guest) : avant le chargement de XAM recompilé,
// CallModuleImport sert la requête par le HLE, et son annulation doit alors rester au HLE.
void NoteXmpRequest(uint32_t overlapped, bool xam) {
  if (!overlapped) return;
  std::lock_guard lock(g_xmp_mutex);
  auto it = std::find(g_xmp_requests.begin(), g_xmp_requests.end(), overlapped);
  if (xam && it == g_xmp_requests.end()) g_xmp_requests.push_back(overlapped);
  if (!xam && it != g_xmp_requests.end()) g_xmp_requests.erase(it);
}

bool TakeXmpRequest(uint32_t overlapped) {
  std::lock_guard lock(g_xmp_mutex);
  auto it = std::find(g_xmp_requests.begin(), g_xmp_requests.end(), overlapped);
  if (it == g_xmp_requests.end()) return false;
  g_xmp_requests.erase(it);
  return true;
}

// Journal : les 16 premiers messages XMP, puis chaque message en échec une fois.
void LogXmp(const char* fn, uint32_t message, uint32_t result, bool xam) {
  static std::atomic<int> n{0};
  static std::mutex m;
  static std::vector<uint32_t> failed;
  bool log = n.fetch_add(1) < 16;
  if (!log && int32_t(result) < 0) {
    std::lock_guard lock(m);
    if (std::find(failed.begin(), failed.end(), message) == failed.end()) {
      failed.push_back(message);
      log = true;
    }
  }
  if (log)
    REXLOG_INFO("[xmp] {}(0xFA, 0x{:08X}) -> 0x{:08X} par {}", fn, message, result,
                xam ? "XAM recompilé" : "HLE (XAM absent)");
}
}  // namespace

#define DASH_XMP_XMSG_HOOK(fn, ordinal, overlapped_expr)                \
  REX_HOOK_RAW(__imp__##fn) {                                           \
    const uint32_t overlapped = (overlapped_expr), message = ctx.r4.u32; \
    if (XmpToXam(ctx.r3.u32)) {                                         \
      static dash::xamredir::ModuleImport imp{#fn, ordinal};            \
      dash::xamredir::CallModuleImport(imp, ctx, base);                 \
      NoteXmpRequest(overlapped, imp.guest.load());                     \
      LogXmp(#fn, message, ctx.r3.u32, imp.guest.load());               \
      return;                                                           \
    }                                                                   \
    NoteXmpRequest(overlapped, false);                                  \
    static PPCFunc* const hle = dash::xamredir::FindHle("__imp__" #fn); \
    if (hle) {                                                          \
      hle(ctx, base);                                                   \
    } else {                                                            \
      ctx.r3.u64 = 0x80004005u;                                         \
    }                                                                   \
  }
DASH_XMP_XMSG_HOOK(XMsgInProcessCall, 500, 0u)
DASH_XMP_XMSG_HOOK(XMsgSystemProcessCall, 502, 0u)
DASH_XMP_XMSG_HOOK(XMsgStartIORequest, 503, ctx.r5.u32)
DASH_XMP_XMSG_HOOK(XMsgStartIORequestEx, 508, ctx.r5.u32)
#undef DASH_XMP_XMSG_HOOK

static dash::xamredir::Slot g_slot_XMsgCancelIORequest{"XMsgCancelIORequest"};
REX_HOOK_RAW(__imp__XMsgCancelIORequest) {
  if (TakeXmpRequest(ctx.r3.u32)) {  // requête confiée à XAM (XMP) : annulée chez XAM
    static dash::xamredir::ModuleImport imp{"XMsgCancelIORequest", 504};
    dash::xamredir::CallModuleImport(imp, ctx, base);
    return;
  }
  dash::xamredir::Dispatch(g_slot_XMsgCancelIORequest, ctx, base);
}
DASH_XAM_REDIRECT_HOOK(XamUserGetXUID)
DASH_XAM_REDIRECT_HOOK(XamUserLogon)
DASH_XAM_REDIRECT_HOOK(XamUserLogonEx)  // ordinal 548 : appelée par dash.xex avant XamShowSigninUIp
DASH_XAM_REDIRECT_HOOK(XamUserGetSigninState)
DASH_XAM_REDIRECT_HOOK(XamUserReadProfileSettings)
DASH_XAM_REDIRECT_HOOK(XamUserReadProfileSettingsEx)
DASH_XAM_REDIRECT_HOOK(XamUserCheckPrivilege)
DASH_XAM_REDIRECT_HOOK(XamUserIsOnlineEnabled)
DASH_XAM_REDIRECT_HOOK(XamUserAreUsersFriends)
DASH_XAM_REDIRECT_HOOK(XamEnumerate)
DASH_XAM_REDIRECT_HOOK(XamContentCreate)
DASH_XAM_REDIRECT_HOOK(XamContentDelete)
DASH_XAM_REDIRECT_HOOK(XamContentCreateDeviceEnumerator)
DASH_XAM_REDIRECT_HOOK(XamContentGetDeviceState)
DASH_XAM_REDIRECT_HOOK(XNotifyGetNext)
DASH_XAM_REDIRECT_HOOK(XamShowSigninUI)
DASH_XAM_REDIRECT_HOOK(XamShowSigninUIp)
DASH_XAM_REDIRECT_HOOK(XamShowKeyboardUI)
DASH_XAM_REDIRECT_HOOK(XamShowMessageBoxUI)
DASH_XAM_REDIRECT_HOOK(XamShowDeviceSelectorUI)
DASH_XAM_REDIRECT_HOOK(XamShowCreateProfileUI)
DASH_XAM_REDIRECT_HOOK(XamShowGamerCardUIForXUID)
DASH_XAM_REDIRECT_HOOK(XamShowMessageBoxUIEx)
DASH_XAM_REDIRECT_HOOK(XamShowGuideUI)
DASH_XAM_REDIRECT_HOOK(XamAppUnloadStack)
DASH_XAM_REDIRECT_HOOK(XamFormatDateString)
DASH_XAM_REDIRECT_HOOK(XamFormatTimeString)
// Imports de l'éditeur d'avatar (AvatarEditor.xex, dashboards/2.0.13604.0-avatar) : hooks sans redirection par défaut (le Dashboard ne les
// importe pas), activés par DASH_XAM_REDIRECT dans le main de l'éditeur. Sans eux le HLE du SDK les laisse en stubs : XamAvatarEnumAssets
// rendait sans rien écrire et l'éditeur le rappelait en boucle (12 millions de lignes de journal en 50 s, un cœur à 100 %).
DASH_XAM_REDIRECT_HOOK(XamAvatarBeginEnumAssets)
DASH_XAM_REDIRECT_HOOK(XamAvatarEnumAssets)
DASH_XAM_REDIRECT_HOOK(XamAvatarEndEnumAssets)
DASH_XAM_REDIRECT_HOOK(XamAvatarGetAssetBinary)
DASH_XAM_REDIRECT_HOOK(XamAvatarGetAssetIcon)
DASH_XAM_REDIRECT_HOOK(XamAvatarGetInstalledAssetPackageDescription)
DASH_XAM_REDIRECT_HOOK(XamAvatarSetManifest)
DASH_XAM_REDIRECT_HOOK(XamUserCreateAvatarAssetEnumerator)
DASH_XAM_REDIRECT_HOOK(XamUserValidateAvatarManifest)
DASH_XAM_REDIRECT_HOOK(XamPngDecode)
DASH_XAM_REDIRECT_HOOK(XamPngEncodeEx)
DASH_XAM_REDIRECT_HOOK(XamWriteGamerTile)
DASH_XAM_REDIRECT_HOOK(XamWriteGamerTileEx)
DASH_XAM_REDIRECT_HOOK(XamWriteTile)
DASH_XAM_REDIRECT_HOOK(XamUserProfileSync)  // « Save and Exit » : le stub du SDK rendait un résidu de r3 (« Can't save your avatar »)
// Autres imports d'interface du Dashboard (inventaire docs/xam_recompile.md) : hooks sans redirection par défaut, pour mesurer les appels
// ([xamredir] <nom> : appel 1 par HLE) ; ajoutés à la liste de Configure quand ils sont redirigés.
DASH_XAM_REDIRECT_HOOK(XamShowAchievementsUIEx)
DASH_XAM_REDIRECT_HOOK(XamShowGamerCardUI)
DASH_XAM_REDIRECT_HOOK(XamShowMarketplaceUI)
DASH_XAM_REDIRECT_HOOK(XamShowMarketplaceUIEx)
DASH_XAM_REDIRECT_HOOK(XamShowFriendRequestUI)
DASH_XAM_REDIRECT_HOOK(XamShowMessagesUI)
DASH_XAM_REDIRECT_HOOK(XamShowMessageComposeUI)
DASH_XAM_REDIRECT_HOOK(XamShowGamesUI)
DASH_XAM_REDIRECT_HOOK(XamShowJoinPartyUI)
DASH_XAM_REDIRECT_HOOK(XamShowJoinSessionInProgressUI)
DASH_XAM_REDIRECT_HOOK(XamShowPrivateChatInviteUI)
DASH_XAM_REDIRECT_HOOK(XamShowVideoChatInviteUI)
DASH_XAM_REDIRECT_HOOK(XamShowGoldUpgradeUI)
DASH_XAM_REDIRECT_HOOK(XamShowLiveSignupUI)
DASH_XAM_REDIRECT_HOOK(XamShowLiveUpsellUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiFriendsUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiGamesUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiGuideUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiHardwareRequiredUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiMessageBoxUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiSigninUI)
DASH_XAM_REDIRECT_HOOK(XamShowNuiTroubleshooterUI)
DASH_XAM_REDIRECT_HOOK(XamShowPasscodeVerifyUI)
DASH_XAM_REDIRECT_HOOK(XamShowPasscodeVerifyUIEx)
DASH_XAM_REDIRECT_HOOK(XamResumeUpdaterUI)
DASH_XAM_REDIRECT_HOOK(XNotifyQueueUI)
static dash::xamredir::Slot g_slot_XamIsUIActive{"XamIsUIActive"};
REX_HOOK_RAW(__imp__XamIsUIActive) {
  dash::xamredir::IsUIActive(g_slot_XamIsUIActive, ctx, base);
}
// « Customize Avatar » de dash.xex (sub_9231DC28) : même point que les modules (AvatarEditorOption2, via CallModuleImport).
// dash.xex ne l'appelle que si XamPackageManagerGetExperienceMode a le bit 0x2 et si XamPackageManagerGetFileSize trouve
// l'éditeur : les deux sont servis par XAM recompilé pour cet appel (voir plus bas).
REX_HOOK_RAW(__imp__XamLaunchAvatarEditor) {
  static dash::xamredir::ModuleImport imp{"XamLaunchAvatarEditor", 2011};
  dash::xamredir::CallModuleImport(imp, ctx, base);
}
// XamIsIptvEnabled (ordinal 1301) : export réel de XAM recompilé (0x818B86E0, bit 0x1000 de RETAIL_FLAGS 3/0x000C lu par
// ExGetXConfigSetting), repli sur le HLE du SDK (même règle) sans XAM recompilé. Condition EcoMediaroomEnabled de dash.xex
// (sub_922FA490, cas 7) : le stub du SDK n'écrivait pas r3 (resté = adresse de l'objet condition, lue comme un succès) et My Xbox
// montrait un emplacement Mediaroom vide (mesure du 2026-11-26).
REX_HOOK_RAW(__imp__XamIsIptvEnabled) {
  static dash::xamredir::ModuleImport imp{"XamIsIptvEnabled", 1301};
  dash::xamredir::CallModuleImport(imp, ctx, base);
}
// XamPackageManagerGetExperienceMode (ordinal 1606) de dash.xex : export réel de XAM recompilé (sub_818BC250) au lieu du HLE du SDK
// (renvoie 0 sans écrire *pMode). Même schéma que hud_xam_imports.cpp / gamerprofile_xam_imports.cpp. Aucune valeur forcée.
REX_HOOK_RAW(__imp__XamPackageManagerGetExperienceMode) {
  // Redirection globale désactivée par défaut : elle fait basculer le démarrage vers AvatarEditor.xex (le hub n'apparaît
  // plus ; DASH_XAM_EXPERIENCE_REDIRECT=1 la réactive, diagnostic). Même résultat que le HLE du SDK (r3 = 0, *pMode non
  // écrit) partout sauf « Customize Avatar » : sub_9231DC28 lit *pMode (appel d'adresse de retour 0x9231DC44, bit 0x2
  // requis) puis, avec XamPackageManagerGetFileSize("AvatarEditor.xex") non nul, appelle XamLaunchAvatarEditor (option 2 :
  // MiniCreator). Avec le HLE (*pMode = 0), dash prend la branche « requires update ». Pour cet appel : valeur de XAM
  // recompilé (mode 0x2 mesuré, bit présent sans rien forcer).
  static const bool all = [] {
    const char* e = std::getenv("DASH_XAM_EXPERIENCE_REDIRECT");
    const bool on = e && std::string_view(e) == "1";
    if (on) REXLOG_INFO("[xamredir] XamPackageManagerGetExperienceMode : redirection active (DASH_XAM_EXPERIENCE_REDIRECT=1)");
    return on;
  }();
  static dash::xamredir::ModuleImport imp{"XamPackageManagerGetExperienceMode", 1606};
  if (all) {
    dash::xamredir::CallModuleImport(imp, ctx, base);
    return;
  }
  if (static_cast<uint32_t>(ctx.lr) != 0x9231DC44u) {  // retour du bl de sub_9231DC28
    ctx.r3.u64 = 0;
    return;
  }
  dash::xamredir::CallModuleImport(imp, ctx, base);  // mesuré le 2026-11-26 : mode 0x2, r3 = 0
}
// XamPackageManagerGetFileSize (ordinal 1608) : HLE du SDK (0) partout ; pour « Customize Avatar » (retour 0x9231DC64),
// XAM recompilé : il trouve AvatarEditor.xex dans l'index des paquets (15 556 608 octets mesurés), l'éditeur lui-même
// n'étant pas recompilé (XamLaunchAvatarEditor est servi par le MiniCreator, option 2).
REX_HOOK_RAW(__imp__XamPackageManagerGetFileSize) {
  static PPCFunc* const hle = dash::xamredir::FindHle("__imp__XamPackageManagerGetFileSize");
  static dash::xamredir::ModuleImport imp{"XamPackageManagerGetFileSize", 1608};
  if (static_cast<uint32_t>(ctx.lr) != 0x9231DC64u) {
    if (hle) hle(ctx, base); else ctx.r3.u64 = 0;
    return;
  }
  dash::xamredir::CallModuleImport(imp, ctx, base);
}
#define DASH_XAM_AVATAR_HOOK(fn)                         \
  static dash::xamredir::Slot g_slot_##fn{#fn};          \
  REX_HOOK_RAW(__imp__##fn) { dash::xamredir::AvatarCall(g_slot_##fn, ctx, base); }
DASH_XAM_AVATAR_HOOK(XamAvatarInitialize)
DASH_XAM_AVATAR_HOOK(XamAvatarShutdown)
DASH_XAM_AVATAR_HOOK(XamAvatarGetManifestLocalUser)
DASH_XAM_AVATAR_HOOK(XamAvatarGetManifestsByXuid)
DASH_XAM_AVATAR_HOOK(XamAvatarGetAssetsResultSize)
DASH_XAM_AVATAR_HOOK(XamAvatarGetAssets)
DASH_XAM_AVATAR_HOOK(XamAvatarGenerateMipMaps)
DASH_XAM_AVATAR_HOOK(XamAvatarSetCustomAsset)
DASH_XAM_AVATAR_HOOK(XamAvatarLoadAnimation)
DASH_XAM_AVATAR_HOOK(XamAvatarManifestGetBodyType)
DASH_XAM_AVATAR_HOOK(XamAvatarGetMetadataRandom)
DASH_XAM_REDIRECT_HOOK(XamVoiceCreate)
DASH_XAM_REDIRECT_HOOK(XamVoiceHeadsetPresent)
DASH_XAM_REDIRECT_HOOK(XamVoiceSubmitPacket)
DASH_XAM_REDIRECT_HOOK(XamVoiceClose)
DASH_XAM_REDIRECT_HOOK(XGetAVPack)
DASH_XAM_REDIRECT_HOOK(XGetLanguage)
DASH_XAM_REDIRECT_HOOK(XGetVideoStandard)
DASH_XAM_REDIRECT_HOOK(XGetVideoMode)
