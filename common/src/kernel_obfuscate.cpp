// xenondash common - XeKeysObfuscate / XeKeysUnObfuscate locaux (palier N4).
//
// Sur la console, ces deux fonctions du noyau protègent le fichier `Account` d'un profil avec une clé propre à la console
// (XAM : sub_81864C38, 380 octets en clair -> 404 octets sur disque). Le SDK les laisse en stub : le fichier recevait des octets
// de pile. Il n'y a ici NI clé de console NI chiffrement : le format est LOCAL, sans valeur de sécurité, et illisible par une
// console (aucun contenu protégé n'est lu ni produit). Sortie = en-tête de 24 octets (étiquette « XDASHOBF », longueur, CRC-32,
// remplissage) suivi des données telles quelles ; l'en-tête permet à XeKeysUnObfuscate de refuser un fichier qui n'en vient pas.
// Même rallonge de 24 octets que la console (380 -> 404), pour que les tailles attendues par XAM restent les mêmes.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace {

constexpr uint32_t kHeader = 24;
constexpr char kMagic[8] = {'X', 'D', 'A', 'S', 'H', 'O', 'B', 'F'};

uint32_t Crc32(const uint8_t* p, uint32_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
uint32_t GetBe32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

}  // namespace

// XeKeysObfuscate(type, entrée, tailleEntrée, sortie, &tailleSortie) : renvoie 1 (BOOL).
REX_HOOK_RAW(__imp__XeKeysObfuscate) {
  const uint32_t in = ctx.r4.u32, in_size = ctx.r5.u32, out = ctx.r6.u32, out_size = ctx.r7.u32;
  if (!in || !out || !out_size) {
    ctx.r3.u64 = 0;
    return;
  }
  uint8_t* o = base + out;
  std::memset(o, 0, kHeader);
  std::memcpy(o, kMagic, sizeof kMagic);
  PutBe32(o + 8, in_size);
  PutBe32(o + 12, Crc32(base + in, in_size));
  std::memmove(o + kHeader, base + in, in_size);
  *reinterpret_cast<uint32_t*>(base + out_size) = __builtin_bswap32(in_size + kHeader);
  REXLOG_INFO("[xekeys] XeKeysObfuscate local : {} -> {} octets (sans clé de console)", in_size, in_size + kHeader);
  ctx.r3.u64 = 1;
}

// XeKeysUnObfuscate(type, entrée, tailleEntrée, sortie, &tailleSortie) : 0 si l'entrée ne vient pas de XeKeysObfuscate local.
REX_HOOK_RAW(__imp__XeKeysUnObfuscate) {
  const uint32_t in = ctx.r4.u32, in_size = ctx.r5.u32, out = ctx.r6.u32, out_size = ctx.r7.u32;
  if (!in || !out || !out_size || in_size < kHeader || std::memcmp(base + in, kMagic, sizeof kMagic) != 0) {
    ctx.r3.u64 = 0;
    return;
  }
  const uint8_t* i = base + in;
  const uint32_t n = GetBe32(i + 8);
  if (n != in_size - kHeader || Crc32(i + kHeader, n) != GetBe32(i + 12)) {
    ctx.r3.u64 = 0;
    return;
  }
  std::memmove(base + out, i + kHeader, n);
  *reinterpret_cast<uint32_t*>(base + out_size) = __builtin_bswap32(n);
  ctx.r3.u64 = 1;
}
