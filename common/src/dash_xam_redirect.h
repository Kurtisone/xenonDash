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

#include <rex/ppc/context.h>

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

}  // namespace dash::xamredir
