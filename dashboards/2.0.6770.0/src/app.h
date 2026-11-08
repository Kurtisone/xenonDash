// dash 2.0.6770.0 - application de cette version : tout le comportement est dans xenondash::DashAppBase.

#pragma once

#include <cstdlib>

#include "dash_app_base.h"

class DashApp : public xenondash::DashAppBase {
 public:
  using xenondash::DashAppBase::DashAppBase;

#ifdef DASH_NATIVE_GPU
  // Rendu natif (native_gpu/) : pas de plugin GPU Xenos ; la fenêtre et la présentation sont gérées par native_gpu.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    xenondash::DashAppBase::OnPreSetup(config);
    if (!std::getenv("DASH_NGPU_DISABLE") && !std::getenv("DASH_NGPU_OBSERVE")) config.gpu_plugin.clear();  // DASH_NGPU_DISABLE=1 ou DASH_NGPU_OBSERVE=N : émulation « xenos » (référence)
  }
#endif

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DashApp>(new DashApp(ctx, "dash", PPCImageConfig, "dash.xex"));
  }
};
