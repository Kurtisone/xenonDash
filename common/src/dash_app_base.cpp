// xenondash common - logique applicative partagée (voir dash_app_base.h).

#include "dash_app_base.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>

#include "dash_input.h"
#include "null_audio.h"

namespace dash::profile {
void ApplyProfileSettings();
}
namespace dash::xamui {
void Init(rex::ui::WindowedAppContext* app, rex::ui::ImGuiDrawer* imgui, rex::ui::ImmediateDrawer* immediate);
bool IsOpen();
}  // namespace dash::xamui

namespace xenondash {

// DASH_AUDIO=nop : backend audio silencieux (machine sans carte son, voir null_audio.h). Sans moteur audio,
// le Dashboard désenregistre la classe XUI XuiSoundXAudio et ses scènes ne chargent plus.
void DashAppBase::OnPreSetup(rex::RuntimeConfig& config) {
  // Sans émulation GPU, rien ne s'affiche : xenos par défaut (--gpu_plugin=... reste prioritaire).
  if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  // Clavier → manette avec une disposition adaptée au Dashboard (voir dash_input.h).
  dash::input::ApplyDashKeyboardLayout();
  const char* audio = std::getenv("DASH_AUDIO");
  if (audio && std::strcmp(audio, "nop") == 0) {
    config.audio_factory = REX_AUDIO_BACKEND(dash::audio::NullAudioSystem);
  }
}

// Le Dashboard n'est pas un jeu : son exécutable s'appelle dash.xex (ou autre selon la version).
void DashAppBase::OnLoadXexImage(std::string& xex_image) {
  xex_image = "game:\\" + xex_name_;
}

// La flash de la console (\SystemRoot, alias media:/dashmedia:) : polices .xtt, shrdres.xzp, xam.xex...
// Montée en lecture seule depuis <game_data_root>/flash, à remplir avec le contenu extrait du dump NAND.
void DashAppBase::OnPostSetup() {
  dash::profile::ApplyProfileSettings();
  // Interfaces XAM remplacées par ImGui (dash_xam_ui.cpp) : entrées du Dashboard suspendues
  // tant qu'elles sont affichées.
  dash::xamui::Init(&app_context(), imgui_drawer(), immediate_drawer());
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      input && imgui_drawer()) {
    auto* drawer = imgui_drawer();
    input->SetActiveCallback(
        [drawer] { return !dash::xamui::IsOpen() && !drawer->GetIO().WantCaptureMouse; });
  }
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

}  // namespace xenondash
