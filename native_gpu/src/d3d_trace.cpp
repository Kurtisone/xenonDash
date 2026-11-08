// native_gpu : point d'entrée unique des remplacements de fonctions D3D de dash.xex 6770.
// Chaque fonction listée dans d3d6770_entries.inc est remplacée par un enveloppeur (symbole fort qui prend le pas sur
// l'alias faible `sub_XXXXXXXX` du code généré). L'enveloppeur :
//   1. compte l'appel et journalise les premiers (traçage, activé par DASH_D3D_TRACE=<fichier>) ;
//   2. appelle le crochet natif `ngpu_hook_XXXXXXXX` s'il existe (symbole faible, voir NGPU_HOOK) ;
//   3. sinon, ou si le crochet renvoie false, appelle la fonction d'origine (`__imp__sub_XXXXXXXX`).

#include <atomic>
#include <exception>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include <rex/system/kernel_state.h>
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
  if (g_out && std::getenv("DASH_D3D_WATCH")) {
    std::thread([] {
      static uint64_t prev[4096] = {};
      for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        std::lock_guard<std::mutex> l(g_mu);
        std::fprintf(g_out, "[watch]");
        for (int k = 0; k < 6; k++) {
          int best = -1; uint64_t bd = 0;
          for (int i = 0; i < g_nslots; i++) { uint64_t d = g_slots[i].total.load() - prev[i]; if (d > bd) { bd = d; best = i; } }
          if (best < 0) break;
          std::fprintf(g_out, " %08X:%llu", g_slots[best].addr, (unsigned long long)bd);
          prev[best] += bd;
        }
        for (int i = 0; i < g_nslots; i++) prev[i] = g_slots[i].total.load();
        std::fprintf(g_out, "\n"); std::fflush(g_out);
      }
    }).detach();
  }
}
int Register(uint32_t a) {
  std::lock_guard<std::mutex> l(g_mu);
  for (int i = 0; i < g_nslots; i++) if (g_slots[i].addr == a) return i;
  g_slots[g_nslots].addr = a;
  return g_nslots++;
}
}  // namespace

namespace ngpu {
void SwapTick();
bool Enabled() {
  static const bool v = std::getenv("DASH_NGPU_DISABLE") == nullptr;
  return v;
}
bool Observe() {
  static const bool v = std::getenv("DASH_NGPU_OBSERVE") != nullptr;
  return v;
}
bool Verbose() {
  static const bool v = std::getenv("DASH_NGPU_VERBOSE") != nullptr;
  return v;
}
void TraceHit(int slot, uint32_t a, PPCContext& ctx) {
  std::call_once(g_once, Init);
  if (!g_out) return;
  static const long seq = std::getenv("DASH_D3D_SEQ") ? std::atol(std::getenv("DASH_D3D_SEQ")) : -1;
  static std::atomic<int> seqlines{0};
  if ((long)g_frame.load() == seq && a != 0x92120C98 && seqlines < 6000) {
    seqlines++;
    std::lock_guard<std::mutex> l(g_mu);
    std::fprintf(g_out, "[seq %08X] r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X f1=%g f2=%g lr=%08X\n", a, ctx.r3.u32, ctx.r4.u32,
                 ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, ctx.f1.f64, ctx.f2.f64, (uint32_t)ctx.lr);
  }
  if (std::getenv("DASH_D3D_FINDSHADER")) {
    auto* m = rex::system::kernel_memory();
    uint32_t regs[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
    for (int i = 0; i < 4; i++) {
      uint32_t p = regs[i];
      if (p >= 0x92000000 && p < 0x92800000) {
        uint32_t w = Be32(m->TranslateVirtual<uint8_t*>(p));
        if ((w & 0xFFFFFFFE) == 0x102A1100) { std::lock_guard<std::mutex> l(g_mu); std::fprintf(g_out, "[shaderptr %08X] arg r%d=%08X lr=%08X\n", a, i + 3, p, (uint32_t)ctx.lr); }
      }
    }
  }
  if (std::getenv("DASH_D3D_FINDDECL")) {
    auto* m = rex::system::kernel_memory();
    uint32_t regs[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
    for (int i = 0; i < 4; i++) {
      uint32_t p = regs[i];
      if ((p >= 0x40000000 && p < 0x40800000) || (p >= 0x70000000 && p < 0x70200000) || (p >= 0x92000000 && p < 0x92800000)) {
        auto* q = m->TranslateVirtual<uint8_t*>(p);
        for (int w = 0; w < 8; w++) {
          uint32_t v = Be32(q + 4 * w);
          if (v == 0x002C23A5 || v == 0x001A23A6 || v == 0x00182886) { std::lock_guard<std::mutex> l(g_mu); std::fprintf(g_out, "[declptr %08X] arg r%d=%08X word %d lr=%08X\n", a, i + 3, p, w, (uint32_t)ctx.lr); break; }
        }
      }
    }
  }
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
// Compteur d'images léger (DASH_NGPU_FPSLOG=1) : fonctionne aussi avec l'émulation « xenos » (mesures de référence).
void SwapTick() {
  static const bool on = std::getenv("DASH_NGPU_FPSLOG") != nullptr;
  if (!on) return;
  static const auto t0 = std::chrono::steady_clock::now();
  static std::atomic<uint64_t> n{0};
  uint64_t k = ++n;
  if (k % 100 == 0) std::fprintf(stderr, "[fps] swap %llu t=%.3f\n", (unsigned long long)k, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}
// Fin d'image (appelée sur Swap) : résumé des appels par image.
void TraceFrame(uint32_t a) {
  if (!g_out) return;
  std::lock_guard<std::mutex> l(g_mu);
  uint64_t f = g_frame++;
  if (f < 12 || f % 100 == 0) {
    static const auto t0 = std::chrono::steady_clock::now();
    std::fprintf(g_out, "== image %llu t=%.3f (swap %08X) ==", (unsigned long long)f, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), a);
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
    if (0x##A == 0x92121EE8) { ngpu::TraceFrame(0x##A); ngpu::SwapTick(); }                                 \
    if (ngpu::Enabled() && ngpu_hook_##A) {                                           \
      try { if (ngpu_hook_##A(ctx, base)) return; }                                   \
      catch (const std::exception& e) { std::fprintf(stderr, "[ngpu] EXCEPTION dans le crochet %08X : %s\n", 0x##A, e.what()); }   \
    }                            \
    __imp__sub_##A(ctx, base);                                                        \
  }
#include "d3d6770_entries.inc"
#undef X
