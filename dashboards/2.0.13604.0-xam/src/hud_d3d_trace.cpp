// Palier N2 (provisoire) : trace des fonctions D3D de XAM (crochets de src_xam_d3d/xam_d3d_hooks.cpp).
//   DASH_HUD_D3D_TRACE=1        : une ligne par seconde (appels par fonction) + premiers appels de chaque fonction
//   DASH_HUD_D3D_SEQ=<n>        : sequence complete des appels pendant la n-ieme execution de la fonction d'image 818A1368
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

namespace {
struct Slot { std::atomic<uint64_t> n{0}; std::atomic<int> logged{0}; };
std::mutex g_mu;
std::map<uint32_t, Slot*> g_slots;
Slot* Get(uint32_t a) {
  std::lock_guard<std::mutex> l(g_mu);
  auto& s = g_slots[a];
  if (!s) s = new Slot;
  return s;
}
thread_local bool t_rec = false;
std::atomic<uint64_t> g_frames{0};
}  // namespace

// Renvoie true si l'appel a ete entierement traite (fonction d'origine appelee) : mode diagnostic seulement.
extern "C" bool hudd3d_probe(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*)) {
  static const bool trace = std::getenv("DASH_HUD_D3D_TRACE") != nullptr;
  static const long seq = std::getenv("DASH_HUD_D3D_SEQ") ? std::atol(std::getenv("DASH_HUD_D3D_SEQ")) : -1;
  static const long seq2 = std::getenv("DASH_HUD_D3D_SEQ2") ? std::atol(std::getenv("DASH_HUD_D3D_SEQ2")) : -1;
  static const bool diag = std::getenv("DASH_HUD_D3D_TRACE") || std::getenv("DASH_HUD_D3D_SEQ") || std::getenv("DASH_HUD_D3D_DEVDUMP") || std::getenv("DASH_HUD_D3D_PROBE");
  if (!diag) return false;
  static const char* devdump = std::getenv("DASH_HUD_D3D_DEVDUMP");  // <dossier> : copie de 0x6000 octets du peripherique au Ne appel de 819C90D8
  if (devdump && addr == 0x819C90D8) {
    static std::atomic<int> k{0};
    int n = ++k;
    if (n == 3 || n == 40000 || n == 80000 || n == 120000 || n == 160000) {
      char fn[600];
      std::snprintf(fn, sizeof fn, "%s/dev_%d_%08X.bin", devdump, n, ctx.r3.u32);
      if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(base + ctx.r3.u32, 1, 0x6000, f); std::fclose(f); }
    }
  }
  static const char* probe = std::getenv("DASH_HUD_D3D_PROBE");  // <dossier> : arguments des BeginVertices et objets shader lies
  if (probe && addr == 0x819C8E38) {
    const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, cnt = ctx.r5.u32, stride = ctx.r6.u32;
    orig(ctx, base);
    static std::atomic<int> nb{0};
    static std::set<uint32_t> seen;
    static std::mutex mu;
    int n = ++nb;
    auto rd = [&](uint32_t a) { return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a)); };
    uint32_t ps = rd(dev + 12944), vs = rd(dev + 12948);
    if (n <= 3) {
      auto hexw = [&](const uint8_t* p, int cnt) { std::string r; char b[12]; for (int i = 0; i < cnt; ++i) { std::snprintf(b, sizeof b, " %08X", __builtin_bswap32(*reinterpret_cast<const uint32_t*>(p + 4 * i))); r += b; } return r; };
      const uint8_t* phys = rex::system::kernel_memory()->TranslatePhysical(0);
      uint32_t vm = rd(vs + 0x28), pm = rd(ps + 0x0C);
      REXLOG_INFO("[probe] vs ucode ptr {:08X} virt:{}", vm, hexw(base + vm, 21));
      REXLOG_INFO("[probe] vs ucode ptr {:08X} phys:{}", vm, hexw(phys + ((vm & 0x1FFFFFFF) + ((((vm >> 20) + 0x200) & 0x1000))), 21));
      REXLOG_INFO("[probe] ps ucode ptr {:08X} virt:{}", pm, hexw(base + pm, 21));
      REXLOG_INFO("[probe] ps ucode ptr {:08X} phys:{}", pm, hexw(phys + ((pm & 0x1FFFFFFF) + ((((pm >> 20) + 0x200) & 0x1000))), 21));
      REXLOG_INFO("[probe] vs container microcode (obj+0x300):{}", hexw(base + vs + 0x300, 21));
    }
    if (n <= 60) REXLOG_INFO("[probe] Begin #{} prim={} count={} stride={} -> {:08X} vs={:08X} ps={:08X}", n, prim, cnt, stride, ctx.r3.u32, vs, ps);
    std::lock_guard<std::mutex> l(mu);
    for (uint32_t o : {vs, ps}) {
      if (o && !seen.count(o)) {
        seen.insert(o);
        char fn[600];
        std::snprintf(fn, sizeof fn, "%s/shobj_%08X.bin", probe, o);
        if (FILE* f = std::fopen(fn, "wb")) { std::fwrite(base + o, 1, 0x800, f); std::fclose(f); }
      }
    }
    return true;
  }
  if (trace || seq >= 0) {
    Slot* s = Get(addr);
    uint64_t k = ++s->n;
    if (trace && s->logged.load() < 3) {
      s->logged++;
      REXLOG_INFO("[hudd3d] {:08X} #{} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} lr={:08X}", addr, k, ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, (uint32_t)ctx.lr);
    }
    if (addr == 0x818A1368) {
      uint64_t f = ++g_frames;
      static const auto t0 = std::chrono::steady_clock::now();
      static const double seqt = std::getenv("DASH_HUD_D3D_SEQT") ? std::atof(std::getenv("DASH_HUD_D3D_SEQT")) : -1.0;
      static double last_seqt_done = -1;
      const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if ((long)f == seq || (long)f == seq2 || (seqt >= 0 && now >= seqt && last_seqt_done < 0)) { t_rec = true; if (seqt >= 0 && now >= seqt) last_seqt_done = now; }
      orig(ctx, base);
      if (t_rec) { t_rec = false; REXLOG_INFO("[hudd3d-seq] fin de la fonction d'image {}", f); }
      return true;
    }
    if (t_rec)
      REXLOG_INFO("[hudd3d-seq] {:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} r8={:08X} f1={} lr={:08X}", addr, ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.f1.f64, (uint32_t)ctx.lr);
    static std::once_flag once;
    std::call_once(once, [] {
      if (!trace) return;
      std::thread([] {
        for (;;) {
          std::this_thread::sleep_for(std::chrono::seconds(5));
          static std::map<uint32_t, uint64_t> prev;
          std::string line;
          {
            std::lock_guard<std::mutex> l(g_mu);
            for (auto& [a, sl] : g_slots) {
              uint64_t n = sl->n.load();
              if (n != prev[a]) { char b[40]; std::snprintf(b, sizeof b, " %08X:%llu", a, (unsigned long long)(n - prev[a])); line += b; prev[a] = n; }
            }
          }
          if (!line.empty()) REXLOG_INFO("[hudd3d-rate/5s]{}", line);
        }
      }).detach();
    });
  }
  orig(ctx, base);
  return true;
}

// Version sans facade native (build sans DASH_NATIVE_GPU) : seulement les diagnostics ; la facade (native_gpu/src/hud_front.cpp) la remplace.
extern "C" __attribute__((weak)) void hudd3d_call(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*)) {
  if (!hudd3d_probe(addr, ctx, base, orig)) orig(ctx, base);
}
