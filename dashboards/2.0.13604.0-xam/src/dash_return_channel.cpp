// Retour de l'éditeur d'avatar : le Dashboard rouvre sur My Xbox (et non Welcome).
//
// Au démarrage, le Dashboard choisit le canal de la page d'accueil dans sub_92300258 (objet « configuration de la page d'accueil »
// : liste des canaux en +0x48, canal demandé en +0x58 sous forme de pointeur vers l'identifiant large). Sans canal demandé, il prend
// WELCOME dès que le canal Welcome n'est pas masqué (RETAIL_FLAGS) ; avec un canal demandé, il prend celui-ci, puis remet +0x58 à
// zéro. L'éditeur d'avatar rend la commande 0x35 (dash::session::ReturnToMyXbox), que la chaîne de sub_9216B740 ne traite pas : rien
// ne demande donc le canal d'origine. Ce correctif demande XBOX360 (identifiant large du Dashboard, 0x9202B2E0) tant que la liste
// n'a pas fourni de canal. DASH_RETURN_CHANNEL=0 le désactive (diagnostic).

#include <cstdint>
#include <cstdlib>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "dash_session.h"

extern "C" void __imp__sub_92300258(PPCContext& __restrict, uint8_t*);

extern "C" void sub_92300258(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kXbox360Wide = 0x9202B2E0, kRequested = 0x58;
  static const bool off = [] {
    const char* e = std::getenv("DASH_RETURN_CHANNEL");
    return e && e[0] == '0';
  }();
  const bool armed = !off && dash::session::ReturnToMyXbox();
  if (armed) {
    auto* requested = reinterpret_cast<uint32_t*>(base + ctx.r3.u32 + kRequested);
    if (*requested == 0) {
      *requested = __builtin_bswap32(kXbox360Wide);
      REXLOG_INFO("[session] retour de l'éditeur : canal My Xbox demandé à la page d'accueil");
    }
  }
  __imp__sub_92300258(ctx, base);
  if (armed && ctx.r3.u32 != 0) dash::session::ReturnToMyXboxDone();
}
