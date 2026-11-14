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

// Exécute l'export `name` de XAM recompilé avec le contexte `ctx`. Faux (rien d'exécuté) si XAM n'est pas chargé ou n'a pas cet export.
bool CallXamExport(const char* name, PPCContext& ctx, uint8_t* base);


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
