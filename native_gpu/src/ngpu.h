// native_gpu : définitions communes (accès à la mémoire invitée, macros de crochets, journal).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace ngpu {

// Mémoire invitée : grand-boutiste, base + adresse.
inline uint32_t Be32(const void* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
inline uint16_t Be16(const void* p) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
inline void PutBe32(void* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }
inline float BeF32(const void* p) { uint32_t v = Be32(p); float f; std::memcpy(&f, &v, 4); return f; }

// Ensemble de l'état du backend natif.
bool Verbose();
bool Observe();  // DASH_NGPU_OBSERVE=N : mode observation (oracle xenos actif) ; les dessins au-delà du N-ième de chaque image sont supprimés
bool Enabled();  // false avec DASH_NGPU_DISABLE=1 (les crochets natifs sont ignorés ; sert aux mesures de référence)
#define NGPU_LOG(...)                                   \
  do {                                                  \
    if (ngpu::Verbose()) {                              \
      std::fprintf(stderr, "[ngpu] " __VA_ARGS__);      \
      std::fputc('\n', stderr);                         \
    }                                                   \
  } while (0)
#define NGPU_ERR(...)                                   \
  do {                                                  \
    std::fprintf(stderr, "[ngpu] ERREUR " __VA_ARGS__); \
    std::fputc('\n', stderr);                           \
  } while (0)

}  // namespace ngpu

// Crochet sur une fonction invitée `sub_<ADRESSE>` : le dispositif de d3d_trace.cpp appelle
// ngpu_hook_<ADRESSE> (symbole faible) avant la fonction d'origine ; renvoyer true = la fonction est remplacée
// (rien d'autre n'est exécuté), false = on enchaîne sur la fonction d'origine. Pour une action « après », le
// crochet appelle lui-même NGPU_ORIG(ADRESSE) puis renvoie true.
#define NGPU_HOOK(ADDR)                                                                        \
  extern "C" void __imp__sub_##ADDR(PPCContext& __restrict, uint8_t*);                         \
  extern "C" bool ngpu_hook_##ADDR([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* base)
#define NGPU_ORIG(ADDR) __imp__sub_##ADDR(ctx, base)
