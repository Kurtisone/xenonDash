// dash - état de connexion (« sign-in ») et compte du profil local, pour les Dashboards NXE et Metro.
//
// Le SDK laisse en stub (r3 inchangé = valeur de retour aléatoire) la plupart des fonctions XamUser* /
// XamProfile* que le hub NXE/Metro interroge pour décider si un profil est connecté (tuile « Sign In /
// 1 Profile Found » sinon). Ici : un profil local, hors ligne (connecté localement, sans Xbox LIVE),
// cohérent avec XamUserGetSigninState (1 = connecté localement) du SDK.
//
// Les signatures viennent de la lecture du code recompilé du Dashboard (voir docs/technique.md, « Profil joueur »).

#include <algorithm>
#include <cstring>

#include <rex/hook.h>

#include "dash_xam_redirect.h"
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/types.h>

namespace dash::profile {
uint64_t LocalXuid();
bool FillLocalAccount(uint8_t* account);
}  // namespace dash::profile

namespace {
constexpr uint32_t kXENoSuchUser = 0x80070525;  // HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER)
constexpr uint32_t kErrorNotFound = 0x490;

inline void Zero(uint8_t* base, uint32_t ptr, size_t n) {
  if (ptr) std::memset(base + ptr, 0, n);
}
inline void Put32(uint8_t* base, uint32_t ptr, uint32_t v) {
  if (ptr) *reinterpret_cast<uint32_t*>(base + ptr) = __builtin_bswap32(v);
}
inline bool IsLocalUser(uint32_t index) { return index == 0; }
}  // namespace

// XamUserGetUserTenure(utilisateur, &a(8), &b(4), &c(8), overlapped) : ancienneté du compte Xbox LIVE.
// Profil hors ligne : zéros, succès (le hub le sondait toutes les 5 s et traitait l'échec comme « pas de profil »).
REX_HOOK_RAW(__imp__XamUserGetUserTenure) { DASH_XAM_FWD(XamUserGetUserTenure);
  Zero(base, ctx.r4.u32, 8);
  Put32(base, ctx.r5.u32, 0);
  Zero(base, ctx.r6.u32, 8);
  ctx.r3.u64 = IsLocalUser(ctx.r3.u32) ? 0 : kXENoSuchUser;
}

// XamUserGetSubscriptionType(utilisateur, &octet, &dword, overlapped) : abonnement (0 = aucun, hors ligne).
REX_HOOK_RAW(__imp__XamUserGetSubscriptionType) { DASH_XAM_FWD(XamUserGetSubscriptionType);
  Zero(base, ctx.r4.u32, 1);
  Put32(base, ctx.r5.u32, 0);
  ctx.r3.u64 = IsLocalUser(ctx.r3.u32) ? 0 : kXENoSuchUser;
}

// XamProfileFindAccount(xuid, &compte(0x17C), 0) : 0 = trouvé (le Dashboard lit xuid_online à +0x28 : 0 = hors ligne).
REX_HOOK_RAW(__imp__XamProfileFindAccount) { DASH_XAM_FWD(XamProfileFindAccount);
  uint64_t xuid = ctx.r3.u64;
  if (!xuid || xuid != dash::profile::LocalXuid() || !ctx.r4.u32) {
    ctx.r3.u64 = kErrorNotFound;
    return;
  }
  dash::profile::FillLocalAccount(base + ctx.r4.u32);
  ctx.r3.u64 = 0;
}

// XamUserGetIndexFromXUID(xuid, indicateurs, &indice) : >= 0 = succès.
REX_HOOK_RAW(__imp__XamUserGetIndexFromXUID) { DASH_XAM_FWD(XamUserGetIndexFromXUID);
  uint64_t xuid = ctx.r3.u64;
  if (xuid && xuid == dash::profile::LocalXuid() && ctx.r5.u32) {
    Put32(base, ctx.r5.u32, 0);
    ctx.r3.u64 = 0;
  } else {
    ctx.r3.u64 = kXENoSuchUser;
  }
}

// Profil hors ligne : pas de niveau d'abonnement, pas de pays/langue Live, adulte, sans contrôle parental.
REX_HOOK_RAW(__imp__XamUserGetMembershipTierFromXUID) { DASH_XAM_FWD(XamUserGetMembershipTierFromXUID); (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamUserGetOnlineCountryFromXUID) { DASH_XAM_FWD(XamUserGetOnlineCountryFromXUID); (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamUserGetOnlineLanguageFromXUID) { DASH_XAM_FWD(XamUserGetOnlineLanguageFromXUID); (void)base; ctx.r3.u64 = 1; }
REX_HOOK_RAW(__imp__XamUserGetAge) { DASH_XAM_FWD(XamUserGetAge); (void)base; ctx.r3.u64 = 25; }
REX_HOOK_RAW(__imp__XamUserGetAgeGroup) { DASH_XAM_FWD(XamUserGetAgeGroup); (void)base; ctx.r3.u64 = 3; }  // adulte
REX_HOOK_RAW(__imp__XamUserIsParentalControlled) { DASH_XAM_FWD(XamUserIsParentalControlled); (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamUserIsPartial) { DASH_XAM_FWD(XamUserIsPartial); (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamUserIsUnsafeProgrammingAllowed) { DASH_XAM_FWD(XamUserIsUnsafeProgrammingAllowed); (void)base; ctx.r3.u64 = 0; }
REX_HOOK_RAW(__imp__XamUserFlushLogonQueue) { DASH_XAM_FWD(XamUserFlushLogonQueue); (void)base; ctx.r3.u64 = 0; }
