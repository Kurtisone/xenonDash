// xenondash common - minuteries du noyau (KeInitializeTimerEx, KeSetTimerEx, KeCancelTimer) pour XAM recompilé.
//
// Le SDK implémente les minuteries par handle (NtCreateTimer, NtSetTimerEx), mais :
// - KeSetTimerEx et KeCancelTimer sont des stubs (r3 inchangé) ;
// - KeInitializeTimerEx ne crée pas l'objet hôte, et XObject::GetNativeObject refuse les objets de type 8/9
//   (minuterie dans la mémoire invitée) : KeWaitForMultipleObjects renvoie alors STATUS_INVALID_PARAMETER.
// Le fil de répartition des tâches de XAM (sub_81890DE8) attend une minuterie et un sémaphore avec
// KeWaitForMultipleObjects : il recevait 0xC000000D en boucle (100 % d'un cœur) et ne distribuait aucune tâche.
// Ici : l'objet hôte (XTimer) est créé à l'initialisation et rattaché à l'en-tête invité, comme le fait
// XObject::StashHandle (signature « REX\0 » dans wait_list_flink, handle dans wait_list_blink).
// DPC : non pris en charge (journalisé), XAM n'en passe pas pour cette minuterie.

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xtimer.h>
#include <rex/types.h>

namespace {

using rex::system::X_DISPATCH_HEADER;
using rex::system::X_KTIMER;
using rex::system::XTimer;

X_KTIMER* TimerAt(uint8_t* base, uint32_t guest) { return reinterpret_cast<X_KTIMER*>(base + guest); }

}  // namespace

// KeInitializeTimerEx(timer, type, processType) : type 0 = notification, 1 = synchronisation (en-tête type 8 ou 9).
REX_HOOK_RAW(__imp__KeInitializeTimerEx) {
  X_KTIMER* t = TimerAt(base, ctx.r3.u32);
  const uint32_t type = ctx.r4.u32 & 1;
  t->header.process_type = static_cast<uint8_t>(ctx.r5.u32 & 0xFF);
  t->header.inserted = 0;
  t->header.type = static_cast<uint8_t>(type + 8);
  t->header.signal_state = 0;
  t->due_time = 0;
  t->period = 0;
  auto timer = rex::system::object_ref<XTimer>(new XTimer(REX_KERNEL_STATE()));
  timer->Initialize(type);
  t->header.wait_list_flink = rex::system::kXObjSignature;
  t->header.wait_list_blink = timer->handle();
}

// KeSetTimerEx(timer, dueTime, period, dpc) : renvoie TRUE si la minuterie était déjà armée.
REX_HOOK_RAW(__imp__KeSetTimerEx) {
  X_KTIMER* t = TimerAt(base, ctx.r3.u32);
  auto timer = rex::system::XObject::GetNativeObject<XTimer>(REX_KERNEL_STATE(), t, t->header.type);
  if (!timer) {
    ctx.r3.u64 = 0;
    return;
  }
  if (ctx.r6.u32) REXLOG_WARN("[kernel] KeSetTimerEx : DPC 0x{:08X} ignoré", ctx.r6.u32);
  const uint8_t was_set = t->header.inserted;
  timer->Cancel();
  timer->SetTimer(static_cast<int64_t>(ctx.r4.s64), ctx.r5.u32, 0, 0, false);
  t->header.inserted = 1;
  ctx.r3.u64 = was_set ? 1 : 0;
}

// KeCancelTimer(timer) : renvoie TRUE si la minuterie était armée.
REX_HOOK_RAW(__imp__KeCancelTimer) {
  X_KTIMER* t = TimerAt(base, ctx.r3.u32);
  auto timer = rex::system::XObject::GetNativeObject<XTimer>(REX_KERNEL_STATE(), t, t->header.type);
  if (!timer) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint8_t was_set = t->header.inserted;
  timer->Cancel();
  t->header.inserted = 0;
  ctx.r3.u64 = was_set ? 1 : 0;
}
