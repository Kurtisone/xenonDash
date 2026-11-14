// xenondash common - XeCryptMd5 (palier N4).
//
// Le SDK laisse XeCryptMd5 en stub (sortie non écrite). XAM en dérive le nom du périphérique d'un paquet de contenu
// (`\Device\Package_<MD5 du chemin>`) : avec la sortie d'origine (octets 0xBE de la pile) tous les paquets portaient le même nom et
// le compte de l'énumération de profils échouait au redémarrage. MD5 standard (RFC 1321), sans dépendance.
// XeCryptMd5(entrée1, taille1, entrée2, taille2, entrée3, taille3, sortie, tailleSortie) : même forme que XeCryptSha.

#include <cstdint>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/ppc/context.h>

namespace {

uint32_t Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void Md5Block(uint32_t st[4], const uint8_t* p) {
  static const uint32_t K[64] = {
      0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
      0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
      0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
      0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
      0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
      0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
  static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
  uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
  for (int i = 0; i < 64; ++i) {
    uint32_t f;
    int g;
    if (i < 16) { f = (b & c) | (~b & d); g = i; }
    else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
    else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
    else { f = c ^ (b | ~d); g = (7 * i) % 16; }
    const uint32_t tmp = d;
    d = c;
    c = b;
    b = b + Rotl(a + f + K[i] + m[g], S[i]);
    a = tmp;
  }
  st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

}  // namespace

REX_HOOK_RAW(__imp__XeCryptMd5) {
  const uint32_t in[3] = {ctx.r3.u32, ctx.r5.u32, ctx.r7.u32};
  const uint32_t len[3] = {ctx.r4.u32, ctx.r6.u32, ctx.r8.u32};
  const uint32_t out = ctx.r9.u32, out_size = ctx.r10.u32;
  std::vector<uint8_t> data;
  for (int i = 0; i < 3; ++i)
    if (in[i] && len[i]) data.insert(data.end(), base + in[i], base + in[i] + len[i]);
  const uint64_t bits = uint64_t(data.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 0; i < 8; ++i) data.push_back(uint8_t(bits >> (8 * i)));
  uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
  for (size_t o = 0; o < data.size(); o += 64) Md5Block(st, data.data() + o);
  uint8_t digest[16];
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 4; ++k) digest[i * 4 + k] = uint8_t(st[i] >> (8 * k));
  if (out) std::memcpy(base + out, digest, out_size < 16 ? out_size : 16);
}
