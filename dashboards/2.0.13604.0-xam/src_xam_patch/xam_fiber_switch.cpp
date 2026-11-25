// Changement de fibre interne de XAM (sub_819BF2E0, appelé seulement par sub_819BF160) : vraie bascule de pile hôte.
//
// Sur la console, sub_819BF2E0 sauvegarde le contexte de la fibre courante (KTHREAD+356), recharge celui de la fibre cible
// (r3 : r14..r31 à +168, f14..f31, v64..v127, CR à +312, LR à +44, r1 à +64), marque la cible courante puis saute à
// KeSetCurrentStackPointers, dont le blr part sur le LR de la cible. Recompilé, ce saut est un appel C++ suivi de `return` :
// le contrôle revient sur la pile HÔTE de l'appelant (sub_819BF160) alors que r1 et r27..r31 sont déjà ceux de la cible ;
// l'épilogue __restgprlr_27 relit des zéros sur la pile invitée de la cible (fibre neuve) et la boucle de sub_819BE610 tourne
// sur l'adresse 0 (fil à 100 % CPU).
//
// Correction : chaque fibre de XAM reçoit une fibre HÔTE (ucontext : getcontext/makecontext/swapcontext, comme le backend
// POSIX de rex::thread::Fiber). L'original (__imp__) fait toujours tout l'échange des registres invités ; ensuite on suspend
// la pile hôte de la fibre sortante et on reprend celle de la cible :
//  - cible déjà suspendue dans cette fonction : swapcontext, elle reprend après son propre swapcontext et revient
//    normalement à son sub_819BF160 (son LR invité vaut justement 0x819BF248) ;
//  - cible neuve (LR invité = 0x819BF2D0, posé par sub_819BEEE8 à la création) : nouvelle fibre hôte qui appelle
//    sub_819BF2D0 avec le contexte invité déjà chargé (r1 = pile de la fibre, r31 = routine de départ).
// La fibre sortante est associée à la fibre hôte en cours sur ce fil (ou au contexte natif du fil s'il n'en exécute aucune).
// sub_819BF0A8 (suppression d'une fibre) libère la fibre hôte qu'on avait créée.
//
// Pourquoi pas rex::thread::Fiber : son backend POSIX alloue la pile dans un std::vector (fiber_posix.cpp, SDK 69764e9),
// donc sans page de garde (un débordement écrase le tas en silence) et entièrement engagée (le vector la remplit de zéros).
// Ici la pile est un mmap de kHostStackSize octets réservés sans engagement (MAP_NORESERVE : seules les pages touchées
// comptent dans le RSS) avec une page PROT_NONE en bas : un débordement donne un SIGSEGV immédiat. On ne touche pas non plus
// à Fiber::tls_current_ du SDK (CreateFiber/SwitchToFiber invités restent indépendants).
//
// Garde-fous (chacun retombe sur l'ancien comportement « bascule invitée seule » avec un WARN, comme une fibre inconnue) :
//  - fil propriétaire : chaque fibre hôte note le pthread qui l'a créée et qui l'exécute (mis à jour à chaque bascule).
//    Un contexte ucontext ne peut être repris que sur le fil où il a été suspendu (TLS, fs_base, pile du fil) ; si XAM
//    demande une bascule vers une fibre suspendue sur un autre fil, on refuse la bascule hôte (« bascule depuis un autre
//    fil ») plutôt que de reprendre la pile d'un fil sur un autre. Sur la console c'est permis mais XAM ne le fait pas
//    pour ses fibres de travail : si le message apparaît, c'est un cas à étudier, pas à masquer.
//  - fibre active : indicateur atomique posé pendant qu'une fibre hôte s'exécute ; il n'est relâché qu'une fois la fibre
//    sortante entièrement sauvegardée (après swapcontext, côté fibre reprise). Une bascule vers une fibre déjà active
//    est refusée.
// Les deux fonctions passent toujours par hudd3d_call (crochets D3D de XAM). DASH_XAM_FIBER_FIX=0 rétablit le comportement généré (diagnostic).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include <pthread.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

extern "C" void __imp__sub_819BF2E0(PPCContext& __restrict, uint8_t*);
extern "C" void __imp__sub_819BF0A8(PPCContext& __restrict, uint8_t*);
extern "C" void sub_819BF2D0(PPCContext& __restrict, uint8_t*);

namespace {

constexpr uint32_t kFiberStartLr = 0x819BF2D0;          // trampoline de départ (sub_819BF2D0)
constexpr size_t kHostStackSize = 16u * 1024 * 1024;    // pile hôte réservée (non engagée) d'une fibre de XAM

struct HostFiber {
  ucontext_t context{};
  uint8_t* mapping = nullptr;  // pile mmap (page de garde comprise) ; nul pour le contexte natif d'un fil
  size_t mapping_size = 0;
  bool owned = false;           // créée ici (sinon : contexte natif d'un fil, jamais détruit ici)
  pthread_t owner{};            // fil qui l'exécute ou l'a suspendue
  std::atomic<bool> active{false};
  // Départ d'une fibre neuve.
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint32_t guest_fiber = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, HostFiber*> g_fibers;  // objet fibre invité -> fibre hôte

thread_local HostFiber* t_current = nullptr;  // fibre hôte en cours sur ce fil (nul : contexte natif jamais vu)
thread_local HostFiber* t_release = nullptr;  // fibre sortante à marquer inactive une fois reprise ailleurs

bool FixOn() {
  static const bool on = [] {
    const char* e = std::getenv("DASH_XAM_FIBER_FIX");
    return !(e && e[0] == '0');
  }();
  return on;
}

// Limite les messages répétés (une bascule refusée se répète à chaque image).
bool ShouldLog(std::atomic<uint32_t>& n) {
  const uint32_t c = n.fetch_add(1, std::memory_order_relaxed);
  return c < 16 || (c % 10000) == 0;
}
std::atomic<uint32_t> g_log_unknown{0}, g_log_thread{0}, g_log_active{0};

uint32_t LoadBE32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

// Appelé juste après chaque reprise d'une fibre hôte : la fibre sortante est entièrement sauvegardée.
void ReleasePrevious() {
  if (HostFiber* prev = t_release) {
    t_release = nullptr;
    prev->active.store(false, std::memory_order_release);
  }
}

HostFiber* CurrentHostFiber() {
  if (!t_current) {
    auto* f = new HostFiber();  // contexte natif du fil : une par fil, jamais libérée (quelques octets)
    f->owner = pthread_self();
    f->active.store(true, std::memory_order_relaxed);
    t_current = f;
  }
  return t_current;
}

// Libère une fibre hôte créée ici si plus rien ne la référence ni ne l'exécute (g_mutex tenu, déjà retirée de la table).
void ReleaseIfUnused(HostFiber* f) {
  if (!f->owned || f == t_current || f->active.load(std::memory_order_acquire)) return;
  for (const auto& [guest, host] : g_fibers)
    if (host == f) return;
  if (f->mapping) munmap(f->mapping, f->mapping_size);
  delete f;
}

void DestroyHostFiber(HostFiber* f) {
  if (f->mapping) munmap(f->mapping, f->mapping_size);
  delete f;
}

void FiberEntry() {
  ReleasePrevious();
  HostFiber* f = t_current;
  // Le contexte invité de la cible a été chargé par __imp__sub_819BF2E0 avant la bascule hôte.
  sub_819BF2D0(*f->ctx, f->base);
  // sub_819BF2D0 se termine par KeBugCheck : une fibre de XAM ne revient jamais.
  REXLOG_ERROR("[fibre] la fibre XAM {:08X} est revenue de sa routine de départ", f->guest_fiber);
  std::abort();
}

HostFiber* CreateHostFiber(PPCContext& ctx, uint8_t* base, uint32_t guest_fiber) {
  const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  const size_t size = kHostStackSize + page;
  void* m = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
  if (m == MAP_FAILED) return nullptr;
  if (mprotect(m, page, PROT_NONE) != 0) {  // page de garde en bas (la pile descend)
    munmap(m, size);
    return nullptr;
  }
  auto* f = new HostFiber();
  f->mapping = static_cast<uint8_t*>(m);
  f->mapping_size = size;
  f->owned = true;
  f->owner = pthread_self();
  f->ctx = &ctx;
  f->base = base;
  f->guest_fiber = guest_fiber;
  if (getcontext(&f->context) == -1) {
    DestroyHostFiber(f);
    return nullptr;
  }
  f->context.uc_stack.ss_sp = f->mapping + page;
  f->context.uc_stack.ss_size = kHostStackSize;
  f->context.uc_link = nullptr;
  makecontext(&f->context, &FiberEntry, 0);
  return f;
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
  const pthread_t me = pthread_self();

  HostFiber* self = CurrentHostFiber();
  self->owner = me;

  HostFiber* next = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto cur = g_fibers.find(current);
    if (cur == g_fibers.end()) {
      g_fibers[current] = self;
    } else if (cur->second != self) {
      // Association périmée (après une bascule invitée seule) : la fibre invitée courante s'exécute sur self. L'ancienne
      // fibre hôte n'est pas libérée ici (elle peut être encore référencée) ; elle le sera par sub_819BF0A8 ou jamais.
      cur->second = self;
    }

    auto it = g_fibers.find(target);
    if (target_lr == kFiberStartLr) {
      // Fibre neuve (ou objet réutilisé) : l'ancienne fibre hôte éventuelle n'est plus atteignable.
      if (it != g_fibers.end()) {
        HostFiber* old = it->second;
        g_fibers.erase(it);
        if (old != self) ReleaseIfUnused(old);
      }
      next = CreateHostFiber(ctx, base, target);
      if (!next) {
        REXLOG_ERROR("[fibre] création de la fibre hôte impossible pour la fibre XAM {:08X} : bascule invitée seule", target);
      } else {
        g_fibers[target] = next;
      }
    } else if (it != g_fibers.end()) {
      next = it->second;
      if (next != self && !pthread_equal(next->owner, me)) {
        if (ShouldLog(g_log_thread))
          REXLOG_WARN("[fibre] fibre XAM {:08X} : bascule depuis un autre fil, bascule invitée seule", target);
        next = nullptr;
      }
    } else {
      if (ShouldLog(g_log_unknown))
        REXLOG_WARN("[fibre] fibre XAM {:08X} inconnue (lr={:08X}) : bascule invitée seule", target, target_lr);
    }

    if (next && next != self) {
      bool expected = false;
      if (!next->active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        if (ShouldLog(g_log_active))
          REXLOG_WARN("[fibre] fibre XAM {:08X} déjà active : bascule invitée seule", target);
        next = nullptr;
      }
    }
  }

  // Échange des registres invités (sauvegarde de la courante, chargement de la cible, KeSetCurrentStackPointers).
  __imp__sub_819BF2E0(ctx, base);
  if (!next || next == self) return;
  next->owner = me;
  t_current = next;
  t_release = self;
  swapcontext(&self->context, &next->context);
  // Repris (sur ce même fil, voir le garde-fou du fil propriétaire) : la fibre qui revient vers nous a déjà rechargé
  // notre contexte invité (LR = retour dans sub_819BF160).
  ReleasePrevious();
  t_current = self;
}

void FiberDelete(PPCContext& ctx, uint8_t* base) {
  const uint32_t guest_fiber = ctx.r3.u32;
  __imp__sub_819BF0A8(ctx, base);
  if (ctx.r3.u32 != 0 || !guest_fiber) return;  // refus (fibre courante, paramètre nul...)
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_fibers.find(guest_fiber);
  if (it == g_fibers.end()) return;
  HostFiber* f = it->second;
  g_fibers.erase(it);
  ReleaseIfUnused(f);
}

}  // namespace

// Ces deux adresses font partie de la plage crochetée par src_xam_d3d (xam_d3d_entries.inc, où elles sont retirées) :
// on garde le passage par hudd3d_call, avec la version corrigée comme « original ».
extern "C" void hudd3d_call(uint32_t addr, PPCContext& ctx, uint8_t* base, void (*orig)(PPCContext&, uint8_t*));
extern "C" void sub_819BF2E0(PPCContext& __restrict ctx, uint8_t* base) { hudd3d_call(0x819BF2E0, ctx, base, FiberSwitch); }
extern "C" void sub_819BF0A8(PPCContext& __restrict ctx, uint8_t* base) { hudd3d_call(0x819BF0A8, ctx, base, FiberDelete); }
