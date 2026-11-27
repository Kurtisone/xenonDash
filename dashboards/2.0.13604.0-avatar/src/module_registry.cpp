// Modules recompilés du Dashboard (xam, hud, XUI), partagés avec l'exécutable dash : leurs bibliothèques libdash_<nom>.so sont
// construites par le projet dashboards/2.0.13604.0-xam et posées à côté de cet exécutable (même fichier que
// dashboards/2.0.13604.0-xam/generated/dash/module_registry.cpp, produit par le codegen de dash).
#include <rex/system/kernel_state.h>

void RegisterRecompiledModules(rex::system::KernelState* kernel_state) {
  kernel_state->RegisterRecompiledModule("xam.xex", "xam.xex", "dash_xam");
  kernel_state->RegisterRecompiledModule("hud.xex", "flash/hud.xex", "dash_hud");
  kernel_state->RegisterRecompiledModule("vk.xex", "flash/vk.xex", "dash_vk");
  kernel_state->RegisterRecompiledModule("signin.xex", "flash/signin.xex", "dash_signin");
  kernel_state->RegisterRecompiledModule("createprofile.xex", "flash/createprofile.xex", "dash_createprofile");
  kernel_state->RegisterRecompiledModule("gamerprofile.xex", "flash/gamerprofile.xex", "dash_gamerprofile");
  kernel_state->RegisterRecompiledModule("deviceselector.xex", "flash/deviceselector.xex", "dash_deviceselector");
  kernel_state->RegisterRecompiledModule("ximecore.xex", "flash/ximecore.xex", "dash_ximecore");
  kernel_state->RegisterRecompiledModule("guide.avatarminicreator.xex", "sep/20352400/guide.avatarminicreator.xex",
                                         "dash_Guide_AvatarMiniCreator");
}
