// Palier N4 : lecture d'entrée d'un module du HUD (signin.xex) quand le cache d'entrée de XAM est vide.
//
// sub_818C4240(port, drapeaux, &état) est le cœur de XamInputGetState. Avec le drapeau système 0x80000000 (celui de signin.xex :
// XamInputGetState(0, 0x80000000, ...)), il lit l'état EN CACHE du HUD (octet de présence 0x81C00000 + 212 + port) tant que le HUD
// détient l'entrée ; ce cache n'est rempli que par une vraie lecture, faite par le HUD pendant ses tours de boucle. Sur la console
// ce HUD tourne déjà quand signin.xex est chargé ; ici il ne tourne qu'après la première requête : le cache est vide,
// XamInputGetState renvoie 0x48F (ERROR_DEVICE_NOT_CONNECTED) et signin.xex quitte aussitôt (« 80004005 », docs/xam_recompile.md,
// palier N3). La manette virtuelle (common/src/kernel_xinputd.cpp) est pourtant annoncée à XAM et lisible.
//
// Ici : si XAM répond 0x48F pour le port 0 avec le drapeau système, l'état est lu directement sur la manette virtuelle
// (XInputdReadState : même source que le Dashboard et le HUD) et rendu comme le ferait la lecture directe de XAM
// (numéro de paquet à +0, XINPUT_GAMEPAD de 12 octets à +4). XAM n'est pas modifié ; le cache n'est pas rempli. L'exclusivité des
// entrées (le Dashboard ne reçoit rien tant que le HUD est affiché) est faite côté Dashboard (dash_xam_redirect.cpp) : inchangée.

#include <cstdint>
#include <cstdlib>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__XInputdReadState);
extern "C" void __imp__sub_818C4240(PPCContext& __restrict, uint8_t*);

extern "C" void sub_818C4240(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t port = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_818C4240(ctx, base);
  constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED
  static const bool off = [] {
    const char* e = std::getenv("DASH_XAM_INPUT_PATCH");  // 0 : correctif désactivé (diagnostic)
    return e && e[0] == '0';
  }();
  if (off || ctx.r3.u32 != kNotConnected || port != 0 || !(flags & 0x80000000u) || !out) return;
  // Même identifiant que common/src/kernel_xinputd.cpp (kDeviceId) ; XInputdReadState(id, &paquet, &état).
  PPCContext c = ctx;
  c.r3.u64 = 0x10000001u;
  c.r4.u64 = out;
  c.r5.u64 = out + 4;
  __imp__XInputdReadState(c, base);
  if (c.r3.u32 == 0) ctx.r3.u64 = 0;
}
