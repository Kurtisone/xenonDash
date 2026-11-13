// xenondash common - objets du noyau définis par XAM (ObCreateObject, ObInsertObject) pour XAM recompilé.
//
// XAM déclare ses propres types d'objets (descripteur OBJECT_TYPE dans ses données, étiquette 'Nfto') et crée ses
// ordonnanceurs de tâches avec ObCreateObject / ObInsertObject (sub_8187D9A8, sub_818C3CD0, sub_818924D8, sub_81A56A38).
// Le SDK les laisse en stubs (r3 inchangé) : *objet n'est jamais écrit, la fonction de XAM renvoie un objet nul, et
// l'initialisation du sous-système d'audio de XAM échoue (E_FAIL de l'application 0xFD).
// Ici : ObCreateObject alloue le corps de l'objet (mémoire invitée du tas système, remise à zéro, précédée d'un
// en-tête de 16 octets : compte de pointeurs, compte de handles, type) ; ObInsertObject lui attribue un handle propre
// à ce fichier ; ObReferenceObjectByHandle et NtClose reconnaissent ces handles et passent les autres au SDK.
// Limites : le corps n'est jamais libéré (ObDereferenceObject du SDK ignore ces objets) et les procédures du type
// (destructeur, fermeture) ne sont pas appelées.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "kernel_sdk_fn.h"

namespace {

constexpr uint32_t kHandleBase = 0xF9000000u;  // le SDK utilise 0xF8000000 + n
constexpr uint32_t kHeader = 16;

std::mutex g_mutex;
std::unordered_map<uint32_t, uint32_t> g_handle_to_body;  // handle -> adresse invitée du corps
std::atomic<uint32_t> g_next_handle{kHandleBase + 4};

uint32_t* Word(uint8_t* base, uint32_t guest) { return reinterpret_cast<uint32_t*>(base + guest); }

}  // namespace

// ObCreateObject(type, attributs, tailleCorps, &objet)
REX_HOOK_RAW(__imp__ObCreateObject) {
  const uint32_t type = ctx.r3.u32, size = ctx.r5.u32, out = ctx.r6.u32;
  const uint32_t raw = REX_KERNEL_MEMORY()->SystemHeapAlloc(size + kHeader);
  if (!raw) {
    if (out) *Word(base, out) = 0;
    ctx.r3.u64 = 0xC0000017u;  // STATUS_NO_MEMORY
    return;
  }
  uint8_t* p = base + raw;
  for (uint32_t i = 0; i < size + kHeader; ++i) p[i] = 0;
  *Word(base, raw) = __builtin_bswap32(1);      // compte de pointeurs
  *Word(base, raw + 8) = __builtin_bswap32(type);
  const uint32_t body = raw + kHeader;
  if (out) *Word(base, out) = __builtin_bswap32(body);
  ctx.r3.u64 = 0;
}

// ObInsertObject(objet, attributs, biaisPointeurs, &handle)
REX_HOOK_RAW(__imp__ObInsertObject) {
  const uint32_t body = ctx.r3.u32, out = ctx.r6.u32;
  const uint32_t handle = g_next_handle.fetch_add(4);
  {
    std::lock_guard lock(g_mutex);
    g_handle_to_body[handle] = body;
  }
  if (out) *Word(base, out) = __builtin_bswap32(handle);
  ctx.r3.u64 = 0;
}

// ObReferenceObjectByHandle(handle, type, &objet)
REX_HOOK_RAW(__imp__ObReferenceObjectByHandle) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__ObReferenceObjectByHandle");
  const uint32_t handle = ctx.r3.u32, out = ctx.r5.u32;
  if (handle >= kHandleBase + 4 && handle < kHandleBase + 0x01000000u) {
    uint32_t body = 0;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_handle_to_body.find(handle);
      if (it != g_handle_to_body.end()) body = it->second;
    }
    if (body) {
      if (out) *Word(base, out) = __builtin_bswap32(body);
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
}

// NtClose(handle)
REX_HOOK_RAW(__imp__NtClose) {
  static PPCFunc* sdk = dash::SdkFunction("__imp__NtClose");
  const uint32_t handle = ctx.r3.u32;
  if (handle >= kHandleBase + 4 && handle < kHandleBase + 0x01000000u) {
    std::lock_guard lock(g_mutex);
    if (g_handle_to_body.erase(handle)) {
      ctx.r3.u64 = 0;
      return;
    }
  }
  sdk(ctx, base);
}
