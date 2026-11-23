// xenondash common - périphériques attendus par XAM recompilé (voir dash_xam_runtime.h).

#include "dash_xam_runtime.h"

#include "kernel_sdk_fn.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/export_resolver.h>
#include <rex/system/guest_path.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/system/xthread.h>

namespace dash::xamrt {

namespace fs = std::filesystem;

// Répertoire d'état des périphériques factices : DASH_XAM_STATE, sinon ${XDG_DATA_HOME:-~/.local/share}/xenondash/xam.
fs::path StateDir() {
  if (const char* e = std::getenv("DASH_XAM_STATE"); e && *e) return e;
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return fs::path(x) / "xenondash" / "xam";
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : ".") / ".local" / "share" / "xenondash" / "xam";
}

namespace {

// Périphérique hôte de la partition système étendue (voir MountSystemExtPartition).
constexpr std::string_view kSystemExtHostDevice = "\\Device\\HostSystemExtPartition";

// Monte `dir` sous `device` (ex. \Device\MU0). Un échec est journalisé sans interrompre le démarrage.
void Mount(rex::filesystem::VirtualFileSystem* vfs, const std::string& device, const fs::path& dir, bool read_only) {
  if (!read_only) {
    std::error_code ec;
    fs::create_directories(dir, ec);
  }
  auto dev = std::make_unique<rex::filesystem::HostPathDevice>(device, dir, read_only);
  if (!dev->Initialize() || !vfs->RegisterDevice(std::move(dev))) {
    REXLOG_WARN("[dash] {} : montage sur {} impossible", device, dir.string());
    return;
  }
  REXLOG_INFO("[dash] {} monté sur {}", device, dir.string());
}

// Thread hôte dont le processus est le processus système (même corps que rex::system::XHostThread::Execute).
class SystemHostThread : public rex::system::XThread {
 public:
  SystemHostThread(rex::system::KernelState* ks, uint32_t stack_size, std::function<int()> fn)
      : XThread(ks, stack_size, 0, 0, 0, 0, false, false, ks->GetSystemProcess()),
        ks_(ks),
        fn_(std::move(fn)) {}

  void Execute() override {
    ks_->OnThreadExecute(this);
    thread_state()->context()->fpscr.InitHost();  // comme XHostThread::Execute du SDK (exceptions flottantes masquées)
    int ret = fn_();
    Exit(ret);
  }

 private:
  rex::system::KernelState* ks_;
  std::function<int()> fn_;
};

std::atomic<uint32_t> g_xam_lo{0}, g_xam_hi{0};

}  // namespace

void SetXamImageRange(uint32_t base, uint32_t size) {
  g_xam_lo.store(base);
  g_xam_hi.store(base + size);
}

// Les modules XUI du HUD (hud, vk, signin, createprofile... : bases 0x90100000-0x917E7000, avant dash.xex à 0x92000000) sont des
// extensions de XAM : leur code est traité comme celui de XAM (système, noms nus de XexLoadImage...).
bool InXamImage(uint32_t addr) {
  return (addr >= g_xam_lo.load() && addr < g_xam_hi.load()) || (addr >= 0x90000000u && addr < 0x91800000u);
}

void MountFlash(rex::filesystem::VirtualFileSystem* vfs, const fs::path& flash_dir) {
  if (!fs::is_directory(flash_dir)) {
    REXLOG_WARN("[dash] {} absent : la flash (\\SystemRoot) ne sera pas montée", flash_dir.string());
    return;
  }
  Mount(vfs, "\\SystemRoot", flash_dir, true);
  Mount(vfs, "\\Device\\Flash", flash_dir, true);
  // game: -> la flash. Les modules d'interface que XAM charge par leur nom nu sont rediriges vers
  // game:\flash\<nom> (voir le crochet XexLoadImage plus bas), et ce chemin n'existait pas comme
  // peripherique : chaque tentative echouait (« VFS: 'game:\flash\hud.xex' -> [no device] »).
  // Mesure du 2026-11-20 : 356 echecs et 356 nouvelles tentatives de chargement de hud.xex en 40 s,
  // soit une boucle qui ne peut pas aboutir. Un fichier present dans la flash est desormais
  // joignable par les deux chemins.
  Mount(vfs, "game:", flash_dir, true);
  // \Device\CdRom0 : XAM cree au boot le lien symbolique GAME: -> \Device\CdRom0 (detection du
  // lecteur de disque, comportement console). Sans peripherique derriere, ce lien pend et masque
  // game: (la correspondance des prefixes est insensible a la casse, l'ordre de la map arbitre) :
  // les chargements de modules des scenes du systeme UI echouaient (« no device »). Sur la console
  // le lecteur existe meme sans disque : on le monte sur un dossier VIDE (pas de disque insere),
  // et non sur les donnees — monter les assets fait voir a XAM un « disque non reconnu »
  // (mesure du 2026-11-21 : le titre de la boite d'erreur du demarrage est « Unrecognized... »).
  const fs::path cdrom_dir = StateDir() / "cdrom";
  std::error_code ec;
  fs::create_directories(cdrom_dir, ec);
  Mount(vfs, "\\Device\\CdRom0", cdrom_dir, true);
}

void MountXamDevices(rex::filesystem::VirtualFileSystem* vfs) {
  const fs::path root = StateDir();
  Mount(vfs, "\\Device\\Harddisk0\\Partition1", root / "hdd", false);
  Mount(vfs, "\\Device\\MU0", root / "mu0", false);
}

// Chemin sous lequel le module de la partition système étendue `name` (résolu par le VFS en `resolved`, sous
// kSystemExtHostDevice) est enregistré comme recompilé ; nullopt s'il ne l'est pas. FindRecompiledModule compare le chemin
// normalisé à l'égalité stricte, et le chargeur du SDK (LoadUserModule) refait la même recherche avec le chemin reçu : le
// guest_path du manifeste emploie l'alias de XAM (\sep\20352400\...) ; un appel par un autre alias de la même partition
// (\Device\Harddisk0\SystemExtPartition\...) serait refusé à tort, ou chargé sans son code recompilé. On essaie donc aussi
// le chemin relatif à la partition sous chacun des alias connus, en vérifiant que l'alias résout vers le même fichier ;
// l'appelant passe alors ce chemin-là au SDK.
std::optional<std::string> RecompiledSystemExtPath(rex::system::KernelState* ks, const char* name,
                                                   const std::string& resolved) {
  if (ks->FindRecompiledModule(name)) return std::string(name);
  const std::string rel = resolved.substr(kSystemExtHostDevice.size());  // "\20352400\Guide.AvatarMiniCreator.xex"
  for (const char* alias : {"\\sep", "\\Device\\Harddisk0\\SystemExtPartition"}) {
    std::string cand = alias + rel;
    if (!ks->FindRecompiledModule(cand)) continue;
    auto* e = REX_KERNEL_FS()->ResolvePath(cand);
    if (!e || rex::system::NormalizeGuestPath(e->absolute_path()) == rex::system::NormalizeGuestPath(resolved)) return cand;
  }
  return std::nullopt;
}

void MountSystemExtPartition(rex::filesystem::VirtualFileSystem* vfs, const fs::path& dir) {
  if (!fs::is_regular_file(dir / "system.manifest")) {
    REXLOG_WARN("[dash] {} sans system.manifest : partition système étendue absente (index des paquets de XAM vide)",
                dir.string());
    return;
  }
  // Le SDK enregistre avant nous un NullDevice sur \Device\Harddisk0 (accès bruts aux partitions) et le VFS prend le premier
  // périphérique dont le préfixe correspond : un montage direct sous \Device\Harddisk0\SystemExtPartition serait masqué
  // (« entry not found ... on device '\Device\Harddisk0' »). Le chemin de la console est donc un lien symbolique (résolu
  // avant la recherche du périphérique) vers un périphérique hôte de nom propre.
  const std::string host(kSystemExtHostDevice);
  Mount(vfs, host, dir, true);
  vfs->RegisterSymbolicLink("\\Device\\Harddisk0\\SystemExtPartition", host);
}

// UsbdBootEnumerationDoneEvent (xboxkrnl 0x2DB) : le SDK l'initialise signalé avec type = 1, soit
// EventSynchronizationObject (réarmement automatique) : le premier KeWaitForSingleObject consomme le signal, les suivants
// bloquent pour toujours (le fil d'initialisation de XAM, sub_81878E10, attend cet événement après un autre fil).
// Sur la console c'est un événement de notification (type 0) signalé en permanence.
void FixKernelVariables(rex::system::KernelState* ks, rex::runtime::ExportResolver* resolver) {
  // XboxHardwareInfo.Flags (xboxkrnl 0x156) : le SDK renvoie 0x20 seul (disque dur présent). L'initialisation du HUD de
  // XAM (sub_81872230 : sub_818A2F90, qui charge huduiskin.xex et hud.xex) n'est lancée que si le bit 0x200 est posé.
  if (auto* hw = resolver ? resolver->GetExportByOrdinal("xboxkrnl.exe", 0x0156) : nullptr; hw && hw->variable_ptr) {
    auto* flags = reinterpret_cast<uint32_t*>(ks->memory()->virtual_membase() + hw->variable_ptr);
    *flags = __builtin_bswap32(__builtin_bswap32(*flags) | 0x200u);
    REXLOG_INFO("[dash] XboxHardwareInfo.Flags = 0x{:08X}", __builtin_bswap32(*flags));
  }
  // XboxKrnlVersion (xboxkrnl 0x158) : le SDK annonce 2.15.65535.0. Sur la console le noyau et le Dashboard sont de la même
  // version système ; XAM compare la version de system.manifest (partition système étendue) à celle du noyau
  // (sub_818BE8B8 : sub_818BF860 contre sub_818B4E80) et ignore un manifeste d'une autre version : l'index de son
  // gestionnaire de paquets restait vide. On reprend la version du Dashboard (en-tête d'exécution de l'exécutable).
  if (auto* kv = resolver ? resolver->GetExportByOrdinal("xboxkrnl.exe", 0x0158) : nullptr; kv && kv->variable_ptr) {
    auto mod = ks->GetExecutableModule();
    const auto* info = mod && mod->xex_module() ? mod->xex_module()->opt_execution_info() : nullptr;
    if (info) {
      const uint32_t v = info->version_value;  // majeur:4 mineur:4 build:16 qfe:8
      uint8_t* p = ks->memory()->virtual_membase() + kv->variable_ptr;
      const uint16_t major = v >> 28, minor = (v >> 24) & 0xF, build = (v >> 8) & 0xFFFF;
      const uint8_t qfe = v & 0xFF;
      p[0] = major >> 8; p[1] = major & 0xFF;
      p[2] = minor >> 8; p[3] = minor & 0xFF;
      p[4] = build >> 8; p[5] = build & 0xFF;
      p[7] = qfe;
      REXLOG_INFO("[dash] XboxKrnlVersion = {}.{}.{}.{} (version du Dashboard)", major, minor, build, qfe);
    }
  }
  // NOTE (2026-11-20) : le compteur d'interfaces systeme de XAM (0x81BFCAE8) vaut 4 plus tard dans
  // l'execution, et le remettre a zero au demarrage ne change rien (il vaut deja 0 ici). Ce n'est
  // donc pas lui qui maintient l'indicateur d'activite a l'ecran. Piste ecartee par la mesure.
  auto* e = resolver ? resolver->GetExportByOrdinal("xboxkrnl.exe", 0x02DB) : nullptr;
  if (!e || !e->variable_ptr) return;
  uint8_t* hdr = ks->memory()->virtual_membase() + e->variable_ptr;
  if (hdr[0] == 1) {
    hdr[0] = 0;  // EventNotificationObject
    REXLOG_INFO("[dash] UsbdBootEnumerationDoneEvent (0x{:08X}) : événement de notification", e->variable_ptr);
  }
}

void RunInSystemProcess(rex::system::KernelState* ks, std::function<int()> fn) {
  rex::system::object_ref<SystemHostThread> t(new SystemHostThread(ks, 256 * 1024, std::move(fn)));
  t->set_name("XAM load");
  t->Create();
  t->Wait(0, 0, 0, nullptr);
}

// Variante NON bloquante : le thread vit jusqu'a la fin du processus et l'appelant repart tout de
// suite. Le chargement de XAM dans OnPostSetup bloquait le thread principal AVANT le premier dessin
// du Dashboard : aucun appel D3D n'arrivait, donc aucun device Vulkan n'etait cree et l'ecran
// restait noir (voir docs/xam_13604_compilation.md). Le thread doit rester reference : un
// object_ref local le detruirait en fin de fonction.
void RunInSystemProcessAsync(rex::system::KernelState* ks, std::function<int()> fn) {
  static std::mutex mu;
  static std::vector<rex::system::object_ref<SystemHostThread>> alive;  // vivants jusqu'a la fin
  rex::system::object_ref<SystemHostThread> t(new SystemHostThread(ks, 256 * 1024, std::move(fn)));
  t->set_name("XAM load");
  t->Create();
  std::lock_guard lock(mu);
  alive.push_back(std::move(t));
}

}  // namespace dash::xamrt

// KeGetCurrentProcessType : 2 (système) pour le code de XAM recompilé, quel que soit le fil (voir SetXamImageRange). Le
// code appelant est reconnu par l'adresse de retour (ctx.lr). La fonction du SDK vient de kernel_sdk_fn.h.
REX_HOOK_RAW(__imp__KeGetCurrentProcessType) {
  static PPCFunc* fn = dash::SdkFunction("__imp__KeGetCurrentProcessType");
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  fn(ctx, base);
  if (dash::xamrt::InXamImage(lr)) ctx.r3.u64 = 2;
}

// KeBugCheckEx / KeBugCheck : journalisation du code et de l'appelant avant le traitement du SDK (diagnostic de
// l'initialisation de XAM recompilé, qui appelle KeBugCheckEx quand une étape obligatoire échoue).
REX_HOOK_RAW(__imp__KeBugCheckEx) {
  static PPCFunc* fn = dash::SdkFunction("__imp__KeBugCheckEx");
  REXLOG_ERROR("[kernel] KeBugCheckEx(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}) depuis lr=0x{:08X}", ctx.r3.u32, ctx.r4.u32,
               ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, static_cast<uint32_t>(ctx.lr));
  fn(ctx, base);
}
REX_HOOK_RAW(__imp__KeBugCheck) {
  static PPCFunc* fn = dash::SdkFunction("__imp__KeBugCheck");
  REXLOG_ERROR("[kernel] KeBugCheck(0x{:X}) depuis lr=0x{:08X}", ctx.r3.u32, static_cast<uint32_t>(ctx.lr));
  fn(ctx, base);
}

// XexLoadImage : XAM charge ses modules d'interface par leur nom nu ("hud.xex", "signin.xex", "vk.xex"...), que la console
// cherche dans la flash. Le SDK résout un nom nu dans le dossier du module exécutable (ici \Device\Harddisk0\Partition1,
// le périphérique d'état de XAM) : le fichier n'y est pas. Pour les appels venant de XAM, un nom nu présent dans la
// flash (\SystemRoot) est remplacé par game:\flash\<nom> (même fichier ; ce chemin est celui que le manifeste donne aux
// modules recompilés, ex. game:\flash\hud.xex) ; les autres noms passent tels quels au SDK.
//
// Un module de la partition système étendue (MountSystemExtPartition) qui n'est pas recompilé est
// refusé (STATUS_OBJECT_NAME_NOT_FOUND, *hmodule = 0) : son code n'existe pas ici, le SDK chargerait une image dont le
// point d'entrée n'est pas dans la table des fonctions. Cas observé : après connexion d'un profil sans avatar, signin.xex
// trouve Guide.AvatarMiniCreator.xex dans l'index des paquets (XamPackageManagerGetFileSize) et appelle
// XamShowAvatarMiniCreatorUI ; XAM chargeait alors ce module non recompilé et le Guide restait un panneau vide. Refusé,
// XAM revient au HUD. Limité à cette partition : les modules de ressources de la flash (huduiskin.xex...) se chargent
// sans code recompilé et doivent passer.
REX_HOOK_RAW(__imp__XexLoadImage) {
  static PPCFunc* fn = dash::SdkFunction("__imp__XexLoadImage");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr)) && ctx.r3.u32) {
    const char* name = reinterpret_cast<const char*>(base + ctx.r3.u32);
    // XAM passe par ses propres liens symboliques (ex. \sep\20352400\Guide.AvatarMiniCreator.xex) : le chemin est résolu
    // par le VFS et c'est le périphérique hôte de la partition qui est reconnu.
    auto* entry = std::strpbrk(name, "\\/:") ? REX_KERNEL_FS()->ResolvePath(name) : nullptr;
    if (entry && entry->absolute_path().starts_with(dash::xamrt::kSystemExtHostDevice)) {
      auto* ks = REX_KERNEL_STATE();
      auto recomp = ks->GetModule(name) ? std::optional<std::string>(name)
                                        : dash::xamrt::RecompiledSystemExtPath(ks, name, entry->absolute_path());
      if (recomp && *recomp != name) {
        // Même module, autre alias : le SDK doit recevoir le chemin enregistré pour câbler le code recompilé.
        static std::mutex amu;
        static std::map<std::string, uint32_t> alias_cache;  // chemin enregistré -> chaîne invitée
        std::lock_guard<std::mutex> lock(amu);
        uint32_t& g = alias_cache[*recomp];
        if (!g) {
          g = REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(recomp->size() + 1));
          std::memcpy(base + g, recomp->c_str(), recomp->size() + 1);
        }
        REXLOG_INFO("[xam] XexLoadImage(\"{}\") -> {} (chemin du module recompilé)", name, *recomp);
        ctx.r3.u64 = g;
        name = reinterpret_cast<const char*>(base + g);
      }
      if (!recomp) {
        REXLOG_WARN("[xam] XexLoadImage(\"{}\") refusé : module non recompilé", name);
        if (ctx.r6.u32) {
          uint8_t* h = base + ctx.r6.u32;
          h[0] = h[1] = h[2] = h[3] = 0;
        }
        ctx.r3.u64 = 0xC0000034;  // STATUS_OBJECT_NAME_NOT_FOUND
        return;
      }
    }
    if (*name && !std::strpbrk(name, "\\/:")) {
      static std::mutex mu;
      static std::map<std::string, uint32_t> cache;  // nom nu -> chaîne invitée "\SystemRoot\<nom>" (0 : absent de la flash)
      uint32_t guest = 0;
      {
        std::lock_guard<std::mutex> lock(mu);
        auto it = cache.find(name);
        if (it == cache.end()) {
          const std::string full = std::string("game:\\flash\\") + name;
          uint32_t g = 0;
          if (REX_KERNEL_FS()->ResolvePath(std::string("\\SystemRoot\\") + name)) {
            g = REX_KERNEL_MEMORY()->SystemHeapAlloc(static_cast<uint32_t>(full.size() + 1));
            std::memcpy(base + g, full.c_str(), full.size() + 1);
          }
          it = cache.emplace(name, g).first;
        }
        guest = it->second;
      }
      if (guest) {
        REXLOG_INFO("[xam] XexLoadImage(\"{}\") -> game:\\flash\\{}", name, name);
        ctx.r3.u64 = guest;
      }
    }
  }
  fn(ctx, base);
}
