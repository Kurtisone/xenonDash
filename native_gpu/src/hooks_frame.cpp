// native_gpu : crochets de cycle de vie D3D (fences, Clear, Swap) du Dashboard 6770.
//   9212A418  attente de fence (dev, fence, ...)   -> GPU nul : fence déjà atteinte
//   92128AF0  D3DDevice_Clear(dev, nb_rects, rects, flags, couleur ARGB, z, stencil)
//   92121EE8  D3DDevice_Swap / Present             -> présentation du tampon d'image natif

#include <atomic>
#include "ngpu.h"
#include "vk_backend.h"

namespace ngpu {
void NextFrame();
void NullGpuAttach(uint32_t dev);
void NullGpuCompleteNow(uint8_t* base, uint32_t dev);
}

NGPU_HOOK(9212A418) {
  if (ngpu::Observe()) return false;
  ngpu::NullGpuCompleteNow(base, ctx.r3.u32);
  return false;
}
// 92129788(dev, pos, taille) : attend que le pointeur de lecture du GPU (mot +60 du bloc de retour) sorte de la zone
// demandée -> GPU drainé : pointeur de lecture = position demandée.
NGPU_HOOK(92129788) {
  if (ngpu::Observe()) return false;
  uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  uint32_t idp = ngpu::Be32(base + dev + 10896);
  if (idp) ngpu::PutBe32(base + idp + 60, ctx.r4.u32);
  return false;
}
// 921296F0(dev, a, b) : attend le second mot du bloc de retour (+4), écrit par le GPU (fence secondaire) ;
// condition de sortie ((b - mot) & 3) == 0.
NGPU_HOOK(921296F0) {
  if (ngpu::Observe()) return false;
  uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  uint32_t idp = ngpu::Be32(base + dev + 10896);
  if (idp) ngpu::PutBe32(base + idp + 4, ctx.r5.u32);
  return false;
}

#ifdef NGPU_CORE_FRONT
// Facade sur ngpu_core (front6770.cpp) : Clear et Swap passent par le noyau de rendu.
namespace ngpu {
void FrontClear(const uint8_t* base, uint32_t dev, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, double z, uint32_t stencil);
void FrontSwap(const uint8_t* base, uint32_t dev);
}
NGPU_HOOK(92128AF0) {
  if (ngpu::Observe()) return false;
  if (std::getenv("DASH_NGPU_EVLOG")) std::fprintf(stderr, "[ev] clear flags=%X color=%08X rects=%u\n", ctx.r6.u32, ctx.r7.u32, ctx.r4.u32);
  ngpu::NullGpuAttach(ctx.r3.u32);
  ngpu::FrontClear(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.f1.f64, ctx.r8.u32);
  NGPU_ORIG(92128AF0);
  return true;
}

NGPU_HOOK(92121EE8) {
  if (ngpu::Observe()) { ngpu::NextFrame(); return false; }
  if (std::getenv("DASH_NGPU_EVLOG")) std::fprintf(stderr, "[ev] swap\n");
  ngpu::NullGpuAttach(ctx.r3.u32);
  // Avant le Swap d'origine : il remet le cache de registres du peripherique a autre chose (la surface a copier y est encore).
  ngpu::FrontSwap(base, ctx.r3.u32);
  NGPU_ORIG(92121EE8);
  ngpu::NextFrame();
  return true;
}
#else
NGPU_HOOK(92128AF0) {
  if (ngpu::Observe()) return false;
  if (std::getenv("DASH_NGPU_EVLOG")) std::fprintf(stderr, "[ev] clear flags=%X color=%08X rects=%u\n", ctx.r6.u32, ctx.r7.u32, ctx.r4.u32);
  ngpu::NullGpuAttach(ctx.r3.u32);
  uint32_t flags = ctx.r6.u32, color = ctx.r7.u32;
  if (flags & 0xF) {
    float c[4] = {((color >> 16) & 255) / 255.f, ((color >> 8) & 255) / 255.f, (color & 255) / 255.f, ((color >> 24) & 255) / 255.f};
    if (std::getenv("DASH_NGPU_CLEARGRAY")) { c[0] = c[1] = c[2] = 0.3f; c[3] = 1; }
    ngpu::VkClear(c);
  }
  NGPU_ORIG(92128AF0);
  return true;
}

NGPU_HOOK(92121EE8) {
  if (ngpu::Observe()) { ngpu::NextFrame(); return false; }
  if (std::getenv("DASH_NGPU_EVLOG")) std::fprintf(stderr, "[ev] swap\n");
  ngpu::NullGpuAttach(ctx.r3.u32);
  NGPU_ORIG(92121EE8);
  uint64_t n = ngpu::VkPresent();
  ngpu::NextFrame();
  if (n == 1 || n % 300 == 0) NGPU_LOG("image %llu présentée", (unsigned long long)n);
  return true;
}

#endif  // NGPU_CORE_FRONT

namespace ngpu {
static std::atomic<uint64_t> g_frame{0};
uint64_t FrameCounter() { return g_frame.load(); }
void NextFrame() { g_frame++; }
}  // namespace ngpu
