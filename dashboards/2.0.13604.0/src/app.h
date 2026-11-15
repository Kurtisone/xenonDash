// dash 2.0.13604.0 - application de cette version : tout le comportement est dans xenondash::DashAppBase.

#pragma once

#include <cstdlib>

#include "dash_app_base.h"
#include "trace_refcount.h"

class DashApp : public xenondash::DashAppBase {
 public:
  using xenondash::DashAppBase::DashAppBase;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DashApp>(new DashApp(ctx, "dash", PPCImageConfig, "dash.xex"));
  }

  // Instrumentation temporaire du compteur de references (DASH_TRACE_REFCOUNT=1) : voir
  // diagnostic_gel_13604.md. A retirer une fois le plantage identifie.
  void OnPostSetup() override {
    xenondash::DashAppBase::OnPostSetup();
    dash::trace::ArmIfRequested();
  }

#ifdef DASH_NATIVE_GPU
  // Rendu natif (native_gpu/) : pas de plugin GPU Xenos ; la fenêtre et la présentation sont gérées par native_gpu.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    xenondash::DashAppBase::OnPreSetup(config);
    if (!std::getenv("DASH_NGPU_DISABLE")) config.gpu_plugin.clear();  // DASH_NGPU_DISABLE=1 (+ --gpu_plugin=xenos) : émulation de référence
  }
#endif
};
