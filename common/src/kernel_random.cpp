// xenondash common - XeCryptRandom : octets aléatoires (le SDK écrit 0xFD partout).
//
// XAM recompilé tire des nombres aléatoires par échantillonnage avec rejet (sub_81A71EE8 : tirer un nombre dans
// [0, borne], recommencer s'il dépasse la borne). Avec la sortie constante 0xFDFDFDFD du SDK, le tirage dépasse toujours
// la borne 0x89ABCDEF et la boucle ne se termine jamais : c'est la cause du démarrage sans fin de l'application 0xFA
// (musicplayer.xex) de XAM (palier G). Ici : octets du générateur du système (getrandom). Ce n'est pas une clé de console.

#include <cstdint>
#include <cstring>
#include <random>

#if !defined(_WIN32)
#include <sys/random.h>
#endif

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

// XeCryptRandom(PBYTE tampon, DWORD taille)
REX_HOOK_RAW(__imp__XeCryptRandom) {
  const uint32_t buf = ctx.r3.u32, size = ctx.r4.u32;
  if (!buf || !size) return;
  uint8_t* dst = base + buf;
#if !defined(_WIN32)
  size_t done = 0;
  while (done < size) {
    const ssize_t n = getrandom(dst + done, size - done, 0);
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  if (done == size) return;
#endif
  static thread_local std::mt19937 rng{std::random_device{}()};
  for (uint32_t i = 0; i < size; ++i) dst[i] = static_cast<uint8_t>(rng());
}
