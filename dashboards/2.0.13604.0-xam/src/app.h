// dash 2.0.13604.0 - application de cette version : tout le comportement est dans xenondash::DashAppBase.

#pragma once

#include <cstdlib>

#include "dash_app_base.h"

class DashApp : public xenondash::DashAppBase {
 public:
  using xenondash::DashAppBase::DashAppBase;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DashApp>(new DashApp(ctx, "dash", PPCImageConfig, "dash.xex"));
  }

#ifdef DASH_NATIVE_GPU
  // Rendu natif (native_gpu/) : pas de plugin GPU Xenos ; fenetre et presentation gerees par native_gpu (Dashboard et HUD de XAM).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    xenondash::DashAppBase::OnPreSetup(config);
    if (!std::getenv("DASH_NGPU_DISABLE")) config.gpu_plugin.clear();
  }
#endif
};
