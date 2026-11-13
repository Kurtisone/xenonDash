// xenondash common - garde de KeResetEvent pour XAM recompilé (voir docs/xam_recompile.md, palier H).
//
// Le SDK associe à un KEVENT invité son objet natif par une signature écrite dans l'en-tête (wait_list_flink = 'REX\0',
// wait_list_blink = handle) ; XObject::GetNativeObject fait confiance à ce couple. XAM recompilé réutilise la mémoire
// de ses objets (tâches du fil de répartition, sub_81890650) sans passer par le noyau : l'en-tête peut contenir la
// signature d'un ancien objet dont le handle a été recyclé pour un objet d'un autre type ; le SDK convertit alors
// l'objet en XEvent et XEvent::Reset() déréférence un pointeur invalide (SIGSEGV, 1 démarrage sur 6).
// Ici : avant l'appel du SDK, un en-tête signé dont le handle ne désigne pas un événement est effacé (le SDK recrée alors
// l'objet natif depuis le type de l'en-tête), sous le verrou global du SDK pour ne pas courir avec GetNativeObject.

#include <atomic>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/thread.h>

#include "dash_xam_runtime.h"
#include "kernel_sdk_fn.h"

namespace {

void GuardEvent(const char* api, uint32_t ea, uint8_t* base) {
  if (!ea) return;
  auto* header = reinterpret_cast<rex::system::X_DISPATCH_HEADER*>(base + ea);
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  if (static_cast<uint32_t>(header->wait_list_flink) != rex::system::kXObjSignature) return;
  const uint32_t handle = header->wait_list_blink;
  auto object = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XObject>(handle);
  const bool is_event = object && object->type() == rex::system::XObject::Type::Event;
  if (is_event) return;
  static std::atomic<int> n{0};
  if (n.fetch_add(1) < 10) {
    REXLOG_WARN("[kernel] {} : en-tête signé périmé en 0x{:08X} (handle 0x{:08X} {}), signature effacée", api, ea, handle,
                object ? "d'un autre type" : "absent de la table");
  }
  header->wait_list_flink = 0;
  header->wait_list_blink = 0;
}

}  // namespace

REX_HOOK_RAW(__imp__KeResetEvent) {
  static PPCFunc* fn = dash::SdkFunction("__imp__KeResetEvent");
  if (dash::xamrt::InXamImage(static_cast<uint32_t>(ctx.lr))) GuardEvent("KeResetEvent", ctx.r3.u32, base);
  fn(ctx, base);
}
