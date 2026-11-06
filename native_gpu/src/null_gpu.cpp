// native_gpu : « GPU nul ». Le D3D invité écrit ses commandes dans un tampon d'anneau et attend, en scrutant un
// compteur écrit en retour par le GPU (`*(dev+10896)` = adresse de l'identifiant, `dev+10908` = dernière position
// émise), que le GPU ait consommé ses commandes. Ici personne ne lit ces commandes (le rendu natif remplace chaque
// fonction de dessin/état) : un fil publie en permanence « tout est consommé » pour que le code invité ne bloque
// jamais. Les offsets ci-dessous sont ceux de D3D 2.0.5526 lié dans dash.xex 6770.

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/system/kernel_state.h>

#include "ngpu.h"

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
      uint32_t idp = Be32(d + 10896);
      if (idp) {
        auto* id = mem->TranslateVirtual<uint8_t*>(idp);
        PutBe32(id, Be32(d + 10908));
      }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}
}  // namespace

// Mémorise le périphérique D3D invité et démarre le fil de complétion (premier appel).
void NullGpuAttach(uint32_t dev) {
  g_dev = dev;
  if (!g_started.exchange(true)) std::thread(Loop).detach();
}
// Publie tout de suite la consommation complète (utilisé par les crochets d'attente de fence).
void NullGpuCompleteNow(uint8_t* base, uint32_t dev) {
  NullGpuAttach(dev);
  uint32_t idp = Be32(base + dev + 10896);
  if (idp) PutBe32(base + idp, Be32(base + dev + 10908));
}
}  // namespace ngpu
