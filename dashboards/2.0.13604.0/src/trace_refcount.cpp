// Instrumentation (temporaire, DASH_TRACE_REFCOUNT=1) : compte les appels au compteur de
// references du Dashboard 13604 et journalise l'etat de l'objet, pour identifier les deux
// proprietaires concurrents du plantage « vtable nulle au demontage ».
//
// Mise en place SANS toucher au code genere : FunctionDispatcher::SetFunction remplace une
// fonction interne par une enveloppe qui appelle l'originale. Le code recompile n'est pas
// modifie (il serait ecrase au prochain `rexglue codegen`).
//
// Voir diagnostic_gel_13604.md, « Cause 2 ».
#include <atomic>
#include <cstdlib>
#include <cstdio>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>

namespace dash::trace {
namespace {

// Adresses du Dashboard 2.0.13604 (dash.xex, image de base 0x92000000).
constexpr uint32_t kRefcountRelease = 0x922AA198;  // refcount -= 1 (offset +32) puis destruction
constexpr uint32_t kDestructor = 0x922AA048;       // lit la vtable du membre +36 et l'appelle
constexpr uint32_t kReleaseOuter = 0x922AA208;     // appelant de kRefcountRelease

std::atomic<long> g_release_calls{0};
std::atomic<long> g_destroy_calls{0};
std::atomic<long> g_outer_calls{0};
std::atomic<bool> g_armed{false};

bool enabled() {
  static const bool v = std::getenv("DASH_TRACE_REFCOUNT") != nullptr;
  return v;
}

uint32_t rd(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

// Etat utile d'un objet candidat : refcount (+32), membre +36 et sa vtable.
void DumpObject(uint8_t* base, uint32_t obj, const char* what, long n) {
  if (!obj) {
    REXLOG_INFO("[rc] {} #{} obj=nul", what, n);
    return;
  }
  const uint32_t refcount = rd(base, obj + 32);
  const uint32_t member36 = rd(base, obj + 36);
  const uint32_t vtable = member36 ? rd(base, member36) : 0;
  REXLOG_INFO("[rc] {} #{} obj={:08X} refcount(+32)={} membre(+36)={:08X} vtable={:08X}", what, n, obj,
              refcount, member36, vtable);
  if (member36 && !vtable) {
    REXLOG_ERROR("[rc]   ^ membre +36 NON nul mais vtable nulle : objet deja detruit ou jamais construit");
  }
}

using Fn = PPCFunc;

Fn* g_orig_release = nullptr;
Fn* g_orig_destroy = nullptr;
Fn* g_orig_outer = nullptr;

void SpyRelease(PPCContext& ctx, uint8_t* base) {
  const long n = ++g_release_calls;
  if (n <= 60) DumpObject(base, ctx.r3.u32, "release", n);
  g_orig_release(ctx, base);
}

void SpyDestroy(PPCContext& ctx, uint8_t* base) {
  const long n = ++g_destroy_calls;
  if (n <= 60) {
    DumpObject(base, ctx.r3.u32, "destructeur", n);
    REXLOG_INFO("[rc]   lr={:08X} (appelant)", static_cast<uint32_t>(ctx.lr));
  }
  g_orig_destroy(ctx, base);
}

void SpyOuter(PPCContext& ctx, uint8_t* base) {
  const long n = ++g_outer_calls;
  if (n <= 60) REXLOG_INFO("[rc] releaseOuter #{}, obj={:08X}", n, ctx.r3.u32);
  g_orig_outer(ctx, base);
}

// Installe les espions une seule fois, quand le dispatcher est pret (appele depuis l'app).
void Arm() {
  REXLOG_INFO("[rc] Arm() appele, enabled={}", enabled());
  if (!enabled() || g_armed.exchange(true)) return;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->function_dispatcher()) {
    REXLOG_WARN("[rc] instrumentation impossible : dispatcher indisponible");
    return;
  }
  auto* d = ks->function_dispatcher();
  g_orig_release = d->GetFunction(kRefcountRelease);
  g_orig_destroy = d->GetFunction(kDestructor);
  g_orig_outer = d->GetFunction(kReleaseOuter);
  REXLOG_INFO("[rc] originales : release={} destructeur={} outer={}", (void*)g_orig_release,
              (void*)g_orig_destroy, (void*)g_orig_outer);
  if (g_orig_release) d->SetFunction(kRefcountRelease, SpyRelease);
  if (g_orig_destroy) d->SetFunction(kDestructor, SpyDestroy);
  if (g_orig_outer) d->SetFunction(kReleaseOuter, SpyOuter);
  REXLOG_INFO("[rc] espions installes (DASH_TRACE_REFCOUNT=1)");
}

}  // namespace

void ArmIfRequested() { Arm(); }

}  // namespace dash::trace
