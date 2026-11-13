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

// Exécute l'import : XAM recompilé si redirigé, sinon le HLE du SDK (même fonction que sans ce fichier).
// Trace : première exécution de chaque import (voie et compteur), puis tous les 1000 appels.
void Dispatch(Slot& s, PPCContext& ctx, uint8_t* base) {
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

  // DASH_TEST_PEEK=<adresse invitée hexadécimale> : journalise 64 mots de la mémoire invitée avant l'appel (diagnostic).
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
  REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : appel");
  Dispatch(*slot, ctx, base);
  REXLOG_INFO("[xamredir] test XamShowMessageBoxUI : retour r3=0x{:08X}, résultat={}",
              static_cast<uint32_t>(ctx.r3.u64),
              __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + result)));
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
