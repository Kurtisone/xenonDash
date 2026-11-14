// xenondash common - périphériques attendus par XAM recompilé (module chargé par le Dashboard, voir README,
// « Dashboard 2.0.17559 et XAM recompilé »).
//
// Le point d'entrée de XAM (DLL_PROCESS_ATTACH) et ses threads système ouvrent des chemins du noyau : \SystemRoot,
// \Device\Flash (traces et diagnostics), \Device\MU0, \Device\Harddisk0\Partition1. Sans eux, les objets de XAM ne sont
// jamais créés (singletons nuls, puis déréférencement nul dans RtlEnterCriticalSection).

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

namespace rex::filesystem {
class VirtualFileSystem;
}
namespace rex::runtime {
class ExportResolver;
}
namespace rex::system {
class KernelState;
}

namespace dash::xamrt {

// Monte \SystemRoot et \Device\Flash sur le répertoire flash du projet (lecture seule). Appelée avant le chargement de
// XAM, pour tous les Dashboards.
void MountFlash(rex::filesystem::VirtualFileSystem* vfs, const std::filesystem::path& flash_dir);

// Monte \Device\Harddisk0\Partition1 et \Device\MU0 sur des répertoires d'état (lecture/écriture). Répertoire racine :
// DASH_XAM_STATE, sinon ${XDG_DATA_HOME:-~/.local/share}/xenondash/xam. Appelée seulement quand XAM est un module.
void MountXamDevices(rex::filesystem::VirtualFileSystem* vfs);

// Exécute `fn` sur un thread hôte du processus SYSTÈME (X_PROCTYPE_SYSTEM) et attend sa fin. XAM choisit son état
// d'après KeGetCurrentProcessType() (== 2) : rex::system::XHostThread le place dans le processus titre (type 1), où
// les objets de XAM ne sont jamais créés. Les threads créés par XAM (ExCreateThread, drapeau 2) sont déjà système.
void RunInSystemProcess(rex::system::KernelState* ks, std::function<int()> fn);

// Plage de l'image de XAM recompilé (adresses invitées). Le code de XAM voit KeGetCurrentProcessType() == 2 (processus
// système) quel que soit le fil appelant : XAM n'existe qu'en une instance, initialisée dans le processus système, alors
// que sur la console elle est chargée aussi dans le processus titre (son état « titre » : tas de classe 0, etc.). Les
// fils titre du Dashboard qui appellent XAM utilisent donc l'état système.
void SetXamImageRange(uint32_t base, uint32_t size);

// Vrai si `addr` (adresse invitée, ex. ctx.lr) est dans l'image de XAM recompilé ou d'un module XUI du HUD (hud, vk, signin...).
bool InXamImage(uint32_t addr);

// Corrige des variables du noyau initialisées par le SDK de façon incompatible avec XAM recompilé. Appelée avant le
// chargement de XAM. Voir dash_xam_runtime.cpp.
void FixKernelVariables(rex::system::KernelState* ks, rex::runtime::ExportResolver* resolver);

}  // namespace dash::xamrt
