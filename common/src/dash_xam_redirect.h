// xenondash common - redirection des imports XAM du Dashboard vers XAM recompilé (palier E, docs/xam_recompile.md).
//
// Le Dashboard appelle ses imports XAM par `__imp__<Nom>(ctx, base)`. Ces symboles sont, par défaut, le HLE du SDK
// (src/kernel/xam). Chaque import XAM a ici un hook dans l'exécutable : si son nom figure dans DASH_XAM_REDIRECT, le
// hook exécute l'export de même nom de XAM recompilé (module chargé, résolu par ordinal) ; sinon il appelle le HLE.
//
// Usage :
//   DASH_XAM_MODULE=2 DASH_XAM_REDIRECT=XGetLanguage,XamUserGetSigninState ./dash ...
//   DASH_TEST_MSGBOX=<secondes> : après ce délai, appelle XamShowMessageBoxUI depuis un thread titre (test).

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

namespace rex::system {
class KernelState;
class UserModule;
}  // namespace rex::system
namespace rex::runtime {
class ExportResolver;
}

namespace dash::xamredir {

// Appelée après le chargement de XAM recompilé. Lit DASH_XAM_REDIRECT et résout chaque nom vers l'export du module.
void Configure(rex::system::KernelState* ks, rex::runtime::ExportResolver* resolver, rex::system::UserModule* xam);

// Palier G : imports `xam.xex` d'un module recompilé (hud.xex...). Le module définit, pour chaque import XAM, un
// `__imp__<Nom>` masqué (dashboards/<projet>/src/hud_xam_imports.cpp, produit par tools/gen_module_xam_imports.py) qui
// appelle CallModuleImport : l'export de même ordinal de XAM recompilé s'exécute avec le même contexte ; si XAM n'en
// a pas, repli sur le symbole visible de l'exécutable (accroche de common/ ou HLE du SDK).
struct ModuleImport {
  const char* name;
  uint16_t ordinal;
  std::atomic<PPCFunc*> fn{nullptr};  // fonction de XAM recompilé, ou du repli
  std::atomic<bool> guest{false};     // true : XAM recompilé
  std::atomic<uint64_t> calls{0};
};

void CallModuleImport(ModuleImport& imp, PPCContext& ctx, uint8_t* base);

// Palier N4 : DASH_PROFILE_MODE=xam (avec DASH_XAM_MODULE). Les profils sont alors ceux de XAM recompilé (création par la fenêtre
// « Create Profile » de createprofile.xex, connexion par signin.xex) et non le profil local du HLE (dash_profile.cpp, dash_user.cpp) :
// aucun profil n'existe au démarrage. Les accroches de profil de common/ passent alors la main à l'export de même nom de XAM.
bool ProfileModeXam();

// Vrai si une scène de XAM est empilée au-dessus du niveau de repos (Guide, boîte, signin...) : pile de scènes de l'état UI
// (0x81BFCAE8 +36) > repos. Repos = 1 au démarrage (la scène de boot de l'app 252 reste empilée, chrome du HUD) ; il tombe à 0
// dès que la pile est vue vide après avoir été non vide : XamAppUnloadStack (signin.xex, sub_901047C0) vide toute la pile,
// scène de boot comprise. Partagé par le compositeur du HUD, le portier des entrées et XamIsUIActive.
bool XamSceneAboveRest(const uint8_t* base);

// Exécute l'export `name` de XAM recompilé avec le contexte `ctx`. Faux (rien d'exécuté) si XAM n'est pas chargé ou n'a pas cet export.
bool CallXamExport(const char* name, PPCContext& ctx, uint8_t* base);

// Chaîne UTF-16 big-endian dans le tas système invité ; renvoie son adresse invitée (0 si le noyau n'est pas prêt).
uint32_t PutWide(const char16_t* s);

// Palier N5 : boîte de message Xbox du HUD de XAM (XamShowMessageBoxUI redirigé vers XAM recompilé), appelée depuis
// l'hôte — socle de dash_xam_hud_ui.cpp (progression de construction). À appeler depuis un thread invité (XHostThread) :
// la fonction invité lit le 9e argument sur la pile de l'appelant. Faux si XAM n'est pas chargé/redirigé ou si la
// requête est refusée. `wait_ms` > 0 : attend la fermeture (borne) et écrit le bouton choisi dans *choice si non nul
// (0xFFFFFFFF = aucun). Le rendu est celui du HUD natif (palier N2), par-dessus le Dashboard.
bool ShowXamMessageBox(const char16_t* title, const char16_t* text, const char16_t* const* buttons, size_t button_count,
                       uint32_t active_button, uint32_t flags, uint32_t* choice, int wait_ms);

// Guide de la console : XamShowGuideUI (ordinal 772) de XAM recompilé, appelé depuis l'hôte — c'est ce que le
// bouton Guide doit ouvrir, pas une lame dessinée par le titre. À appeler depuis un thread invité (XHostThread),
// comme ShowXamMessageBox. `wait_ms` > 0 : attend la fermeture (borne). Faux si XAM n'est pas chargé/redirigé ou si
// l'appel est refusé ; le journal dit alors lequel des deux.
bool ShowXamGuide(uint32_t user_index, int wait_ms);

// Notification Xbox (toast) : XNotifyQueueUI de XAM recompilé, non bloquante. À appeler depuis un thread invité
// (XHostThread). Faux si XAM n'est pas chargé/redirigé ou si XAM refuse (r3 != 0).
bool ShowXamToast(const char16_t* text);

// XUID (masque 3) de l'utilisateur `user` (0 à 3) connecté dans XAM recompilé, 0 si l'emplacement est vide. Appel invité : `ctx` est le
// contexte de l'appelant (sa pile sert de tampon).
uint64_t UserXuid(const PPCContext& ctx, uint8_t* base, uint32_t user);

}  // namespace dash::xamredir

// Accroche typée (comme REX_HOOK) qui, en mode profil XAM, exécute l'export de XAM recompilé de même nom.
#define DASH_XAM_FWD_HOOK(name, function)                                                               \
  extern "C" REX_FUNC(__imp__##name) {                                                                  \
    if (dash::xamredir::ProfileModeXam() && dash::xamredir::CallXamExport(#name, ctx, base)) return;   \
    rex::ppc::HostToGuestFunction<function>(ctx, base);                                                \
  }

// Idem pour une accroche brute : à placer en tête du corps.
#define DASH_XAM_FWD(name)                                                                              \
  if (dash::xamredir::ProfileModeXam() && dash::xamredir::CallXamExport(#name, ctx, base)) return
