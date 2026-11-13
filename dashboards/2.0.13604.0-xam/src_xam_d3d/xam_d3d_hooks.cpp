// Palier N2 : crochets sur les fonctions D3D statiques de XAM 13604 (module recompile dash_xam).
//
// Chaque `sub_<adresse>` du code genere est un alias FAIBLE de `__imp__sub_<adresse>` : une definition forte dans cette unite
// de la DLL prend le pas pour tous les appels directs (et pour la table de fonctions, qui enregistre `sub_<adresse>`).
// L'enveloppeur passe par `hudd3d_call`, definie dans l'executable (native_gpu/src/hud_front.cpp), qui decide d'appeler ou non
// la fonction d'origine (`orig`) avant/apres son traitement. Aucun contenu Microsoft : des adresses.

#include <cstdint>

#include <rex/ppc/context.h>

extern "C" void hudd3d_call(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*));

#define X(A)                                                                                           \
  extern "C" void __imp__sub_##A(PPCContext& __restrict, uint8_t*);                                   \
  extern "C" void sub_##A(PPCContext& __restrict ctx, uint8_t* base) {                                \
    hudd3d_call(0x##A, ctx, base, __imp__sub_##A);                                                     \
  }
#include "xam_d3d_entries.inc"
#undef X
