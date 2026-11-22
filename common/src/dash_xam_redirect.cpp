// xenondash common - redirection des imports XAM du Dashboard vers XAM recompilé (voir dash_xam_redirect.h).

#include "dash_xam_redirect.h"

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

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
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
    // Même critère que le compositeur (hud_front.cpp, PublishOverlay) : masque +0 non nul (ouverture) ou pile de scènes +36 >= 2
    // (Guide ou boîte empilé sur la scène de boot ; le masque retombe à 0 Guide affiché).
    const uint32_t ui_mask = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + 0x81BFCAE8u));
    const uint32_t depth = __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(base + 0x81BFCAE8u + 36));
    const bool visible = ngpu::HudActive();
    const bool hud = visible && (ui_mask != 0 || depth >= 2);
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
  DispatchImpl(s, ctx, base);
}

void DispatchImpl(Slot& s, PPCContext& ctx, uint8_t* base) {
  PPCFunc* fn = s.guest.load();
  const uint64_t n = s.calls.fetch_add(1) + 1;
  if (n == 1 || n % 1000 == 0) {
    REXLOG_INFO("[xamredir] {} : appel {} par {}", s.name, n, fn ? "XAM recompilé" : "HLE");
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
  // Diagnostic table des apps système (sub_81893EE0 : ctx(app) = 0x81C06F78 - app*184 ; validé par
  // sub_818941B0 qui enregistre l'app 254 avec la magie 0xAAAABBBB à +8 et son id à +4). Le message
  // du Guide part vers l'app 254 ; le répartiteur du système UI ne délivre qu'à l'app ACTIVE
  // (cellule 0x817E4B64). Le champ +4 de chaque contexte est l'id auto-déclaré à l'enregistrement.
  {
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
    if (fn) {
      imp.guest.store(true);
    } else {
      // Repli : symbole visible de l'exécutable (accroche de common/ ou HLE du SDK ; le `__imp__` du module est masqué).
#if !defined(_WIN32)
      fn = reinterpret_cast<PPCFunc*>(dlsym(RTLD_DEFAULT, (std::string("__imp__") + imp.name).c_str()));
#else
      fn = rex::ppc::FindPPCFuncByName((std::string("__imp__") + imp.name).c_str());
#endif
      REXLOG_WARN("[xamredir] module : {} (ordinal {}) absent de XAM recompilé, repli {}", imp.name, imp.ordinal,
                  fn ? "HLE" : "aucun (retour 0)");
      if (!fn) {
        ctx.r3.u64 = 0;
        return;
      }
    }
    imp.fn.store(fn);
  }
  const uint64_t n = imp.calls.fetch_add(1) + 1;
  if (n == 1) {
    REXLOG_INFO("[xamredir] module : {} (ordinal {}) : appel 1 par {}", imp.name, imp.ordinal,
                imp.guest.load() ? "XAM recompilé" : "HLE");
  }
  // Boîte de message du HUD au démarrage (« disque illisible » : ERROR_WRONG_DISK, lecteur vide) : supprimée. Renvoyer une
  // erreur (ERROR_CANCELLED) faisait arrêter la boucle d'images du HUD (vtable+44, docs : mesure du 21/11) ; on renvoie donc
  // le chemin de succès (r3 = 0). DASH_HUD_MSGBOX_KEEP=1 conserve la boîte (diagnostic).
  if (std::string_view(imp.name) == "XamShowMessageBox" && !std::getenv("DASH_HUD_MSGBOX_KEEP")) {
    REXLOG_INFO("[xamredir] XamShowMessageBox refusee (lecteur vide, boite sans cause) lr={:08X}", (uint32_t)ctx.lr);
    ctx.r3.u64 = 0;
    return;
  }
  if (std::string_view(imp.name) == "XamAppUnloadSelf")
    REXLOG_INFO("[xamredir] XamAppUnloadSelf lr={:08X}", (uint32_t)ctx.lr);  // 0x913EC1A4 : sub_913EC0C8 (+92 = 0) ; sinon sub_913EA4C0
  // Frappes données au HUD (XamInputGetKeystrokeHud(utilisateur, X_INPUT_KEYSTROKE*)) : journal de chaque réponse non vide.
  if (std::string_view(imp.name) == "XamInputGetKeystrokeHud") {
    const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
    fn(ctx, base);
    if (ctx.r3.u32 != 0x10D2 && out) {
      const uint8_t* k = base + out;
      REXLOG_INFO("[xamredir] XamInputGetKeystrokeHud(utilisateur {}) = 0x{:X} : VirtualKey=0x{:04X} unicode=0x{:04X} "
                  "flags=0x{:04X} utilisateur={} hid=0x{:02X}",
                  user, ctx.r3.u32, (k[0] << 8) | k[1], (k[2] << 8) | k[3], (k[4] << 8) | k[5], k[6], k[7]);
    }
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
    redirect_list = "XGetVideoMode,XamUserGetXUID,XamUserGetSigninState,XamUserLogon,XamUserCheckPrivilege,XamUserIsOnlineEnabled,"
                    "XamShowMessageBoxUI,XamShowMessageBoxUIEx,"
                    "XamShowKeyboardUI,XamShowSigninUI,XamShowSigninUIp,XamShowCreateProfileUI,XamShowDeviceSelectorUI,"
                    "XamShowGamerCardUIForXUID,XamShowGuideUI";
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

  // DASH_XAM_EXPORTS=<ordinal,...> : journalise l'adresse invitée de ces exports de XAM recompilé (diagnostic, désassemblage).
  if (const char* e = std::getenv("DASH_XAM_EXPORTS"); e && *e && xam) {
    for (const std::string& o : SplitList(e)) {
      const uint16_t ordinal = static_cast<uint16_t>(std::atoi(o.c_str()));
      REXLOG_INFO("[xamredir] export XAM ordinal {} = 0x{:08X}", ordinal, xam->GetProcAddressByOrdinal(ordinal));
    }
  }

  StartTestThread(ks, "DASH_TEST_SIGNIN", "XAM test signin",
                  [](rex::system::KernelState* k) { RunUiCallTest(k, "XamShowSigninUI", 1, 0); });
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
DASH_XAM_REDIRECT_HOOK(XMsgCancelIORequest)
DASH_XAM_REDIRECT_HOOK(XamUserGetXUID)
DASH_XAM_REDIRECT_HOOK(XamUserLogon)
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
DASH_XAM_REDIRECT_HOOK(XamVoiceCreate)
DASH_XAM_REDIRECT_HOOK(XamVoiceHeadsetPresent)
DASH_XAM_REDIRECT_HOOK(XamVoiceSubmitPacket)
DASH_XAM_REDIRECT_HOOK(XamVoiceClose)
DASH_XAM_REDIRECT_HOOK(XGetAVPack)
DASH_XAM_REDIRECT_HOOK(XGetLanguage)
DASH_XAM_REDIRECT_HOOK(XGetVideoStandard)
DASH_XAM_REDIRECT_HOOK(XGetVideoMode)
