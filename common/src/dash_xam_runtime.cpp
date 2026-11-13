// xenondash common - périphériques attendus par XAM recompilé (voir dash_xam_runtime.h).

#include "dash_xam_runtime.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
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
    int ret = fn_();
    Exit(ret);
  }

 private:
  rex::system::KernelState* ks_;
  std::function<int()> fn_;
};

}  // namespace

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

void RunInSystemProcess(rex::system::KernelState* ks, std::function<int()> fn) {
  rex::system::object_ref<SystemHostThread> t(new SystemHostThread(ks, 256 * 1024, std::move(fn)));
  t->set_name("XAM load");
  t->Create();
  t->Wait(0, 0, 0, nullptr);
}

}  // namespace dash::xamrt
