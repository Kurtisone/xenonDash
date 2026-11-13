// xenondash common - garde des clients audio du noyau pour XAM recompilé.
//
// XAM recompilé a son propre moteur audio : un fil l'appelle (callback invité lancé par le fil audio du SDK) et appelle
// XAudioSubmitRenderDriverFrame, tandis qu'un autre fil désinscrit puis réinscrit le client
// (XAudioUnregisterRenderDriverClient, sub_819FBE28). AudioSystem::SubmitFrame du SDK ne vérifie pas que le client
// existe encore (assert seulement) : après la désinscription, `clients_[index].driver` est nul et le fil audio plante
// (SIGSEGV dans SubmitFrame, constaté par gdb).
// Ici : les clients inscrits sont suivis ; une soumission pour un client absent est ignorée (succès), et la
// désinscription attend les soumissions en cours. Les fonctions du SDK sont appelées telles quelles.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>

#include "kernel_sdk_fn.h"

namespace {

constexpr uint32_t kMaxClients = 64;  // le SDK en gère 8 (kMaximumClientCount)

std::shared_mutex g_mutex;
std::atomic<bool> g_registered[kMaxClients];

}  // namespace

// XAudioRegisterRenderDriverClient(PXAUDIO_CALLBACK* callback, DWORD* driver) : *driver = 0x4155 0000 | indice.
REX_HOOK_RAW(__imp__XAudioRegisterRenderDriverClient) {
  static PPCFunc* fn = dash::SdkFunction("__imp__XAudioRegisterRenderDriverClient");
  const uint32_t driver_ptr = ctx.r4.u32;
  fn(ctx, base);
  if (ctx.r3.u32 == 0 && driver_ptr) {
    const uint32_t v = __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + driver_ptr));
    if ((v & 0xFFFF0000u) == 0x41550000u && (v & 0xFFFFu) < kMaxClients) g_registered[v & 0xFFFFu] = true;
  }
}

// XAudioUnregisterRenderDriverClient(driver)
REX_HOOK_RAW(__imp__XAudioUnregisterRenderDriverClient) {
  static PPCFunc* fn = dash::SdkFunction("__imp__XAudioUnregisterRenderDriverClient");
  const uint32_t v = ctx.r3.u32;
  std::unique_lock lock(g_mutex);
  if ((v & 0xFFFF0000u) == 0x41550000u && (v & 0xFFFFu) < kMaxClients) g_registered[v & 0xFFFFu] = false;
  fn(ctx, base);
}

// XAudioSubmitRenderDriverFrame(driver, samples)
REX_HOOK_RAW(__imp__XAudioSubmitRenderDriverFrame) {
  static PPCFunc* fn = dash::SdkFunction("__imp__XAudioSubmitRenderDriverFrame");
  const uint32_t v = ctx.r3.u32;
  std::shared_lock lock(g_mutex);
  if ((v & 0xFFFF0000u) != 0x41550000u || (v & 0xFFFFu) >= kMaxClients || !g_registered[v & 0xFFFFu]) {
    ctx.r3.u64 = 0;  // X_ERROR_SUCCESS : client absent, trame ignorée
    return;
  }
  fn(ctx, base);
}

// Réglages du « ducker » (atténuation du son du titre pendant les sons du système) : le SDK les laisse en stub (f1
// inchangé). Le moteur audio de XAM divise par le temps de relâchement (sub_81BAB930, fdivs f0 = f13 / (f1 * c)) : avec
// f1 nul ou ancien, la division par zéro lève SIGFPE (exceptions flottantes hôtes démasquées par le code invité).
// Valeur renvoyée : 1.0 pour chaque lecture (niveau, seuil, durées), pas de ducker actif.
#define DASH_DUCKER_GET(name) \
  REX_HOOK_RAW(__imp__##name) { (void)base; ctx.f1.f64 = 1.0; ctx.r3.u64 = 0; }
#define DASH_DUCKER_SET(name) \
  REX_HOOK_RAW(__imp__##name) { (void)base; ctx.r3.u64 = 0; }
DASH_DUCKER_GET(XAudioGetDuckerLevel)
DASH_DUCKER_GET(XAudioGetDuckerThreshold)
DASH_DUCKER_GET(XAudioGetDuckerAttackTime)
DASH_DUCKER_GET(XAudioGetDuckerReleaseTime)
DASH_DUCKER_GET(XAudioGetDuckerHoldTime)
DASH_DUCKER_SET(XAudioSetDuckerLevel)
DASH_DUCKER_SET(XAudioSetDuckerThreshold)
DASH_DUCKER_SET(XAudioSetDuckerAttackTime)
DASH_DUCKER_SET(XAudioSetDuckerReleaseTime)
DASH_DUCKER_SET(XAudioSetDuckerHoldTime)
DASH_DUCKER_SET(XAudioIsDuckerEnabled)
