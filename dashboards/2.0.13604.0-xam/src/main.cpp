// dash - ReXGlue Recompiled Project

#include <cstdlib>

#include "generated/dash/dash_init.h"

#include "app.h"

// Seule variante du Dashboard Kinect (l'ancienne variante sans XAM a été supprimée le 2026-11-26) : XAM recompilé, son HUD
// et ses profils sont toujours actifs. Sans ces deux variables, le binaire retombait sur la couche XAM émulée du SDK. Une
// valeur déjà posée reste prioritaire (DASH_XAM_MODULE=1 : module chargé sans son point d'entrée). Elles ne sont lues
// qu'après main (DashAppBase::OnPreSetup et suivants).
[[maybe_unused]] static const bool g_xam_defaults = [] {
  setenv("DASH_XAM_MODULE", "2", 0);
  setenv("DASH_PROFILE_MODE", "xam", 0);
  return true;
}();

REX_DEFINE_APP(dash, DashApp::Create)
