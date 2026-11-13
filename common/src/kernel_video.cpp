// xenondash common - appels vidéo du noyau pour XAM recompilé et le HUD (palier G).
//
// VdQuerySystemCommandBuffer : le SDK le laisse en stub (r3 inchangé, soit 0). XAM l'appelle dans des boucles d'attente
// (sub_8189DFF0 : « répéter tant que le résultat est nul », fil du HUD) : avec un résultat nul, le fil tourne sans fin à
// 100 % d'un cœur (4,5 millions d'appels en 20 s). Ici : 1 (tampon de commandes système disponible) pour les appels venant
// de XAM ; les autres appelants gardent le stub du SDK. Le HUD n'écrit pas dans ce tampon par cet appel.

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
#include <cstdlib>
#include <string>

#include <rex/logging.h>

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
    if (p >= 0x10000 && p < 0xF0000000u) {
      std::string e;
      for (uint32_t i = 0; i < 16; ++i) {
        std::snprintf(b, sizeof b, " %08X", rd(p + i * 4));
        e += b;
      }
      REXLOG_INFO("[vd]   tampon {:08X} :{}", p, e);
    }
  }
  sdk(ctx, base);
}
