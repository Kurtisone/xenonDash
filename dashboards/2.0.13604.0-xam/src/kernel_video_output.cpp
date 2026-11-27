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
//
// Changement de résolution (Paramètres > Console Settings > Display > Screen Resolution, mesure du 2026-11-27) : le Dashboard
// (sub_9225EC80) appelle VdSetDisplayModeOverride(f1 = fréquence, r3 = largeur, r4 = hauteur) ; un retour nul signifie « mode refusé » et
// il remet aussitôt les réglages (XConfig 3/0x15 et 3/0x0A, vus dans le journal) sans afficher la confirmation : choisir 1080 « ne faisait
// rien » (le SDK renvoie 0). Sur la console le noyau applique le mode au contrôleur d'affichage (retour non nul), le Dashboard écrit alors
// XConfig 3/0x15, pose la question « voyez-vous l'image ? » (10 s) et garde le mode si on répond Oui ; Non ou le délai : il appelle
// VdSetDisplayModeOverride(0, 0, 0) et remet les réglages précédents. Ici : le mode annoncé (VdQueryVideoMode, VdQueryRealVideoMode,
// VdQueryVideoFlags, donc XGetVideoMode de XAM et la ligne « Current Setting » du Dashboard) devient celui demandé, retour non nul ;
// (0, 0) revient au mode configuré du SDK. Le rendu natif ne change pas : le Dashboard dessine toujours dans son tampon de 1280x720
// (le matériel le met à l'échelle) et la fenêtre hôte reste telle quelle.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "kernel_sdk_fn.h"

static void QueryMode(PPCContext& __restrict ctx, uint8_t* base);

namespace {
// Mode demandé par VdSetDisplayModeOverride ; width == 0 : aucun (mode configuré du SDK).
struct ModeOverride {
  std::atomic<uint32_t> width{0}, height{0};
  std::atomic<float> refresh{0.f};
};
ModeOverride g_override;

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
  QueryMode(ctx, base);
}

// X_VIDEO_MODE (48 octets, grand-boutiste) : le mode configuré du SDK, remplacé par le mode demandé s'il y en a un.
static void QueryMode(PPCContext& __restrict ctx, uint8_t* base) {
  static PPCFunc* sdk_mode = dash::SdkFunction("__imp__VdQueryVideoMode");
  const uint32_t p = ctx.r3.u32;
  sdk_mode(ctx, base);
  const uint32_t w = g_override.width.load(), h = g_override.height.load();
  if (!Enabled() || !p || !w) return;
  auto put32 = [&](uint32_t off, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(base + p + off, &v, 4);
  };
  put32(0, w);
  put32(4, h);
  put32(8, 0);                                  // entrelacé
  put32(12, w * 3 >= h * 4 ? 1 : 0);            // écran large
  put32(16, (w >= 1280 || h >= 720) ? 1 : 0);   // haute définition
  float hz = g_override.refresh.load();
  if (hz <= 0.f) hz = 60.f;
  uint32_t bits;
  std::memcpy(&bits, &hz, 4);
  put32(20, bits);
}

REX_HOOK_RAW(__imp__VdQueryVideoMode) { QueryMode(ctx, base); }

REX_HOOK_RAW(__imp__VdQueryVideoFlags) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdQueryVideoFlags");
  const uint32_t w = g_override.width.load();
  if (!Enabled() || !w) {
    sdk(ctx, base);
    return;
  }
  const uint32_t h = g_override.height.load();
  ctx.r3.u64 = (w * 3 >= h * 4 ? 1u : 0u) | (w >= 1024 ? 2u : 0u) | (w >= 1920 ? 4u : 0u);
}

REX_HOOK_RAW(__imp__VdSetDisplayModeOverride) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__VdSetDisplayModeOverride");
  if (!Enabled()) {
    sdk(ctx, base);
    return;
  }
  const uint32_t w = ctx.r3.u32, h = ctx.r4.u32;
  const float hz = static_cast<float>(ctx.f1.f64);
  if (w == 0 && h == 0) {
    if (g_override.width.exchange(0))
      REXLOG_INFO("[vd] VdSetDisplayModeOverride(0, 0) : retour au mode configuré (lr {:08X})", static_cast<uint32_t>(ctx.lr));
    g_override.height.store(0);
    ctx.r3.u64 = 1;
    return;
  }
  if (w < 320 || w > 7680 || h < 200 || h > 4320) {
    REXLOG_WARN("[vd] VdSetDisplayModeOverride({}x{}) refusé : hors limites", w, h);
    ctx.r3.u64 = 0;
    return;
  }
  g_override.height.store(h);
  g_override.refresh.store(hz);
  g_override.width.store(w);
  REXLOG_INFO("[vd] VdSetDisplayModeOverride({}x{}, {:.1f} Hz) : mode annoncé (lr {:08X})", w, h, hz, static_cast<uint32_t>(ctx.lr));
  ctx.r3.u64 = 1;
}
