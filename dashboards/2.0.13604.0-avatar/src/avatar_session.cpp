// avatar (prototype de phase 1) : rejoue la connexion des profils du Dashboard dans le XAM recompilé du processus de l'éditeur.
//
// Sur console, les utilisateurs connectés survivent au changement de titre (XAM est dans le processus système). Ici, l'éditeur est un
// second processus : son XAM démarre sans utilisateur connecté et l'éditeur annonce « The player signed out. The avatar editor will now
// exit. ». Le Dashboard doit donc léguer ses connexions (XUID par emplacement) ; DASH_AVATAR_LOGON=<XUID hexa>[,<XUID>...] les rejoue par
// XamUserLogon(XUID[4], drapeaux 0x424, NULL) de XAM recompilé (mêmes arguments que signin.xex, sub_90102xxx) depuis un fil invité.
// Version définitive : fichier de session écrit par le Dashboard avant le lancement (voir l'étude, étape « session »).
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/system/xmodule.h>

#include "dash_xam_redirect.h"

static std::atomic<bool> g_done{false};

void AvatarReplayLogon(rex::system::KernelState* ks) {
  const char* env = std::getenv("DASH_AVATAR_LOGON");
  if (!env || !*env) return;
  std::vector<uint64_t> xuids;
  for (size_t p = 0; p < std::strlen(env);) {
    size_t e = std::string_view(env).find(',', p);
    if (e == std::string_view::npos) e = std::strlen(env);
    xuids.push_back(std::strtoull(std::string(env + p, e - p).c_str(), nullptr, 16));
    p = e + 1;
  }
  static rex::system::object_ref<rex::system::XHostThread> host;
  host.reset(new rex::system::XHostThread(ks, 256 * 1024, 0, [ks, xuids] {
    auto* ts = rex::runtime::ThreadState::Get();
    if (!ts) return 0;
    for (int i = 0; i < 300 && !ks->GetModule("xam.xex", true); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::this_thread::sleep_for(std::chrono::seconds(4));  // XAM : point d'entrée exécuté, Configure fait
    uint8_t* base = ks->memory()->virtual_membase();
    const uint32_t arr = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x40);
    std::memset(base + arr, 0, 0x40);
    for (size_t i = 0; i < xuids.size() && i < 4; ++i) {
      const uint64_t be = __builtin_bswap64(xuids[i]);
      std::memcpy(base + arr + 8 * i, &be, 8);
    }
    PPCContext ctx = *ts->context();
    ctx.r1.u64 = static_cast<uint32_t>(ctx.r1.u64) - 0x200;
    ctx.r3.u64 = arr;
    ctx.r4.u64 = 0x424;
    ctx.r5.u64 = 0;
    const bool ok = dash::xamredir::CallXamExport("XamUserLogon", ctx, base);
    g_done.store(true);
    REXLOG_INFO("[avatar] connexion rejouée : XamUserLogon({} XUID, 0x424) -> {} 0x{:08X}", xuids.size(), ok ? "XAM" : "absent", ctx.r3.u32);
    return 0;
  }));
  host->set_name("avatar session");
  host->Create();
  // Prototype : l'éditeur lit l'état de connexion dès son démarrage ; on retient OnPostSetup (donc le démarrage du titre) jusqu'à la
  // connexion rejouée (borne 30 s). Si XAM dépend du titre pour s'initialiser, l'attente expire (mesure à faire).
  if (std::getenv("DASH_AVATAR_WAIT"))
    for (int i = 0; i < 300 && !g_done.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  REXLOG_INFO("[avatar] attente de la connexion rejouée : {}", g_done.load() ? "faite" : "expirée");
}
