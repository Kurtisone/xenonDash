// SPDX-License-Identifier: GPL-3.0-or-later
// Repris de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), recette XenonDash 45410813.
// skate1 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/perf/counter.h>
#include <rex/rex_app.h>

#ifdef REXGLUE_ENABLE_PERF_COUNTERS
REXCVAR_DECLARE(std::string, perf_log_csv);
#endif

#ifdef XD_NATIVE_GPU
#include "xd_front.h"
#endif
#include "pause_settings.h"
#include "quit_menu.h"

class Skate1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Skate1App>(new Skate1App(ctx, "skate1",
        skate1_PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // Profiling builds (TRACY=1 / PERF=1 tools/steamdeck/build.sh): the SDK's
  // Tracy client has a manual lifetime and nothing in the SDK starts it, nor
  // opens the perf_log_csv per-frame counter log.
  void OnPostInitLogging() override {
    rex::perf::Profiler::Startup();
#ifdef REXGLUE_ENABLE_PERF_COUNTERS
    rex::perf::SetCsvLogPath(REXCVAR_GET(perf_log_csv));
#endif
  }
#ifdef XD_NATIVE_GPU
  // Rendu natif (ngpu_core) : aucun plugin GPU, le D3D invite est remplace (recette 45410813, d3d_profile.toml).
  void OnPreSetup(rex::RuntimeConfig& config) override { config.gpu_plugin.clear(); }
#endif
  // void OnLoadXexImage(std::string& xex_image) override {}
  void OnPostSetup() override {
    SetMainMenuQuitHandler([this] {
      app_context().CallInUIThreadDeferred([this] {
        if (window()) window()->RequestClose();
      });
    });
    SetPauseSettingsSource([this] { return MakeVideoSettings(); });
#ifdef XD_NATIVE_GPU
    XdFrontStart(runtime());
#endif
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  // Portable layout: a relative (or missing) game_data_root resolves next to
  // the exe instead of the working directory, and a portable.txt beside the
  // exe keeps saves and caches in the exe folder too. The app root is the
  // exe folder on desktop and the app's external files folder on Android.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto exe_dir = rex::filesystem::GetAppRootFolder();
    if (paths.game_data_root.empty()) paths.game_data_root = exe_dir / "game";
    if (paths.game_data_root.is_relative())
      paths.game_data_root = exe_dir / paths.game_data_root;
    if (std::filesystem::exists(exe_dir / "portable.txt")) {
      paths.user_data_root = exe_dir / "userdata";
      paths.cache_root = paths.user_data_root / "cache";
    }
  }
};
