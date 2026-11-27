// avatar 2.0.13604 - AvatarEditor.xex (titre 584D07D1) : tout le comportement est dans xenondash::DashAppBase (même runtime que dash).
#pragma once

#include <cstdlib>

#include "dash_app_base.h"

void RegisterRecompiledModules(rex::system::KernelState*);  // src/module_registry.cpp
void AvatarReplayLogon(rex::system::KernelState*);           // src/avatar_session.cpp

class AvatarApp : public xenondash::DashAppBase {
 public:
  using xenondash::DashAppBase::DashAppBase;

  // L'éditeur se lit dans la partition système étendue du dossier de données (aucune copie) : game:\SystemExtPartition\20352400\AvatarEditor.xex.
  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<AvatarApp>(
        new AvatarApp(ctx, "avatar", PPCImageConfig, "SystemExtPartition\\20352400\\AvatarEditor.xex"));
  }

  // Le codegen de l'éditeur ne déclare aucun module : le registre (xam, hud, XUI) est celui du Dashboard, enregistré avant le chargement de XAM.
  void OnPostSetup() override {
    RegisterRecompiledModules(runtime()->kernel_state());
    xenondash::DashAppBase::OnPostSetup();
    AvatarReplayLogon(runtime()->kernel_state());
  }

  // Rendu natif (native_gpu/, profil avatar13604) : pas de plugin GPU Xenos.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    xenondash::DashAppBase::OnPreSetup(config);
    if (!std::getenv("DASH_NGPU_DISABLE")) config.gpu_plugin.clear();
  }
};
