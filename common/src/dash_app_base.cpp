// xenondash common - logique applicative partagée (voir dash_app_base.h).

#include "dash_app_base.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <rex/cvar.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xex_module.h>
#include <rex/system/xthread.h>
#include <string_view>
#include <rex/system/user_module.h>
#include <rex/ui/imgui_drawer.h>

#include "dash_input.h"
#include "dash_launch.h"
#include "dash_xam_redirect.h"
#include "dash_xam_runtime.h"
#include "null_audio.h"

namespace dash::profile {
void ApplyProfileSettings();
}
namespace dash::disc {
void Init();
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
  // XAM recompilé écrit ses paquets de contenu (profils : Content\<XUID>\FFFE07D1\00010000\...) sur
  // \Device\Harddisk0\Partition1, c'est-à-dire dans <game_data_root> : la racine du jeu doit être inscriptible.
  if (std::getenv("DASH_XAM_MODULE")) REXCVAR_SET(allow_game_relative_writes, true);
  const char* audio = std::getenv("DASH_AUDIO");
  if (audio && std::strcmp(audio, "nop") == 0) {
    config.audio_factory = REX_AUDIO_BACKEND(dash::audio::NullAudioSystem);
  }
}

// Le Dashboard n'est pas un jeu : son exécutable s'appelle dash.xex (ou autre selon la version).
void DashAppBase::OnLoadXexImage(std::string& xex_image) {
  xex_image = "game:\\" + xex_name_;
}

namespace {
// Shaders SPIR-V du rendu natif (native_gpu/) : <game_data_root>/shaders/{dash,hud} par défaut. DASH_NGPU_SHADERS et
// DASH_HUD_SHADERS, si elles sont définies, restent prioritaires.
void SetDefaultShaderDirs(const std::filesystem::path& data_root) {
  const std::pair<const char*, const char*> vars[] = {{"DASH_NGPU_SHADERS", "dash"}, {"DASH_HUD_SHADERS", "hud"}};
  for (const auto& [var, sub] : vars) {
    const char* e = std::getenv(var);
    if (e && *e) continue;
    const std::string dir = (data_root / "shaders" / sub).string();
#ifdef _WIN32
    _putenv_s(var, dir.c_str());
#else
    setenv(var, dir.c_str(), 1);
#endif
  }
}
}  // namespace

// La flash de la console (\SystemRoot, alias media:/dashmedia:) : polices .xtt, shrdres.xzp, xam.xex...
// Montée en lecture seule depuis <game_data_root>/flash, à remplir avec le contenu extrait du dump NAND.
void DashAppBase::OnPostSetup() {
  SetDefaultShaderDirs(game_data_root());
  // Dashboards NXE (17559) : les ressources XEX (dashlua...) sont des pages « lecture seule » de l'image, mais le
  // Dashboard y écrit (sur console ces pages sont accessibles en écriture) : tout l'exécutable passe en lecture/écriture
  // (le code n'est pas concerné : il est recompilé).
  if (auto mod = runtime()->kernel_state()->GetExecutableModule()) {
    if (auto* xex = mod->xex_module()) {
      uint32_t base = xex->base_address(), size = xex->image_size();
      if (auto* heap = runtime()->memory()->LookupHeap(base)) {
        heap->Protect(base, size, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
      }
    }
  }
  // Réglages du profil servis par le HLE (carte joueur : Gamerscore, zone, réputation). En mode profil XAM (DASH_PROFILE_MODE=xam,
  // palier N4) ils restent servis ainsi : XamUserReadProfileSettings(Ex) de XAM recompilé, appelé par le Dashboard, planterait
  // (lecture invitée de 0x7008004F : le Dashboard prend un identifiant de réglage pour un pointeur, résultat asynchrone non attendu).
  dash::profile::ApplyProfileSettings();
  dash::launch::Init();
  dash::disc::Init();
  // Interfaces XAM remplacées par ImGui (dash_xam_ui.cpp) : entrées du Dashboard suspendues
  // tant qu'elles sont affichées.
  dash::xamui::Init(&app_context(), imgui_drawer(), immediate_drawer());
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      input && imgui_drawer()) {
    auto* drawer = imgui_drawer();
    input->SetActiveCallback(
        [drawer] { return !dash::xamui::IsOpen() && !drawer->GetIO().WantCaptureMouse; });
  }
  // La flash (\SystemRoot, \Device\Flash) est montée avant XAM : son point d'entrée lit ces chemins.
  dash::xamrt::MountFlash(runtime()->file_system(), game_data_root() / "flash");
  // P4 (prototype) : XAM recompilé comme module ReXGlue (voir README, « XAM recompilé »). Actif seulement si le projet
  // déclare le module (`[[modules]]` dans dash_manifest.toml) ET si DASH_XAM_MODULE est défini.
  if (std::getenv("DASH_XAM_MODULE")) {
    auto* ks = runtime()->kernel_state();
    if (ks->FindRecompiledModule("xam.xex")) {
      dash::xamrt::MountXamDevices(runtime()->file_system());
      dash::xamrt::FixKernelVariables(ks, runtime()->export_resolver());
      // DASH_XAM_MODULE=1 : chargement seul ; =2 : + point d'entrée (DllMain/DLL_PROCESS_ATTACH) sur un thread invité.
      const bool run_entry = std::string_view(std::getenv("DASH_XAM_MODULE")) == "2";
      auto* resolver = runtime()->export_resolver();
      auto load = [ks, resolver, run_entry]() {
        auto xam = ks->LoadUserModule("game:\\xam.xex", run_entry);
        REXLOG_INFO("[dash] module xam.xex recompilé : {}", xam ? "chargé" : "ÉCHEC");
        if (xam) dash::xamrt::SetXamImageRange(xam->xex_module()->base_address(), xam->xex_module()->image_size());
        // Palier E : redirection des imports XAM du Dashboard (DASH_XAM_REDIRECT), voir dash_xam_redirect.h.
        dash::xamredir::Configure(ks, resolver, xam.get());
        return 0;
      };
      if (run_entry) {
        dash::xamrt::RunInSystemProcess(ks, load);
      } else {
        load();
      }
    } else {
      REXLOG_WARN("[dash] DASH_XAM_MODULE défini mais le projet ne déclare pas de module xam.xex");
    }
  }
}

}  // namespace xenondash
