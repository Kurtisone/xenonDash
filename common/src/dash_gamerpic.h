// dash - image de joueur par défaut, générée (aucune ressource externe).
//
// PNG 64x64 RGB : disque dégradé vert sur fond sombre, anneau clair, motif dérivé du gamertag
// (la teinte et l'épaisseur varient d'un gamertag à l'autre). Encodeur PNG minimal : blocs
// deflate non compressés ; en RGB l'image fait ~12 Ko, sous le tampon de 16 Ko du Dashboard.

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dash::gamerpic {

inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc ^= p[i];
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
  }
  return ~crc;
}

inline void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

inline void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBE32(png, uint32_t(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  png.insert(png.end(), td.begin(), td.end());
  PutBE32(png, Crc32(td.data(), td.size()));
}

inline std::vector<uint8_t> EncodePng(const std::vector<uint8_t>& rgb, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;  // lignes préfixées du filtre 0
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    raw.insert(raw.end(), rgb.begin() + y * w * 3, rgb.begin() + (y + 1) * w * 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  for (size_t off = 0; off < raw.size(); off += 65535) {
    uint16_t len = uint16_t(std::min<size_t>(65535, raw.size() - off));
    z.push_back(off + len >= raw.size() ? 1 : 0);
    z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
    z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + len);
  }
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  PutBE32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBE32(ihdr, w); PutBE32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  return png;
}

inline std::vector<uint8_t> MakeDefault(const std::string& gamertag, uint32_t size = 64) {
  uint32_t seed = Crc32(reinterpret_cast<const uint8_t*>(gamertag.data()), gamertag.size());
  float hue_shift = float(seed % 60) / 100.0f - 0.3f;  // -0.3 .. +0.3 autour du vert
  float ring_w = 0.06f + float((seed >> 8) % 5) * 0.015f;
  int spokes = 3 + int((seed >> 16) % 4);

  std::vector<uint8_t> rgb(size * size * 3);
  for (uint32_t y = 0; y < size; y++) {
    for (uint32_t x = 0; x < size; x++) {
      float u = (x + 0.5f) / size * 2 - 1, v = (y + 0.5f) / size * 2 - 1;
      float r = std::sqrt(u * u + v * v), ang = std::atan2(v, u);
      float R = 0.10f, G = 0.12f, B = 0.11f;  // fond
      if (r < 0.86f) {
        float t = 1.0f - r / 0.86f;           // dégradé radial
        float wave = 0.5f + 0.5f * std::cos(ang * spokes + r * 6.0f);
        G = 0.35f + 0.55f * t + 0.10f * wave;
        R = std::max(0.0f, 0.15f + hue_shift + 0.25f * t * wave);
        B = std::max(0.0f, 0.12f - hue_shift + 0.20f * t * (1 - wave));
      }
      if (std::fabs(r - 0.86f) < ring_w) { R = G = B = 0.92f; }  // anneau
      uint8_t* p = &rgb[(y * size + x) * 3];
      p[0] = uint8_t(std::min(1.0f, R) * 255); p[1] = uint8_t(std::min(1.0f, G) * 255);
      p[2] = uint8_t(std::min(1.0f, B) * 255);
    }
  }
  return EncodePng(rgb, size, size);
}

}  // namespace dash::gamerpic
