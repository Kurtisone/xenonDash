// native_gpu : « GPU nul ». Le D3D invité écrit ses commandes dans un tampon d'anneau et attend, en scrutant un
// compteur écrit en retour par le GPU (`*(dev+10896)` = adresse de l'identifiant, `dev+10908` = dernière position
// émise), que le GPU ait consommé ses commandes. Ici personne ne lit ces commandes (le rendu natif remplace chaque
// fonction de dessin/état) : un fil publie en permanence « tout est consommé » pour que le code invité ne bloque
// jamais. Les offsets ci-dessous sont ceux de D3D 2.0.5526 lié dans dash.xex 6770.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include "ngpu.h"
#include NGPU_PROFILE_HEADER

namespace ngpu {
namespace {
std::atomic<uint32_t> g_dev{0};
std::atomic<bool> g_started{false};

void Loop() {
  auto* mem = rex::system::kernel_memory();
  for (;;) {
    uint32_t dev = g_dev.load();
    if (dev) {
      auto* d = mem->TranslateVirtual<uint8_t*>(dev);
      uint32_t idp = Be32(d + ngpu_profile::kFencePtr);
      if (idp) {
        auto* id = mem->TranslateVirtual<uint8_t*>(idp);
        PutBe32(id, Be32(d + ngpu_profile::kFenceValue));
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
}  // namespace

// Interruption de retour de balayage (vblank) : le D3D invité compte les images affichées et libère les échanges en
// attente (dev+16532, file des Swap à +16684/+16688) dans son gestionnaire 92121210(dev), appelé en temps normal par le
// GPU à 60 Hz via le rappel enregistré par VdSetGraphicsInterruptCallback. Sans GPU on appelle ce gestionnaire nous-mêmes.
static rex::system::object_ref<rex::system::XHostThread> g_vblank;
static void StartVblank(uint32_t dev) {
  g_vblank = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(rex::system::kernel_state(), 128 * 1024, 0, [dev]() {
        auto next = std::chrono::steady_clock::now();
        for (;;) {
          next += std::chrono::microseconds(16667);
          std::this_thread::sleep_until(next);
          auto* t = rex::system::XThread::GetCurrentThread();
          t->SetActiveCpu(2);
          uint64_t args[] = {dev};
          rex::system::kernel_state()->function_dispatcher()->ExecuteInterrupt(t->thread_state(), ngpu_profile::kVblankHandler, args, 1);
        }
        return 0;
      }));
  g_vblank->set_name("native-vblank");
  g_vblank->Create();
}

// Mémorise le périphérique D3D invité et démarre le fil de complétion (premier appel).
void NullGpuAttach(uint32_t dev) {
  if (!dev) return;
  // D3D n'a pas fini de s'initialiser tant que le pointeur de retour du GPU est nul : ne rien declencher avant.
  if (!Be32(rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(dev) + ngpu_profile::kFencePtr)) return;
  g_dev = dev;
  if (!g_started.exchange(true)) {
    std::thread(Loop).detach();
    if (!std::getenv("DASH_NGPU_NOVBLANK")) StartVblank(dev);
  }
}
// Publie tout de suite la consommation complète (utilisé par les crochets d'attente de fence).
void NullGpuCompleteNow(uint8_t* base, uint32_t dev) {
  NullGpuAttach(dev);
  uint32_t idp = Be32(base + dev + ngpu_profile::kFencePtr);
  if (idp) PutBe32(base + idp, Be32(base + dev + ngpu_profile::kFenceValue));
}
}  // namespace ngpu
