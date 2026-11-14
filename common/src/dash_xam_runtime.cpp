// xenondash common - périphériques attendus par XAM recompilé (voir dash_xam_runtime.h).

#include "dash_xam_runtime.h"

#include "kernel_sdk_fn.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

namespace dash::xamrt {
namespace {

namespace fs = std::filesystem;

// Répertoire d'état des périphériques factices : DASH_XAM_STATE, sinon ${XDG_DATA_HOME:-~/.local/share}/xenondash/xam.
fs::path StateDir() {
  if (const char* e = std::getenv("DASH_XAM_STATE"); e && *e) return e;
  if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return fs::path(x) / "xenondash" / "xam";
  const char* home = std::getenv("HOME");
  return fs::path(home ? home : ".") / ".local" / "share" / "xenondash" / "xam";
}

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
}

void MountXamDevices(rex::filesystem::VirtualFileSystem* vfs) {
  const fs::path root = StateDir();
  Mount(vfs, "\\Device\\Harddisk0\\Partition1", root / "hdd", false);
  Mount(vfs, "\\Device\\MU0", root / "mu0", false);
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
REX_HOOK_RAW(__imp__XexLoadImage) {
  static PPCFunc* fn = dash::SdkFunction("__imp__XexLoadImage");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr)) && ctx.r3.u32) {
    const char* name = reinterpret_cast<const char*>(base + ctx.r3.u32);
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
