// native_gpu : décodage des textures Xenos (détuilage 2D, échange d'octets, DXT/8888/565...) vers RGBA8 hôte.
#pragma once
#include <cstdint>
#include <vector>

namespace ngpu {

struct FetchConst { uint32_t d[6]; };

struct TexInfo {
  uint32_t width = 0, height = 0, format = 0, endian = 0, pitch = 0, base = 0;  // base : adresse invitée des données
  bool tiled = false;
  bool ok = false;
};

TexInfo DescribeTexture(const FetchConst& f);
// Taille en octets de la zone de données lue (pour la détection de modifications).
size_t TextureDataSize(const TexInfo& t);
// Décode le niveau 0 en RGBA8 (w*h*4), en appliquant le swizzle de la fetch constant. false si format non géré.
// `data` : copie des octets invités (TextureDataSize(t) octets).
bool DecodeTexture(const uint8_t* data, const FetchConst& f, const TexInfo& t, std::vector<uint8_t>& rgba);

}  // namespace ngpu
