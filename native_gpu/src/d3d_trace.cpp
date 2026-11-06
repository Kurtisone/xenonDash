// native_gpu : point d'entrée unique des remplacements de fonctions D3D de dash.xex 6770.
// Chaque fonction listée dans d3d6770_entries.inc est remplacée par un enveloppeur (symbole fort qui prend le pas sur
// l'alias faible `sub_XXXXXXXX` du code généré). L'enveloppeur :
//   1. compte l'appel et journalise les premiers (traçage, activé par DASH_D3D_TRACE=<fichier>) ;
//   2. appelle le crochet natif `ngpu_hook_XXXXXXXX` s'il existe (symbole faible, voir NGPU_HOOK) ;
//   3. sinon, ou si le crochet renvoie false, appelle la fonction d'origine (`__imp__sub_XXXXXXXX`).

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "ngpu.h"

namespace {
struct Slot { uint32_t addr; std::atomic<uint64_t> frame_calls{0}, total{0}; int logged = 0; };
Slot g_slots[4096];
int g_nslots = 0;
FILE* g_out = nullptr;
std::once_flag g_once;
std::atomic<uint64_t> g_frame{0};
std::mutex g_mu;

void Init() {
  const char* p = std::getenv("DASH_D3D_TRACE");
  if (p && *p) g_out = std::fopen(p, "w");
}
int Register(uint32_t a) {
  std::lock_guard<std::mutex> l(g_mu);
  for (int i = 0; i < g_nslots; i++) if (g_slots[i].addr == a) return i;
  g_slots[g_nslots].addr = a;
  return g_nslots++;
}
}  // namespace

namespace ngpu {
bool Verbose() {
  static const bool v = std::getenv("DASH_NGPU_VERBOSE") != nullptr;
  return v;
}
void TraceHit(int slot, uint32_t a, PPCContext& ctx) {
  std::call_once(g_once, Init);
  if (!g_out) return;
  Slot& s = g_slots[slot];
  s.addr = a;
  s.frame_calls++; s.total++;
  if (s.logged < 6) {
    s.logged++;
    std::lock_guard<std::mutex> l(g_mu);
    std::fprintf(g_out, "[call %08X #%llu] r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X f1=%g lr=%08X\n", a,
                 (unsigned long long)s.total.load(), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                 ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.f1.f64, (uint32_t)ctx.lr);
    std::fflush(g_out);
  }
}
// Fin d'image (appelée sur Swap) : résumé des appels par image.
void TraceFrame(uint32_t a) {
  if (!g_out) return;
  std::lock_guard<std::mutex> l(g_mu);
  uint64_t f = g_frame++;
  if (f < 12 || f % 100 == 0) {
    std::fprintf(g_out, "== image %llu (swap %08X) ==", (unsigned long long)f, a);
    for (int i = 0; i < 4096; i++)
      if (g_slots[i].frame_calls.load()) std::fprintf(g_out, " %08X:%llu", g_slots[i].addr, (unsigned long long)g_slots[i].frame_calls.load());
    std::fprintf(g_out, "\n");
    std::fflush(g_out);
  }
  for (int i = 0; i < 4096; i++) g_slots[i].frame_calls = 0;
}
}  // namespace ngpu

#define X(A)                                                                          \
  extern "C" void __imp__sub_##A(PPCContext& __restrict, uint8_t*);                   \
  extern "C" __attribute__((weak)) bool ngpu_hook_##A(PPCContext&, uint8_t*);         \
  extern "C" void sub_##A(PPCContext& __restrict ctx, uint8_t* base) {                \
    static int slot = Register(0x##A);                                                \
    ngpu::TraceHit(slot, 0x##A, ctx);                                                 \
    if (0x##A == 0x92121EE8) ngpu::TraceFrame(0x##A);                                 \
    if (ngpu_hook_##A && ngpu_hook_##A(ctx, base)) return;                            \
    __imp__sub_##A(ctx, base);                                                        \
  }
#include "d3d6770_entries.inc"
#undef X
