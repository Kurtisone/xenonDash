// Sortie vidéo de la console (Dashboard Kinect 13604, variante -xam) : prise AV, découverte de l'affichage, mode réel.
//
// Le SDK laisse HalGetCurrentAVPack, VdGetDisplayDiscoveryData et VdQueryRealVideoMode en stubs : r3 inchangé, aucune
// sortie écrite. La liste des résolutions des Paramètres du Dashboard en dépend :
//  - XGetAVPack de XAM (sub_818CAD20) n'est qu'un saut vers HalGetCurrentAVPack : il renvoyait le r3 laissé par son
//    appelant. sub_9225E410 et sub_9225D640 (dash.xex) choisissent la liste selon la prise : 3/4/6/8 ont une liste de
//    modes, et 4/6/8 la liste construite d'après la découverte de l'affichage (tête 0x92953B48) ;
//  - sub_9225D778 passe 35 octets (+0 = taille) à VdGetDisplayDiscoveryData ; si r3 est non nul, il lit le type de liaison
//    (u32 en +29) et des indicateurs (+4) : 1 -> type 4, les 10 modes PC (table 0x9201D330) ; 2 ou 3 -> types 0/2, les
//    4 modes HDTV (table 0x9201D2D4) si +4 & 0x100, sinon types 1/3 sans liste ; toute autre valeur garde le type
//    précédent (5 au départ : aucune liste) ;
//  - sub_9225D640 cherche le mode réel (VdQueryRealVideoMode : X_VIDEO_MODE, largeur +0, hauteur +4, entrelacé +8) dans la
//    liste pour désigner l'entrée courante.
// Avec les stubs, ces choix lisaient des résidus de registres et de pile invités : la liste apparaissait ou non selon les
// appels précédents (elle a disparu quand les fibres de XAM ont reçu leur propre pile hôte). Ici, des sorties déterministes,
// cohérentes avec le XGetAVPack du SDK (6 = VGA) : prise VGA, liaison VGA (+29 = 1, aucun indicateur, taille physique de
// l'écran inconnue : +21/+22 = 0, que les appelants ignorent), mode réel = mode configuré (VdQueryVideoMode).
// VdReadEEDIDBlock et VdEnumerateVideoModes gardent leur stub : le Dashboard remet son tampon EDID à zéro avant l'appel
// (bloc 0 : r3 = 0, échec), et avec la liaison VGA (type 4) le rappel d'énumération (sub_9225CB30) renvoie 0 d'emblée
// sans rien ajouter : ne pas l'appeler revient au même.
// DASH_VIDEO_DISCOVERY=0 rétablit les stubs du SDK (comparaison).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "kernel_sdk_fn.h"

extern "C" void __imp__VdQueryVideoMode(PPCContext& __restrict, uint8_t*);

namespace {
constexpr uint32_t kAvPackVga = 6;          // valeur du XGetAVPack du SDK (variante sans XAM)
constexpr uint32_t kDiscoverySize = 35;     // taille passée par tous les appelants (dash.xex et XAM)
constexpr uint32_t kLinkOffset = 29;        // u32 grand-boutiste non aligné : type de liaison
constexpr uint32_t kLinkVga = 1;

bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("DASH_VIDEO_DISCOVERY");
    return !(e && *e == '0');
  }();
  return on;
}
}  // namespace

REX_HOOK_RAW(__imp__HalGetCurrentAVPack) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__HalGetCurrentAVPack");
  if (!Enabled()) {
    sdk(ctx, base);
    return;
  }
  ctx.r3.u64 = kAvPackVga;
}

REX_HOOK_RAW(__imp__VdGetDisplayDiscoveryData) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdGetDisplayDiscoveryData");
  if (!Enabled()) {
    sdk(ctx, base);
    return;
  }
  const uint32_t p = ctx.r3.u32;
  uint32_t size = 0;
  if (p) {
    std::memcpy(&size, base + p, 4);
    size = __builtin_bswap32(size);
  }
  static std::atomic<bool> logged{false};
  if (!p || size < kDiscoverySize) {  // aucun appelant connu ne le fait : échec, sans rien écrire
    REXLOG_WARN("[vd] VdGetDisplayDiscoveryData({:08X}) taille {} refusée (lr {:08X})", p, size, static_cast<uint32_t>(ctx.lr));
    ctx.r3.u64 = 0;
    return;
  }
  uint8_t* d = base + p;
  std::memset(d + 4, 0, kDiscoverySize - 4);  // jamais au-delà des 35 octets : le reste appartient à l'appelant
  const uint32_t link = __builtin_bswap32(kLinkVga);
  std::memcpy(d + kLinkOffset, &link, 4);
  if (!logged.exchange(true))
    REXLOG_INFO("[vd] découverte de l'affichage : liaison VGA (+29 = {}), prise AV {} (lr {:08X})", kLinkVga, kAvPackVga,
                static_cast<uint32_t>(ctx.lr));
  ctx.r3.u64 = 1;
}

REX_HOOK_RAW(__imp__VdQueryRealVideoMode) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdQueryRealVideoMode");
  if (!Enabled() || !ctx.r3.u32) {
    sdk(ctx, base);
    return;
  }
  __imp__VdQueryVideoMode(ctx, base);
}
