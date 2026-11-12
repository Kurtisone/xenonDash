// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - « GPU nul » de Skate (recette 45410813). Voir native_gpu/src/null_gpu.cpp (Dashboard 6770), dont c'est la
// transposition : les adresses et decalages viennent de d3d_profile.toml ([null_gpu]).
//
// Le D3D invite ecrit ses commandes dans un tampon d'anneau et attend, en scrutant un compteur ecrit en retour par le GPU
// (`*(dev+fence_ptr)` = adresse du bloc de retour, `dev+fence_value` = derniere position emise), que le GPU ait consomme ses
// commandes. Ici personne ne lit ces commandes (le rendu natif remplace chaque fonction de dessin/etat) : un fil publie en
// permanence « tout est consomme » pour que le code invite ne bloque jamais, et un fil de vblank declenche le gestionnaire
// d'interruption graphique 60 fois par seconde (il libere les Swap en attente).

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <rex/hook.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "d3d_profile.generated.h"
#include "generated/default/skate1_init.h"
#include "ngpu.h"

namespace ngpu {
namespace {
std::atomic<uint32_t> g_dev{0};
std::atomic<bool> g_started{false};

void Loop() {
  auto* mem = rex::system::kernel_memory();
  for (;;) {
    const uint32_t dev = g_dev.load();
    if (dev) {
      auto* d = mem->TranslateVirtual<uint8_t*>(dev);
      const uint32_t idp = Be32(d + xd_profile::kNull_fence_ptr);
      if (idp) PutBe32(mem->TranslateVirtual<uint8_t*>(idp), Be32(d + xd_profile::kNull_fence_value));
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

rex::system::object_ref<rex::system::XHostThread> g_vblank;
void StartVblank(uint32_t dev) {
  g_vblank = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(rex::system::kernel_state(), 128 * 1024, 0, [dev]() {
        auto next = std::chrono::steady_clock::now();
        for (;;) {
          next += std::chrono::microseconds(16667);
          std::this_thread::sleep_until(next);
          auto* t = rex::system::XThread::GetCurrentThread();
          t->SetActiveCpu(2);
          uint64_t args[] = {dev};
          rex::system::kernel_state()->function_dispatcher()->ExecuteInterrupt(t->thread_state(), xd_profile::kNull_vblank_handler,
                                                                               args, 1);
        }
        return 0;
      }));
  g_vblank->set_name("native-vblank");
  g_vblank->Create();
}
}  // namespace

// Memorise le peripherique D3D invite et demarre les fils (premier appel avec un peripherique initialise).
void NullGpuAttach(uint32_t dev, uint8_t* base) {
  if (!dev || !base) return;
  // D3D n'a pas fini de s'initialiser tant que le pointeur de retour est nul : ne rien declencher avant.
  if (!Be32(base + dev + xd_profile::kNull_fence_ptr)) return;
  g_dev = dev;
  if (!g_started.exchange(true)) {
    std::thread(Loop).detach();
    StartVblank(dev);
  }
}

void NullGpuCompleteNow(uint8_t* base, uint32_t dev) {
  NullGpuAttach(dev, base);
  const uint32_t idp = Be32(base + dev + xd_profile::kNull_fence_ptr);
  if (idp) PutBe32(base + idp, Be32(base + dev + xd_profile::kNull_fence_value));
}
}  // namespace ngpu

// Attentes de la XDK D3D : on publie la completion puis on laisse la fonction d'origine (qui ne boucle plus).
// Les trois fonctions sont celles du D3D du Dashboard retrouvees dans Skate (meme code, memes decalages).
REX_HOOK_RAW(sub_82CB11C0) {  // wait_fence(dev, fence, ...)
  ngpu::NullGpuCompleteNow(base, ctx.r3.u32);
  __imp__sub_82CB11C0(ctx, base);
}
REX_HOOK_RAW(sub_82CB0060) {  // wait_drain(dev, pos, size) : le pointeur de lecture du GPU sort de la zone demandee
  const uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  const uint32_t idp = ngpu::Be32(base + dev + xd_profile::kNull_fence_ptr);
  if (idp) ngpu::PutBe32(base + idp + xd_profile::kNull_drain_ptr_word, ctx.r4.u32);
  __imp__sub_82CB0060(ctx, base);
}
REX_HOOK_RAW(sub_82CAFF88) {  // wait_second(dev, a, b) : fence secondaire
  const uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  const uint32_t idp = ngpu::Be32(base + dev + xd_profile::kNull_fence_ptr);
  if (idp) ngpu::PutBe32(base + idp + xd_profile::kNull_second_word, ctx.r5.u32);
  __imp__sub_82CAFF88(ctx, base);
}
