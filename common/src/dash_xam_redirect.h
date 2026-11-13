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

#include <cstdint>

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

}  // namespace dash::xamredir
