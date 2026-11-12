// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - « GPU nul » generique (games/_generic). Voir native_gpu/src/null_gpu.cpp (Dashboard 6770) et la version de Skate
// (games/45410813/native/null_gpu.cpp) : les adresses et decalages viennent du profil genere par d3d_match.py.
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
#include "xd_project.h"
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

// Attentes de la XDK D3D (WaitFence, WaitDrain, WaitSecond) : crochets generes (adresses retrouvees par d3d_match.py).
#include "d3d_nullgpu_hooks.generated.inc"
