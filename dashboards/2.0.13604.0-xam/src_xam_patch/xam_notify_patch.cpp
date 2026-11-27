// Notifications de XAM recompilé destinées au Dashboard.
//
// Le Dashboard écoute les notifications du HLE du SDK (XNotifyGetNext HLE), pas celles de XAM recompilé (deux files distinctes) ;
// seules XN_SYS_SIGNINCHANGED et PROFILESETTINGCHANGED sont déjà rediffusées (common/src/dash_xam_redirect.cpp).
//
// Mesure du 2026-11-27 (« les liens vers Game, Video, Music Library ne fonctionnent pas », Guide > Media/Games) : XAM traite le choix
// d'une bibliothèque par XamLoaderRebootToDash (sub_8187A970), qui, le Dashboard étant déjà le titre courant, ne relance rien : il
// diffuse XNotifyBroadcast(0x80040016, utilisateur << 16 | paramètre) (id système, version 4, index 0x16 ; paramètre 0x2F pour la
// bibliothèque de jeux) pour que le Dashboard ouvre lui-même la page. Cette notification restait dans la file de XAM : le Dashboard
// ne la voyait jamais, le Guide se fermait et rien ne s'ouvrait. On la rediffuse vers le HLE avant l'original.
#include <cstdint>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

extern "C" void __imp__sub_8187E240(PPCContext& __restrict, uint8_t*);

// XNotifyBroadcast(r3 = identifiant, r4 = paramètre) de XAM.
extern "C" void sub_8187E240(PPCContext& __restrict ctx, uint8_t* base) {
  constexpr uint32_t kDashNavigate = 0x80040016u;
  const uint32_t id = ctx.r3.u32, param = ctx.r4.u32;
  if (id == kDashNavigate) {
    REXLOG_INFO("[notify] XNotifyBroadcast {:08X} paramètre {:08X} rediffusée vers le HLE du Dashboard", id, param);
    REX_KERNEL_STATE()->BroadcastNotification(rex::system::XNotificationID(id), param);
  }
  __imp__sub_8187E240(ctx, base);
}

// Journal ciblé (toasts) : XNotifyQueueUI de XAM (sub_81880470) ; r3 = identifiant du type de notification, r4 = utilisateurs.
// Une ligne par toast demandé : si un toast (« signed out », succès...) manque à l'écran, elle dit si XAM l'a demandé.
extern "C" void __imp__sub_81880470(PPCContext& __restrict, uint8_t*);
extern "C" void sub_81880470(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t type = ctx.r3.u32, users = ctx.r4.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_81880470(ctx, base);
  REXLOG_INFO("[notify] XNotifyQueueUI type {} utilisateurs {:08X} (lr {:08X}) -> {:08X}", type, users, lr, ctx.r3.u32);
}
