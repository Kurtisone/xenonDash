// dash - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <cstring>

#include "dash_input.h"

namespace dash::profile {
void ApplyProfileSettings();
}
#include "null_audio.h"
#include <rex/rex_app.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/logging.h>
#include <rex/runtime.h>

class DashApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<DashApp>(new DashApp(ctx, "dash",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // DASH_AUDIO=nop : backend audio silencieux (machine sans carte son, voir null_audio.h). Sans moteur audio,
  // le Dashboard désenregistre la classe XUI XuiSoundXAudio et ses scènes ne chargent plus.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    // Sans émulation GPU, rien ne s'affiche : xenos par défaut (--gpu_plugin=... reste prioritaire).
    if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
    // Clavier → manette avec une disposition adaptée au Dashboard (voir dash_input.h).
    dash::input::ApplyDashKeyboardLayout();
    const char* audio = std::getenv("DASH_AUDIO");
    if (audio && std::strcmp(audio, "nop") == 0) {
      config.audio_factory = REX_AUDIO_BACKEND(dash::audio::NullAudioSystem);
    }
  }
  // Le Dashboard n'est pas un jeu : son exécutable s'appelle dash.xex.
  void OnLoadXexImage(std::string& xex_image) override {
    xex_image = "game:\\dash.xex";
  }
  // void OnPostLoadXexImage() override {}
  // La flash de la console (\SystemRoot, alias media:/dashmedia:) : polices .xtt,
  // shrdres.xzp, xam.xex... Montée en lecture seule depuis <game_data_root>/flash,
  // à remplir avec le contenu extrait du dump NAND.
  void OnPostSetup() override {
    dash::profile::ApplyProfileSettings();
    auto flash = game_data_root() / "flash";
    if (!std::filesystem::is_directory(flash)) {
      REXLOG_WARN("[dash] {} absent : la flash (\\SystemRoot) ne sera pas montée", flash.string());
      return;
    }
    auto* vfs = runtime()->file_system();
    for (const char* mount : {"\\SystemRoot", "\\Device\\Flash"}) {
      auto dev = std::make_unique<rex::filesystem::HostPathDevice>(mount, flash, true);
      if (dev->Initialize() && vfs->RegisterDevice(std::move(dev))) {
        REXLOG_INFO("[dash] {} monté sur {}", mount, flash.string());
      }
    }
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
