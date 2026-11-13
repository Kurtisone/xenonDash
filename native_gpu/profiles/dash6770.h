// native_gpu : constantes de la XDK D3D du Dashboard 6770 (D3D 2.0.6132) lues par le code commun (null_gpu.cpp, d3d_trace.cpp).
// Adresses et decalages seulement ; les crochets propres a cette version restent dans front6770.cpp / hooks_*.cpp.
#pragma once

#include <cstdint>

namespace ngpu_profile {
constexpr uint32_t kFencePtr = 10896;        // dev + : pointeur vers le bloc de retour du GPU (identifiant ecrit en retour)
constexpr uint32_t kFenceValue = 10908;      // dev + : derniere position emise
constexpr uint32_t kVblankHandler = 0x92121210;  // gestionnaire de l'interruption graphique (dev)
constexpr uint32_t kSwapEntry = 0x92121EE8;  // entree publique de Swap (compteur d'images du traçage)
constexpr uint32_t kSeqIgnore = 0x92120C98;  // scrutation a delai : exclue du journal sequentiel (DASH_D3D_SEQ)
}  // namespace ngpu_profile
