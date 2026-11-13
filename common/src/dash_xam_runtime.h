// xenondash common - périphériques attendus par XAM recompilé (module chargé par le Dashboard, voir README,
// « Dashboard 2.0.17559 et XAM recompilé »).
//
// Le point d'entrée de XAM (DLL_PROCESS_ATTACH) et ses threads système ouvrent des chemins du noyau : \SystemRoot,
// \Device\Flash (traces et diagnostics), \Device\MU0, \Device\Harddisk0\Partition1. Sans eux, les objets de XAM ne sont
// jamais créés (singletons nuls, puis déréférencement nul dans RtlEnterCriticalSection).

#pragma once

#include <filesystem>
#include <functional>

namespace rex::filesystem {
class VirtualFileSystem;
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

}  // namespace dash::xamrt
