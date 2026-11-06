#include "guest_tex.h"

#include <algorithm>
#include <cstring>

#include "ngpu.h"

namespace ngpu {
namespace {

// Formats Xenos (TextureFormat)
enum : uint32_t { k_1_REVERSE = 0, k_1 = 1, k_8 = 2, k_1_5_5_5 = 3, k_5_6_5 = 4, k_6_5_5 = 5, k_8_8_8_8 = 6, k_2_10_10_10 = 7, k_8_A = 8, k_8_B = 9, k_8_8 = 10,
                  k_DXT1 = 18, k_DXT2_3 = 19, k_DXT4_5 = 20, k_16 = 24, k_16_16 = 25, k_DXN = 49 };

int BytesPerBlock(uint32_t f, bool* block) {
  *block = false;
  switch (f) {
    case k_8: case k_8_A: case k_8_B: return 1;
    case k_1_5_5_5: case k_5_6_5: case k_6_5_5: case k_8_8: case k_16: return 2;
    case k_8_8_8_8: case k_2_10_10_10: case k_16_16: return 4;
    case k_DXT1: *block = true; return 8;
    case k_DXT2_3: case k_DXT4_5: case k_DXN: *block = true; return 16;
    default: return 0;
  }
}

// Décalage (en éléments) d'un élément (x, y) dans une surface 2D tuilée (algorithme de désentrelacement Xenos).
uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t width_aligned, uint32_t log2_bpb) {
  uint32_t macro = ((x >> 5) + (y >> 5) * (width_aligned >> 5)) << (log2_bpb + 7);
  uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bpb;
  uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F))) >> log2_bpb;
}

uint32_t Log2(uint32_t v) { uint32_t r = 0; while ((1u << r) < v) r++; return r; }

void DecodeDxtColor(const uint8_t* b, uint8_t out[4][4], bool dxt1_alpha) {
  uint16_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
  auto expand = [](uint16_t c, uint8_t* o) {
    o[0] = ((c >> 11) & 31) * 255 / 31; o[1] = ((c >> 5) & 63) * 255 / 63; o[2] = (c & 31) * 255 / 31; o[3] = 255;
  };
  expand(c0, out[0]); expand(c1, out[1]);
  if (c0 > c1 || !dxt1_alpha) {
    for (int i = 0; i < 3; i++) { out[2][i] = (2 * out[0][i] + out[1][i]) / 3; out[3][i] = (out[0][i] + 2 * out[1][i]) / 3; }
    out[2][3] = out[3][3] = 255;
  } else {
    for (int i = 0; i < 3; i++) { out[2][i] = (out[0][i] + out[1][i]) / 2; out[3][i] = 0; }
    out[2][3] = 255; out[3][3] = 0;
  }
}

}  // namespace

TexInfo DescribeTexture(const FetchConst& f) {
  TexInfo t;
  t.width = (f.d[2] & 0x1FFF) + 1;
  t.height = ((f.d[2] >> 13) & 0x1FFF) + 1;
  t.format = f.d[1] & 0x3F;
  t.endian = (f.d[1] >> 6) & 3;
  t.pitch = ((f.d[0] >> 22) & 0x1FF) * 32;
  t.tiled = (f.d[0] >> 31) & 1;
  uint32_t phys = ((f.d[1] >> 12) & 0xFFFFF) << 12;
  t.base = 0xA0000000u | (phys & 0x1FFFFFFF);
  bool blk;
  t.ok = BytesPerBlock(t.format, &blk) != 0 && t.width <= 4096 && t.height <= 4096;
  return t;
}

size_t TextureDataSize(const TexInfo& t) {
  bool blk; int bpb = BytesPerBlock(t.format, &blk);
  if (!bpb) return 0;
  uint32_t w = blk ? (t.width + 3) / 4 : t.width, h = blk ? (t.height + 3) / 4 : t.height;
  if (t.tiled) { w = (w + 31) & ~31u; h = (h + 31) & ~31u; }
  else { uint32_t pw = blk ? t.pitch / 4 : t.pitch; w = std::max(w, pw); }
  return (size_t)w * h * bpb;
}

bool DecodeTexture(const uint8_t* data, const FetchConst& f, const TexInfo& t, std::vector<uint8_t>& rgba) {
  bool blk; int bpb = BytesPerBlock(t.format, &blk);
  if (!bpb) return false;
  const uint32_t w = t.width, h = t.height;
  const uint32_t bw = blk ? (w + 3) / 4 : w, bh = blk ? (h + 3) / 4 : h;
  const uint32_t lg = Log2(bpb);
  uint32_t row_elems = blk ? t.pitch / 4 : t.pitch;
  if (row_elems < bw) row_elems = bw;
  const uint32_t aligned_w = (bw + 31) & ~31u;
  const uint8_t* src = data;
  const size_t total = TextureDataSize(t);
  // copie locale échangée (endianness) : lecture élément par élément
  std::vector<uint8_t> raw(total);
  std::memcpy(raw.data(), src, total);
  switch (t.endian) {
    case 1: for (size_t i = 0; i + 1 < total; i += 2) std::swap(raw[i], raw[i + 1]); break;
    case 2: for (size_t i = 0; i + 3 < total; i += 4) { std::swap(raw[i], raw[i + 3]); std::swap(raw[i + 1], raw[i + 2]); } break;
    case 3: for (size_t i = 0; i + 3 < total; i += 4) { std::swap(raw[i], raw[i + 2]); std::swap(raw[i + 1], raw[i + 3]); } break;
    default: break;
  }
  auto elem = [&](uint32_t ex, uint32_t ey) -> const uint8_t* {
    size_t idx = t.tiled ? TiledOffset2D(ex, ey, aligned_w, lg) : (size_t)ey * row_elems + ex;
    size_t off = idx * bpb;
    return off + bpb <= raw.size() ? raw.data() + off : nullptr;
  };
  std::vector<uint8_t> comp((size_t)w * h * 4, 0);  // composantes x,y,z,w après décodage, avant swizzle
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      uint8_t* o = &comp[((size_t)y * w + x) * 4];
      if (!blk) {
        const uint8_t* p = elem(x, y);
        if (!p) continue;
        switch (t.format) {
          case k_8_8_8_8: o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3]; break;
          case k_8: case k_8_A: case k_8_B: o[0] = p[0]; o[1] = 0; o[2] = 0; o[3] = 255; break;
          case k_8_8: o[0] = p[0]; o[1] = p[1]; o[2] = 0; o[3] = 255; break;
          case k_5_6_5: { uint16_t v = p[0] | (p[1] << 8); o[0] = (v & 31) * 255 / 31; o[1] = ((v >> 5) & 63) * 255 / 63; o[2] = ((v >> 11) & 31) * 255 / 31; o[3] = 255; } break;
          case k_1_5_5_5: { uint16_t v = p[0] | (p[1] << 8); o[0] = (v & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = ((v >> 10) & 31) * 255 / 31; o[3] = (v >> 15) ? 255 : 0; } break;
          case k_16: o[0] = p[1]; o[1] = 0; o[2] = 0; o[3] = 255; break;
          case k_16_16: o[0] = p[1]; o[1] = p[3]; o[2] = 0; o[3] = 255; break;
          default: break;
        }
      } else {
        const uint8_t* p = elem(x >> 2, y >> 2);
        if (!p) continue;
        uint8_t pal[4][4];
        uint32_t px = x & 3, py = y & 3;
        if (t.format == k_DXT1) {
          DecodeDxtColor(p, pal, true);
          uint32_t idx = (p[4 + py] >> (px * 2)) & 3;
          // les composantes sortent déjà en R,G,B,A naturel (pas d'inversion de composantes pour DXT)
          o[0] = pal[idx][0]; o[1] = pal[idx][1]; o[2] = pal[idx][2]; o[3] = pal[idx][3];
        } else if (t.format == k_DXT2_3) {
          DecodeDxtColor(p + 8, pal, false);
          uint32_t idx = (p[8 + 4 + py] >> (px * 2)) & 3;
          uint32_t a = (p[(py * 2) + (px >> 1)] >> ((px & 1) * 4)) & 15;
          o[0] = pal[idx][0]; o[1] = pal[idx][1]; o[2] = pal[idx][2]; o[3] = a * 17;
        } else if (t.format == k_DXT4_5) {
          DecodeDxtColor(p + 8, pal, false);
          uint32_t idx = (p[8 + 4 + py] >> (px * 2)) & 3;
          uint8_t a0 = p[0], a1 = p[1];
          uint64_t bits = 0; for (int i = 0; i < 6; i++) bits |= (uint64_t)p[2 + i] << (8 * i);
          uint32_t ai = (bits >> (3 * (py * 4 + px))) & 7;
          uint8_t av;
          if (ai == 0) av = a0; else if (ai == 1) av = a1;
          else if (a0 > a1) av = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
          else if (ai == 6) av = 0; else if (ai == 7) av = 255; else av = ((6 - ai) * a0 + (ai - 1) * a1) / 5;
          o[0] = pal[idx][0]; o[1] = pal[idx][1]; o[2] = pal[idx][2]; o[3] = av;
        } else {
          o[0] = 128; o[1] = 128; o[2] = 0; o[3] = 255;
        }
      }
    }
  }
  // swizzle de la fetch constant (dword 3, bits 1..12) : 0..3 = x,y,z,w ; 4 = 0 ; 5 = 1
  uint32_t sw[4] = {(f.d[3] >> 1) & 7, (f.d[3] >> 4) & 7, (f.d[3] >> 7) & 7, (f.d[3] >> 10) & 7};
  bool dxt = blk;
  rgba.resize((size_t)w * h * 4);
  for (size_t i = 0; i < (size_t)w * h; i++) {
    const uint8_t* c = &comp[i * 4];
    for (int k = 0; k < 4; k++) {
      uint32_t s = sw[k];
      // pour DXT, le décodage est déjà en RGBA naturel : swizzle identité attendu (0,1,2,3)
      rgba[i * 4 + k] = s < 4 ? c[s] : (s == 4 ? 0 : 255);
    }
    (void)dxt;
  }
  return true;
}

}  // namespace ngpu
