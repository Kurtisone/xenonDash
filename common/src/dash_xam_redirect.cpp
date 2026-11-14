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

// Exécute l'import : XAM recompilé si redirigé, sinon le HLE du SDK (même fonction que sans ce fichier).
// Trace : première exécution de chaque import (voie et compteur), puis tous les 1000 appels.
void Dispatch(Slot& s, PPCContext& ctx, uint8_t* base) {
  // Entrees : tant que le HUD est affiche (et jusqu'au relachement de tous les boutons ensuite), le Dashboard ne recoit plus rien, comme un
  // titre sous une interface systeme (sinon l'appui qui ferme la boite declenche aussi une action du Dashboard).
  static std::atomic<bool> wait_release{false};
  const bool is_state = std::string_view(s.name) == "XamInputGetState";
  const bool is_key = std::string_view(s.name) == "XamInputGetKeystrokeEx";
  if ((is_state || is_key) && ngpu::HudActive) {
    const bool hud = ngpu::HudActive();
    if (hud) wait_release.store(true);
    if (hud || wait_release.load()) {
      if (is_key) {
        ctx.r3.u64 = 0x10D2;  // ERROR_EMPTY
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
std::atomic<rex::system::KernelState*> g_ks{nullptr};
std::atomic<rex::system::UserModule*> g_xam{nullptr};

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

// Chaîne UTF-16 big-endian dans le tas système invité ; renvoie son adresse invitée.
uint32_t PutWide(const char16_t* s) {
  const size_t n = std::char_traits<char16_t>::length(s) + 1;  // avec le zéro final
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(n * 2));
  auto* dst = reinterpret_cast<uint16_t*>(REX_KERNEL_MEMORY()->virtual_membase() + a);
  for (size_t i = 0; i < n; ++i) dst[i] = __builtin_bswap16(static_cast<uint16_t>(s[i]));
  return a;
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
void RunMessageBoxTest(rex::system::KernelState* ks) {
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts) {
    REXLOG_WARN("[xamredir] test XamShowMessageBoxUI : pas de thread invité");
    return;
  }
  Slot* slot = FindSlot("XamShowMessageBoxUI");
  if (!slot) return;
  uint8_t* base = ks->memory()->virtual_membase();
  PPCContext& tc = *ts->context();

  const uint32_t title = PutWide(u"Xenondash");
  const uint32_t text = PutWide(u"XamShowMessageBoxUI : test de redirection vers XAM recompile");
  const uint32_t b0 = PutWide(u"OK"), b1 = PutWide(u"Annuler");
  const uint32_t buttons = REX_KERNEL_MEMORY()->SystemHeapAlloc(8);
  *reinterpret_cast<uint32_t*>(base + buttons) = __builtin_bswap32(b0);
  *reinterpret_cast<uint32_t*>(base + buttons + 4) = __builtin_bswap32(b1);
  const uint32_t result = REX_KERNEL_MEMORY()->SystemHeapAlloc(4);
  *reinterpret_cast<uint32_t*>(base + result) = __builtin_bswap32(0xFFFFFFFFu);

  // XOVERLAPPED (0x20 octets, à zéro) : XAM refuse overlapped nul avec pResult non nul (chemin d'erreur 87).
  const uint32_t overlapped = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x20);
  std::memset(base + overlapped, 0, 0x20);

  // Neuf arguments : le 9e (overlapped) est sur la pile, à r1 + 0x54 de l'appelant. XAM le lit à 1412(r1) après un
  // stwu de 1328 octets : 1412 - 1328 = 0x54 (convention du SDK, FunctionDispatcher::Execute).
  PPCContext ctx = tc;
  const uint32_t sp = static_cast<uint32_t>(tc.r1.u64) - 0x100;
  *reinterpret_cast<uint32_t*>(base + sp + 0x54) = __builtin_bswap32(overlapped);
  ctx.r1.u64 = sp;
  ctx.r3.u64 = 0;                 // utilisateur 0
  ctx.r4.u64 = title;
  ctx.r5.u64 = text;
  ctx.r6.u64 = 2;                 // nombre de boutons
  ctx.r7.u64 = buttons;
  ctx.r8.u64 = 0;                 // bouton actif
  ctx.r9.u64 = 0;                 // drapeaux
  ctx.r10.u64 = result;

  PeekGuest(base);
  REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : appel");
  Dispatch(*slot, ctx, base);
  REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : retour r3=0x{:08X}, résultat={}",
              static_cast<uint32_t>(ctx.r3.u64),
              __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + result)));
  if (static_cast<uint32_t>(ctx.r3.u64) != 0x3E5) return;

  // Attente de la réponse (XOVERLAPPED : InternalLow à +0 vaut 0x3E5 tant que la requête est en cours ; dwExtendedError
  // à +0x18). La boîte reçoit ses touches de XamInputGetKeystrokeHud ; DASH_TEST_MSGBOX_WAIT = durée en secondes (60).
  const char* w = std::getenv("DASH_TEST_MSGBOX_WAIT");
  const int wait_ms = (w && *w ? std::atoi(w) : 60) * 1000;
  auto word = [&](uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + ea)); };
  for (int t = 0; t < wait_ms; t += 50) {
    if (word(overlapped) != 0x3E5) {
      REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : TERMINÉ après {} ms, overlapped.InternalLow=0x{:08X}, "
                  "erreur étendue=0x{:08X}, bouton choisi={} (0xFFFFFFFF = aucun)",
                  t, word(overlapped), word(overlapped + 0x18), word(result));
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REXLOG_WARN("[xamredir] test XamShowMessageBoxUI : NON terminé après {} ms (overlapped.InternalLow=0x{:08X}, bouton={})",
              wait_ms, word(overlapped), word(result));
}

// Attend la fin d'une requête asynchrone (XOVERLAPPED : InternalLow à +0 vaut 0x3E5 tant qu'elle est en cours). Renvoie vrai si
// elle s'est terminée avant `wait_ms`.
bool WaitOverlapped(uint8_t* base, uint32_t overlapped, int wait_ms) {
  auto word = [&](uint32_t ea) { return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + ea)); };
  for (int t = 0; t < wait_ms; t += 50) {
    if (word(overlapped) != 0x3E5) {
      REXLOG_INFO("[xamredir] test : TERMINÉ après {} ms, overlapped.InternalLow=0x{:08X}, erreur étendue=0x{:08X}", t,
                  word(overlapped), word(overlapped + 0x18));
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REXLOG_WARN("[xamredir] test : NON terminé après {} ms (overlapped.InternalLow=0x{:08X})", wait_ms, word(overlapped));
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
  const char* env = std::getenv("DASH_XAM_REDIRECT");
  if (!env || !*env) {
    REXLOG_INFO("[xamredir] DASH_XAM_REDIRECT absent : imports du Dashboard servis par le HLE");
  } else if (!xam) {
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
DASH_XAM_REDIRECT_HOOK(XamVoiceCreate)
DASH_XAM_REDIRECT_HOOK(XamVoiceHeadsetPresent)
DASH_XAM_REDIRECT_HOOK(XamVoiceSubmitPacket)
DASH_XAM_REDIRECT_HOOK(XamVoiceClose)
DASH_XAM_REDIRECT_HOOK(XGetAVPack)
DASH_XAM_REDIRECT_HOOK(XGetLanguage)
DASH_XAM_REDIRECT_HOOK(XGetVideoStandard)
DASH_XAM_REDIRECT_HOOK(XGetVideoMode)
