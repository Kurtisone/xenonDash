// Changement de fibre interne de XAM (sub_819BF2E0, appelé seulement par sub_819BF160) : vraie bascule de pile hôte.
//
// Sur la console, sub_819BF2E0 sauvegarde le contexte de la fibre courante (KTHREAD+356), recharge celui de la fibre cible
// (r3 : r14..r31 à +168, f14..f31, v64..v127, CR à +312, LR à +44, r1 à +64), marque la cible courante puis saute à
// KeSetCurrentStackPointers, dont le blr part sur le LR de la cible. Recompilé, ce saut est un appel C++ suivi de `return` :
// le contrôle revient sur la pile HÔTE de l'appelant (sub_819BF160) alors que r1 et r27..r31 sont déjà ceux de la cible ;
// l'épilogue __restgprlr_27 relit des zéros sur la pile invitée de la cible (fibre neuve) et la boucle de sub_819BE610 tourne
// sur l'adresse 0 (fil à 100 % CPU).
//
// Correction : chaque fibre de XAM reçoit une fibre HÔTE (rex::thread::Fiber du SDK, ucontext sous Linux, comme le fait le SDK
// pour CreateFiber/SwitchToFiber). L'original (__imp__) fait toujours tout l'échange des registres invités ; ensuite on
// suspend la pile hôte de la fibre sortante et on reprend celle de la cible :
//  - cible déjà suspendue dans cette fonction : Fiber::SwitchTo, elle reprend après son propre SwitchTo et revient
//    normalement à son sub_819BF160 (son LR invité vaut justement 0x819BF248) ;
//  - cible neuve (LR invité = 0x819BF2D0, posé par sub_819BEEE8 à la création) : nouvelle fibre hôte qui appelle
//    sub_819BF2D0 avec le contexte invité déjà chargé (r1 = pile de la fibre, r31 = routine de départ).
// La fibre sortante est associée à la fibre hôte en cours (Fiber::Current, la fibre principale du XThread pour la fibre
// de thread de XAM). sub_819BF0A8 (suppression d'une fibre) libère la fibre hôte qu'on avait créée.
// Les deux fonctions passent toujours par hudd3d_call (crochets D3D de XAM). DASH_XAM_FIBER_FIX=0 rétablit le comportement généré (diagnostic).

#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/thread/fiber.h>

extern "C" void __imp__sub_819BF2E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_819BF0A8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_819BF2D0(PPCContext& __restrict, uint8_t*);

namespace {

using rex::thread::Fiber;

constexpr uint32_t kFiberStartLr = 0x819BF2D0;   // trampoline de départ (sub_819BF2D0)
constexpr size_t kHostStackSize = 2u * 1024 * 1024;  // pile hôte d'une fibre de XAM

struct HostFiber {
  Fiber* fiber;
  bool owned;  // créée ici (sinon : fibre principale d'un XThread, jamais détruite ici)
};

std::mutex g_mutex;
std::unordered_map<uint32_t, HostFiber> g_fibers;  // objet fibre invité -> fibre hôte

struct StartArgs {
  PPCContext* ctx;
  uint8_t* base;
  uint32_t guest_fiber;
};

bool FixOn() {
  static const bool on = [] {
    const char* e = std::getenv("DASH_XAM_FIBER_FIX");
    return !(e && e[0] == '0');
  }();
  return on;
}

uint32_t LoadBE32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

void FiberEntry(void* raw) {
  StartArgs a = *static_cast<StartArgs*>(raw);
  delete static_cast<StartArgs*>(raw);
  // Le contexte invité de la cible a été chargé par __imp__sub_819BF2E0 avant la bascule hôte.
  sub_819BF2D0(*a.ctx, a.base);
  // sub_819BF2D0 se termine par KeBugCheck : une fibre de XAM ne revient jamais.
  REXLOG_ERROR("[fibre] la fibre XAM {:08X} est revenue de sa routine de départ", a.guest_fiber);
  std::abort();
}

void FiberSwitch(PPCContext& ctx, uint8_t* base) {
  if (!FixOn()) {
    __imp__sub_819BF2E0(ctx, base);
    return;
  }
  const uint32_t kthread = LoadBE32(base, ctx.r13.u32 + 256);
  const uint32_t current = LoadBE32(base, kthread + 356);
  const uint32_t target = ctx.r3.u32;
  const uint32_t target_lr = LoadBE32(base, target + 44);

  Fiber* self = Fiber::Current();
  if (!self) self = Fiber::ConvertCurrentThread();

  Fiber* next = nullptr;
  StartArgs* start = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto cur = g_fibers.find(current);
    if (cur == g_fibers.end() || cur->second.fiber != self) g_fibers[current] = HostFiber{self, false};

    auto it = g_fibers.find(target);
    if (target_lr == kFiberStartLr) {
      // Fibre neuve (ou objet réutilisé) : l'ancienne fibre hôte éventuelle n'est plus atteignable.
      if (it != g_fibers.end() && it->second.owned && it->second.fiber != self) it->second.fiber->Destroy();
      start = new StartArgs{&ctx, base, target};
      next = Fiber::Create(kHostStackSize, FiberEntry, start);
      if (!next) {
        delete start;
        REXLOG_ERROR("[fibre] création de la fibre hôte impossible pour la fibre XAM {:08X}", target);
        if (it != g_fibers.end()) g_fibers.erase(it);
      } else {
        g_fibers[target] = HostFiber{next, true};
      }
    } else if (it != g_fibers.end()) {
      next = it->second.fiber;
    } else {
      REXLOG_WARN("[fibre] fibre XAM {:08X} inconnue (lr={:08X}) : bascule invitée seule", target, target_lr);
    }
  }

  // Échange des registres invités (sauvegarde de la courante, chargement de la cible, KeSetCurrentStackPointers).
  __imp__sub_819BF2E0(ctx, base);
  if (!next || next == self) return;
  Fiber::SwitchTo(next);
  // Repris : la fibre qui revient vers nous a déjà rechargé notre contexte invité (LR = retour dans sub_819BF160).
}

void FiberDelete(PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_fiber = ctx.r3.u32;
  __imp__sub_819BF0A8(ctx, base);
  if (ctx.r3.u32 != 0 || !guest_fiber) return;  // refus (fibre courante, paramètre nul...)
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_fibers.find(guest_fiber);
  if (it == g_fibers.end()) return;
  if (it->second.owned && it->second.fiber != Fiber::Current()) it->second.fiber->Destroy();
  g_fibers.erase(it);
}

}  // namespace

// Ces deux adresses font partie de la plage crochetée par src_xam_d3d (xam_d3d_entries.inc, où elles sont retirées) :
// on garde le passage par hudd3d_call, avec la version corrigée comme « original ».
extern "C" void hudd3d_call(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*));
extern "C" void sub_819BF2E0(PPCContext& __restrict ctx, uint8_t* base) { hudd3d_call(0x819BF2E0, ctx, base, FiberSwitch); }
extern "C" void sub_819BF0A8(PPCContext& __restrict ctx, uint8_t* base) { hudd3d_call(0x819BF0A8, ctx, base, FiberDelete); }
