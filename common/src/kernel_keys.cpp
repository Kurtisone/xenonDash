// xenondash common - clés de la console (XeKeysGetKey, XeKeysExGetKey) pour XAM recompilé.
//
// Le SDK les laisse en stubs (r3 inchangé, tampon non écrit). XAM (sub_81A64680, clé 0x100 puis sub_81A63580) lit une clé
// RSA de la console et calcule des paramètres de grands nombres : avec un tampon nul, la division de sub_81A71FD8
// tourne sans fin (le démarrage de l'application 0xFA de XAM ne revient jamais, le fil d'initialisation reste bloqué).
// Ici : le tampon est rempli d'octets pseudo-aléatoires fixes (non nuls, dernier octet impair), taille inchangée,
// statut de succès. Ce ne sont pas de vraies clés : tout contrôle de signature ou de licence qui en dépend échouera.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

#include "dash_xam_runtime.h"
#include "kernel_sdk_fn.h"

namespace {

// XeKeysGetKey(id, tampon, *taille) / XeKeysExGetKey(id, tampon, *taille) : mêmes trois premiers arguments.
void FillKey(PPCContext& ctx, uint8_t* base, const char* name) {
  const uint32_t id = ctx.r3.u32, buf = ctx.r4.u32, size_ptr = ctx.r5.u32;
  uint32_t size = size_ptr ? __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + size_ptr)) : 0;
  if (size > 0x10000) size = 0x10000;
  uint32_t x = 0x9E3779B9u ^ id;
  for (uint32_t i = 0; buf && i < size; ++i) {
    x = x * 1664525u + 1013904223u;
    base[buf + i] = static_cast<uint8_t>((x >> 24) | 1u);
  }
  REXLOG_INFO("[kernel] {}(0x{:X}) : clé factice de {} octets", name, id, size);
  ctx.r3.u64 = 0;
}

}  // namespace

// Seuls les appels venant du code de XAM recompilé (adresse de retour dans son image) reçoivent la clé factice ; les
// autres appelants gardent le comportement du SDK.
REX_HOOK_RAW(__imp__XeKeysGetKey) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__XeKeysGetKey");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr))) FillKey(ctx, base, "XeKeysGetKey");
  else sdk(ctx, base);
}
REX_HOOK_RAW(__imp__XeKeysExGetKey) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__XeKeysExGetKey");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr))) FillKey(ctx, base, "XeKeysExGetKey");
  else sdk(ctx, base);
}
