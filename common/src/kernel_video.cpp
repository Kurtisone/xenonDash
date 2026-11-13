// xenondash common - appels vidéo du noyau pour XAM recompilé et le HUD (palier G).
//
// VdQuerySystemCommandBuffer : le SDK le laisse en stub (r3 inchangé, soit 0). XAM l'appelle dans des boucles d'attente
// (sub_8189DFF0 : « répéter tant que le résultat est nul », fil du HUD) : avec un résultat nul, le fil tourne sans fin à
// 100 % d'un cœur (4,5 millions d'appels en 20 s). Ici : 1 (tampon de commandes système disponible) pour les appels venant
// de XAM ; les autres appelants gardent le stub du SDK. Le HUD n'écrit pas dans ce tampon par cet appel.

#include <cstring>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "dash_xam_runtime.h"
#include "kernel_sdk_fn.h"

REX_HOOK_RAW(__imp__VdQuerySystemCommandBuffer) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdQuerySystemCommandBuffer");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr))) ctx.r3.u64 = 1;
  else sdk(ctx, base);
}

// VdSetSystemCommandBuffer(descripteur) : le SDK l'ignore (stub). XAM l'appelle à chaque image du HUD (rendu XUI de XAM
// recompilé : commandes GPU dans un tampon système). DASH_TRACE_VD=<n> : journalise les n premiers descripteurs (14 mots)
// et les premiers mots du tampon désigné (diagnostic, palier G).
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REX_HOOK_RAW(__imp__VdSetSystemCommandBuffer) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdSetSystemCommandBuffer");
  static const long max_trace = [] {
    const char* e = std::getenv("DASH_TRACE_VD");
    return e && *e ? std::atol(e) : 0L;
  }();
  static std::atomic<long> n{0};
  static std::atomic<uint64_t> total{0};
  total.fetch_add(1);
  if (max_trace > 0 && n.fetch_add(1) < max_trace && ctx.r3.u32) {
    auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
    std::string d;
    char b[16];
    for (uint32_t i = 0; i < 14; ++i) {
      std::snprintf(b, sizeof b, " %08X", rd(ctx.r3.u32 + i * 4));
      d += b;
    }
    REXLOG_INFO("[vd] VdSetSystemCommandBuffer({:08X}) lr={:08X} total={} :{}", ctx.r3.u32,
                static_cast<uint32_t>(ctx.lr), total.load(), d);
    const uint32_t p = rd(ctx.r3.u32);
    if (p >= 0x10000 && p < 0x20000000u) {  // adresse physique
      const uint8_t* phys = rex::system::kernel_memory()->TranslatePhysical(0);
      std::string e;
      for (uint32_t i = 0; i < 16; ++i) {
        std::snprintf(b, sizeof b, " %08X", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(phys + p + i * 4)));
        e += b;
      }
      REXLOG_INFO("[vd]   tampon {:08X} :{}", p, e);
    }
  }
  // DASH_TRACE_VD_SCAN=<n> (palier N2) : compte les mots non nuls du tampon (taille = mot 1 du descripteur, en mots) à chaque
  // soumission et journalise (au plus n lignes) quand ce compte change : analyse de la structure des tampons du HUD.
  static const long max_scan = [] {
    const char* e = std::getenv("DASH_TRACE_VD_SCAN");
    return e && *e ? std::atol(e) : 0L;
  }();
  if (max_scan > 0 && ctx.r3.u32) {
    static std::atomic<long> nscan{0};
    static std::atomic<uint32_t> last_nz{0xFFFFFFFFu};
    auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
    const uint32_t p = rd(ctx.r3.u32), words = rd(ctx.r3.u32 + 4);
    // Le descripteur donne une adresse PHYSIQUE : ne pas la lire par la vue virtuelle (autre mémoire).
    const uint8_t* phys = rex::system::kernel_memory()->TranslatePhysical(0);
    auto rdp = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(phys + a)); };
    if (p >= 0x10000 && p < 0x20000000u && words > 0 && words < 0x100000) {
      uint32_t nz = 0, first = 0xFFFFFFFFu, lastnz = 0;
      for (uint32_t i = 0; i < words; ++i)
        if (rdp(p + i * 4)) { ++nz; if (first == 0xFFFFFFFFu) first = i; lastnz = i; }
      if (nz != last_nz.exchange(nz) && nscan.fetch_add(1) < max_scan) {
        std::string e;
        char b[16];
        for (uint32_t i = 0; i < 24 && first != 0xFFFFFFFFu && first + i < words; ++i) {
          std::snprintf(b, sizeof b, " %08X", rdp(p + (first + i) * 4));
          e += b;
        }
        if (const char* dir = std::getenv("DASH_TRACE_VD_DUMP")) {  // fichiers pour l'analyse hors ligne (structure des paquets)
          char fn[512];
          std::snprintf(fn, sizeof fn, "%s/vd_%03ld_%u.bin", dir, (long)nscan.load(), words);
          if (FILE* f = std::fopen(fn, "wb")) {
            std::fwrite(phys + p, 4, words, f);
            std::fclose(f);
          }
        }
        REXLOG_INFO("[vdscan] tampon {:08X} mots={} non-nuls={} premier={} dernier={} :{}", p, words, nz,
                    first == 0xFFFFFFFFu ? -1 : (int)first, (int)lastnz, e);
      }
    }
  }
  sdk(ctx, base);
}

// ---- notifications graphiques vers XAM (palier N2) ----
// Sur la console le noyau appelle les rappels enregistres par VdRegisterXamGraphicsNotification (celui de XAM : sub_818A04F8, r3 = 0, r4 = message
// 1 a 9, r5 = donnees) : message 1 = VdCallGraphicsNotificationRoutines(1, BufferScaling...) du D3D du titre (tailles du tampon d'image et
// de la surface de dessin, 64 octets : u16 fb_largeur, fb_hauteur, bb_largeur, bb_hauteur puis rectangles), message 2 = X_DISPLAY_INFO (88
// octets, tailles de l'affichage aux decalages 0x48/0x4A). Le SDK n'en fait rien (stubs) : XAM gardait des zeros et ses calculs de mise en page
// divisaient par zero (le premier dessin de chaque image du HUD avait des sommets NaN). DASH_XAM_GFXNOTIFY=0 : n'envoie rien.
#include <array>
#include <chrono>
#include <mutex>
#include <thread>

#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>

namespace {
std::mutex g_gfx_mutex;
uint32_t g_gfx_cb = 0;                        // rappel de XAM
bool g_gfx_have_scaling = false;              // dernier message 1 recu du D3D du titre
std::array<uint8_t, 64> g_gfx_scaling{};      // ... ses 64 octets (grand-boutistes)

bool GfxNotifyEnabled() {
  static const bool on = [] {
    const char* e = std::getenv("DASH_XAM_GFXNOTIFY");
    return !(e && *e == '0');
  }();
  return on;
}

// Appelle le rappel de XAM (message, donnees copiees dans le tas systeme invite) depuis le fil invite courant.
void CallXamGfxCallback(uint32_t cb, uint32_t message, const uint8_t* data, size_t size, const PPCContext& caller, uint8_t* base) {
  auto* ks = rex::system::kernel_state();
  PPCFunc* fn = ks->function_dispatcher()->GetFunction(cb);
  if (!fn) {
    REXLOG_WARN("[vd] rappel de XAM 0x{:08X} : fonction absente", cb);
    return;
  }
  const uint32_t guest = ks->memory()->SystemHeapAlloc(0x58);
  std::memset(base + guest, 0, 0x58);
  std::memcpy(base + guest, data, size);
  PPCContext n = caller;
  n.r1.u64 = static_cast<uint32_t>(n.r1.u64) - 0x100;
  n.r3.u64 = 0;
  n.r4.u64 = message;
  n.r5.u64 = guest;
  fn(n, base);
  REXLOG_INFO("[vd] notification graphique {} remise a XAM (rappel 0x{:08X})", message, cb);
}
}  // namespace

// VdRegisterXamGraphicsNotification(rappel, contexte) : le SDK l'ignore (stub) ; ici le rappel est memorise et recoit aussitot (fil dedie,
// apres un court delai : XAM acheve son initialisation) le message 2 puis le dernier message 1 deja emis par le D3D du titre.
REX_HOOK_RAW(__imp__VdRegisterXamGraphicsNotification) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdRegisterXamGraphicsNotification");
  static PPCFunc* info_fn = dash::SdkFunction("__imp__VdGetCurrentDisplayInformation");
  if (sdk) sdk(ctx, base);
  const uint32_t cb = ctx.r3.u32;
  REXLOG_INFO("[vd] VdRegisterXamGraphicsNotification(0x{:08X}, 0x{:08X}) lr={:08X}", cb, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  if (!cb || !info_fn || !GfxNotifyEnabled()) return;
  {
    std::lock_guard<std::mutex> lock(g_gfx_mutex);
    g_gfx_cb = cb;
  }
  auto* ks = rex::system::kernel_state();
  std::thread([ks, cb] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    static rex::system::object_ref<rex::system::XHostThread> host;  // vit jusqu'a la fin du processus
    host = rex::system::object_ref<rex::system::XHostThread>(new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, cb] {
      auto* ts = rex::runtime::ThreadState::Get();
      if (!ts) return 0;
      uint8_t* mem = ks->memory()->virtual_membase();
      PPCFunc* info = dash::SdkFunction("__imp__VdGetCurrentDisplayInformation");
      if (!info) return 0;
      const uint32_t data = ks->memory()->SystemHeapAlloc(0x58);
      std::memset(mem + data, 0, 0x58);
      PPCContext c = *ts->context();
      c.r1.u64 = static_cast<uint32_t>(c.r1.u64) - 0x100;
      c.r3.u64 = data;
      info(c, mem);  // X_DISPLAY_INFO de l'affichage courant
      CallXamGfxCallback(cb, 2, mem + data, 0x58, *ts->context(), mem);
      std::array<uint8_t, 64> scaling{};
      bool have;
      {
        std::lock_guard<std::mutex> lock(g_gfx_mutex);
        have = g_gfx_have_scaling;
        scaling = g_gfx_scaling;
      }
      if (have) CallXamGfxCallback(cb, 1, scaling.data(), scaling.size(), *ts->context(), mem);
      return 0;
    }));
    host->set_name("XAM gfx notify");
    host->Create();
  }).detach();
}

// VdCallGraphicsNotificationRoutines(message, donnees) : appele par le D3D du titre (dash.xex, XDK 20158 : message 1, BufferScaling) ; le
// noyau le transmet aux rappels enregistres (ici celui de XAM). Memorise pour un rappel enregistre plus tard.
REX_HOOK_RAW(__imp__VdCallGraphicsNotificationRoutines) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdCallGraphicsNotificationRoutines");
  const uint32_t message = ctx.r3.u32, data = ctx.r4.u32;
  if (GfxNotifyEnabled() && message == 1 && data) {
    std::array<uint8_t, 64> copy;
    std::memcpy(copy.data(), base + data, copy.size());
    uint32_t cb;
    {
      std::lock_guard<std::mutex> lock(g_gfx_mutex);
      g_gfx_have_scaling = true;
      g_gfx_scaling = copy;
      cb = g_gfx_cb;
    }
    REXLOG_INFO("[vd] VdCallGraphicsNotificationRoutines(1, {:08X}) lr={:08X} : tampon {}x{}, surface {}x{}", data, static_cast<uint32_t>(ctx.lr),
                (copy[0] << 8) | copy[1], (copy[2] << 8) | copy[3], (copy[4] << 8) | copy[5], (copy[6] << 8) | copy[7]);
    if (cb) CallXamGfxCallback(cb, 1, copy.data(), copy.size(), ctx, base);
  }
  if (sdk) sdk(ctx, base);
}
