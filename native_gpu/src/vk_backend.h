// native_gpu : backend Vulkan (SDL + Vulkan directement, sans le plugin « xenos » du SDK).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ngpu {

// Écran de présentation (dimensions du tampon d'image du Dashboard : 1280x720).
constexpr uint32_t kBackbufferWidth = 1280;
constexpr uint32_t kBackbufferHeight = 720;

// Initialise Vulkan (instance, périphérique, tampon d'image) ; idempotent ; false si indisponible.
bool VkInit();
// Efface le tampon d'image (couleur RGBA en flottants).
void VkClear(const float rgba[4]);
// Présente le tampon d'image dans la fenêtre SDL (blit + file de présentation) ; renvoie le nombre d'images présentées.
uint64_t VkPresent();


// --- dessin ---
struct HostTexture;  // image Vulkan RGBA8 + vue (opaque)
// Crée une texture RGBA8 à partir de pixels décodés (w*h*4 octets) ; nullptr en cas d'échec.
HostTexture* VkCreateTexture(uint32_t w, uint32_t h, const uint8_t* rgba);
void VkDestroyTexture(HostTexture* t);

struct SamplerDesc {
  uint8_t linear = 1;     // filtre min/mag : 0 = point, 1 = linéaire
  uint8_t mip_linear = 0;
  uint8_t addr_u = 2, addr_v = 2;  // 0 = répétition, 1 = miroir, 2 = bord (clamp), 3 = miroir+clamp
};
struct VtxAttr {
  uint32_t location;
  bool is_uint;     // false : float4 (R32G32B32A32_SFLOAT) ; true : uint4 (R32G32B32A32_UINT)
  uint32_t offset;  // dans le sommet hôte
};
struct BlendDesc {
  bool enable = false;
  uint8_t src_c = 1, dst_c = 0, op_c = 0, src_a = 1, dst_a = 0, op_a = 0;  // valeurs Xenos (voir hooks_draw.cpp)
  uint8_t color_mask = 0xF;
};
struct DrawDesc {
  const uint32_t* vs_spv = nullptr; size_t vs_words = 0; uint64_t vs_id = 0;
  const uint32_t* ps_spv = nullptr; size_t ps_words = 0; uint64_t ps_id = 0;
  uint32_t topology = 3;  // VkPrimitiveTopology
  VtxAttr attrs[16]; uint32_t nattrs = 0; uint32_t stride = 0;
  const void* vdata = nullptr; uint32_t vcount = 0;
  BlendDesc blend;
  const float* vsc = nullptr;  // 256 x float4
  const float* psc = nullptr;  // 256 x float4
  HostTexture* tex[16] = {}; SamplerDesc samp[16];
  float viewport[4] = {0, 0, 1280, 720};
  int32_t scissor[4] = {0, 0, 1280, 720};  // x, y, w, h
  float half_pixel[2] = {0, 0};
};
bool VkDraw(const DrawDesc& d);

}  // namespace ngpu
