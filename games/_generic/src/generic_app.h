// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - application minimale d'un jeu recompile en mode generique : aucun crochet propre au jeu, rendu natif par ngpu_core
// (aucun plugin GPU), chemins portables comme les recettes (jeu a cote de l'executable ou game_data_root absolu).
#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/rex_app.h>

#include "xd_front.h"

class GenericApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GenericApp>(new GenericApp(ctx, "xdgame", xdgame_PPCImageConfig));
  }

  // Rendu natif : aucun plugin GPU (zero emulation) ; le D3D invite est remplace par les crochets de la facade.
  void OnPreSetup(rex::RuntimeConfig& config) override { config.gpu_plugin.clear(); }
  void OnPostSetup() override { XdFrontStart(runtime()); }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetAppRootFolder();
    if (paths.game_data_root.empty()) paths.game_data_root = exe_dir / "game";
    if (paths.game_data_root.is_relative()) paths.game_data_root = exe_dir / paths.game_data_root;
    if (std::filesystem::exists(exe_dir / "portable.txt")) {
      paths.user_data_root = exe_dir / "userdata";
      paths.cache_root = paths.user_data_root / "cache";
    }
  }
};
