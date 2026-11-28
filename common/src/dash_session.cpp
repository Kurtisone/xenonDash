#include "dash_session.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include <rex/kernel/xam/module.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmodule.h>
#include <rex/system/xthread.h>

#include "dash_xam_redirect.h"
#include "dash_xam_runtime.h"

namespace dash::session {

std::filesystem::path Path() { return dash::xamrt::StateDir() / "session.txt"; }

namespace {
std::string Hex(const std::vector<uint8_t>& v) {
  std::string s;
  char b[3];
  for (uint8_t c : v) {
    std::snprintf(b, sizeof b, "%02x", c);
    s += b;
  }
  return s;
}
std::vector<uint8_t> Unhex(const std::string& s) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < s.size(); i += 2) v.push_back(uint8_t(std::strtoul(s.substr(i, 2).c_str(), nullptr, 16)));
  return v;
}
}  // namespace

bool Write(const Session& s) {
  std::error_code ec;
  std::filesystem::create_directories(Path().parent_path(), ec);
  std::ofstream f(Path(), std::ios::trunc);
  if (!f) {
    REXLOG_WARN("[session] écriture impossible : {}", Path().string());
    return false;
  }
  for (int i = 0; i < 4; ++i)
    if (s.xuid[i]) f << "xuid" << i << "=" << std::hex << s.xuid[i] << std::dec << "\n";
  if (!s.launch.empty()) f << "launch=" << Hex(s.launch) << "\n";
  f.flush();
  REXLOG_INFO("[session] écrite ({} compte(s), {} octets de données de lancement) : {}",
              int(!!s.xuid[0]) + int(!!s.xuid[1]) + int(!!s.xuid[2]) + int(!!s.xuid[3]), s.launch.size(), Path().string());
  return bool(f);
}

std::array<uint64_t, 4> SignedInXuids(const PPCContext& ctx, uint8_t* base) {
  std::array<uint64_t, 4> r{};
  for (uint32_t u = 0; u < 4; ++u) r[u] = dash::xamredir::UserXuid(ctx, base, u);
  return r;
}

namespace {
bool Read(Session& s) {
  std::ifstream f(Path());
  if (!f) return false;
  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    if (k.rfind("xuid", 0) == 0 && k.size() == 5 && k[4] >= '0' && k[4] <= '3')
      s.xuid[k[4] - '0'] = std::strtoull(v.c_str(), nullptr, 16);
    else if (k == "launch")
      s.launch = Unhex(v);
  }
  f.close();
  std::error_code ec;
  std::filesystem::remove(Path(), ec);
  return true;
}

std::atomic<bool> g_logon_done{false};
std::atomic<bool> g_return_my_xbox{false};
}  // namespace

bool ReturnToMyXbox() { return g_return_my_xbox.load(); }
void ReturnToMyXboxDone() { g_return_my_xbox.store(false); }

void Restore(rex::system::KernelState* ks) {
  Session s;
  if (!Read(s)) return;
  REXLOG_INFO("[session] lue : {} compte(s), {} octets de données de lancement", int(!!s.xuid[0]) + int(!!s.xuid[1]) + int(!!s.xuid[2]) + int(!!s.xuid[3]),
              s.launch.size());
  if (s.launch.size() >= 8)
    REXLOG_INFO("[session] données de lancement : commande (+4) {:#x}, début {}", uint32_t(s.launch[4]) << 24 | uint32_t(s.launch[5]) << 16 | uint32_t(s.launch[6]) << 8 | s.launch[7],
                Hex(std::vector<uint8_t>(s.launch.begin(), s.launch.begin() + std::min<size_t>(s.launch.size(), 96))));
  if (s.launch.size() >= 8 && s.launch[4] == 0 && s.launch[5] == 0 && s.launch[6] == 0 && s.launch[7] == 0x35) g_return_my_xbox.store(true);
  if (!s.launch.empty()) {
    if (auto xam = ks->GetKernelModule<rex::kernel::xam::XamModule>("xam.xex")) {
      auto& ld = xam->loader_data();
      ld.launch_data = s.launch;
      ld.launch_data_present = true;
    }
  }
  if (!s.Any() || !dash::xamredir::ProfileModeXam()) return;
  static rex::system::object_ref<rex::system::XHostThread> host;
  host.reset(new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, s] {
    auto* ts = rex::runtime::ThreadState::Get();
    if (!ts) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(); };
    for (int i = 0; i < 300 && !ks->GetModule("xam.xex", true); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::seconds(4));  // point d'entrée de XAM exécuté, redirections configurées
    uint8_t* base = ks->memory()->virtual_membase();
    const uint32_t arr = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x40);
    std::memset(base + arr, 0, 0x40);
    for (int i = 0; i < 4; ++i) {
      const uint64_t be = __builtin_bswap64(s.xuid[i]);
      std::memcpy(base + arr + 8 * i, &be, 8);
    }
    PPCContext ctx = *ts->context();
    ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x200;
    ctx.r3.u64 = arr;
    ctx.r4.u64 = 0x424;  // drapeaux de signin.xex (sub_90102xxx, XamUserLogon) : connexion locale, sans Xbox LIVE
    ctx.r5.u64 = 0;
    const bool ok = dash::xamredir::CallXamExport("XamUserLogon", ctx, base);
    REXLOG_INFO("[session] connexion rejouée : XamUserLogon -> {} 0x{:08X} après {} ms", ok ? "XAM" : "absent", ctx.r3.u32, ms());
    g_logon_done.store(true);
    return 0;
  }));
  host->set_name("session logon");
  host->Create();
  for (int i = 0; i < 300 && !g_logon_done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REXLOG_INFO("[session] attente de la connexion : {}", g_logon_done.load() ? "faite" : "expirée (30 s)");
}

}  // namespace dash::session
