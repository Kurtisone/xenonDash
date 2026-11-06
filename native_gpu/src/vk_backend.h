// native_gpu : backend Vulkan (SDL + Vulkan directement, sans le plugin « xenos » du SDK).
#pragma once

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

}  // namespace ngpu
