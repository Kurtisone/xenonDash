// Palier N4 : lecture d'entrée des interfaces système de XAM (HUD, Guide, clavier virtuel, boîtes, signin.xex).
//
// sub_818C4240(port, drapeaux, &état) est le cœur de XamInputGetState. Avec le drapeau système 0x80000000 (celui des interfaces du
// HUD, aussi de signin.xex : XamInputGetState(0, 0x80000000, ...)), il lit l'état EN CACHE du HUD (octet de présence 0x81C00000 + 212
// + port) tant que le HUD détient l'entrée ; ce cache n'est rempli que par une vraie lecture, faite par le HUD pendant ses tours de
// boucle. Sur la console ce HUD tourne déjà quand signin.xex est chargé ; ici il ne tourne qu'après la première requête : le cache
// est vide, XamInputGetState renvoie 0x48F (ERROR_DEVICE_NOT_CONNECTED) et signin.xex quitte aussitôt (« 80004005 »,
// docs/xam_recompile.md, palier N3). La manette virtuelle (common/src/kernel_xinputd.cpp) est pourtant annoncée à XAM et lisible.
//
// 1. Si XAM répond 0x48F pour un port (0 à 3) avec le drapeau système, l'état est lu directement sur la manette virtuelle de ce port
//    (XInputdReadState : même source que le Dashboard et le HUD ; un port sans manette reste en 0x48F) et rendu comme le ferait la
//    lecture directe de XAM (numéro de paquet à +0, XINPUT_GAMEPAD de 12 octets à +4). XAM n'est pas modifié ; le cache n'est pas rempli.
// 2. Les interfaces du HUD ne lisent QUE le port de l'utilisateur pour lequel elles sont ouvertes (mesure du 2026-11-27 : le Guide et
//    les boîtes interrogent XamInputGetKeystrokeHud(utilisateur 0), jamais les ports 1 à 3). Une seconde manette annoncée à XAM
//    (port 1) n'y changeait donc rien. Une lecture système réussie est complétée par l'état des AUTRES manettes annoncées (boutons
//    en OU, gâchettes au maximum, sticks de plus grande amplitude) : chaque manette pilote ces interfaces, quel que soit le port pour
//    lequel elles ont été ouvertes. Lectures non système (titres, activité) : inchangées. DASH_XAM_INPUT_MERGE=0 le désactive.
//
// L'exclusivité des entrées (le Dashboard ne reçoit rien tant que le HUD est affiché) est faite côté Dashboard (dash_xam_redirect.cpp) :
// inchangée.

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/input/input.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__XInputdReadState);
extern "C" void __imp__sub_818C4240(PPCContext& __restrict, uint8_t*);
extern "C" uint32_t dash_xinputd_pad_mask();  // common/src/kernel_xinputd.cpp : emplacements annoncés à XAM (bit N = manette N)

namespace {

// Plie `src` dans `dst` (mêmes règles que rex::input::MergeInto, sur la mémoire invitée grand-boutiste).
void MergeGamepad(rex::input::X_INPUT_GAMEPAD& dst, const rex::input::X_INPUT_GAMEPAD& src) {
  auto axis = [](rex::be<int16_t>& a, int16_t b) {
    const int16_t va = a;
    if (std::abs(int(b)) > std::abs(int(va))) a = b;
  };
  dst.buttons = uint16_t(uint16_t(dst.buttons) | uint16_t(src.buttons));
  if (src.left_trigger > dst.left_trigger) dst.left_trigger = src.left_trigger;
  if (src.right_trigger > dst.right_trigger) dst.right_trigger = src.right_trigger;
  axis(dst.thumb_lx, src.thumb_lx);
  axis(dst.thumb_ly, src.thumb_ly);
  axis(dst.thumb_rx, src.thumb_rx);
  axis(dst.thumb_ry, src.thumb_ry);
}

}  // namespace

extern "C" void sub_818C4240(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t port = ctx.r3.u32, flags = ctx.r4.u32, out = ctx.r5.u32;
  __imp__sub_818C4240(ctx, base);
  constexpr uint32_t kNotConnected = 1167;  // ERROR_DEVICE_NOT_CONNECTED
  static const bool off = [] {
    const char* e = std::getenv("DASH_XAM_INPUT_PATCH");  // 0 : correctif désactivé (diagnostic)
    return e && e[0] == '0';
  }();
  static const bool merge_off = [] {
    const char* e = std::getenv("DASH_XAM_INPUT_MERGE");  // 0 : seule la manette du port interrogé (diagnostic)
    return e && e[0] == '0';
  }();
  if (off || port > 3 || !(flags & 0x80000000u) || !out) return;
  if (ctx.r3.u32 == kNotConnected) {
    // Même identifiant que common/src/kernel_xinputd.cpp (kDeviceId + emplacement = port) ; XInputdReadState(id, &paquet, &état).
    PPCContext c = ctx;
    c.r3.u64 = 0x10000001u + port;
    c.r4.u64 = out;
    c.r5.u64 = out + 4;
    __imp__XInputdReadState(c, base);
    if (c.r3.u32 == 0) ctx.r3.u64 = 0;
  }
  if (merge_off || ctx.r3.u32 != 0) return;
  // Les autres manettes annoncées : lues dans un tampon sous la pile invitée (16 octets, jamais lus par l'appelant).
  const uint32_t mask = dash_xinputd_pad_mask();
  const uint32_t scratch = (ctx.r1.u32 - 0x40u) & ~15u;
  for (uint32_t q = 0; q < 4; ++q) {
    if (q == port || !(mask >> q & 1)) continue;
    PPCContext c = ctx;
    c.r3.u64 = 0x10000001u + q;
    c.r4.u64 = scratch;
    c.r5.u64 = scratch + 4;
    __imp__XInputdReadState(c, base);
    if (c.r3.u32 != 0) continue;
    MergeGamepad(*reinterpret_cast<rex::input::X_INPUT_GAMEPAD*>(base + out + 4),
                 *reinterpret_cast<const rex::input::X_INPUT_GAMEPAD*>(base + scratch + 4));
  }
}
