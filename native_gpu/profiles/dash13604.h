// native_gpu : XDK D3D 2.0.20158 liee dans le Dashboard 2.0.13604 (dash.xex) : adresses et decalages du peripherique.
// Retrouves par tools/d3d_match.py (famille dash-13604, tools/d3d_signatures/references.toml) et verifies a la main
// (docs/ngpu_core.md section 13604). Aucun contenu Microsoft : des adresses et des nombres.
#pragma once

#include <cstdint>

// Symboles des fonctions invitees (`sub_<adresse>`) : le jeton d'adresse sert aux crochets (NGPU_HOOK, ngpu.h).
#define NGP_BeginVertices 927E0A58          // (dev, prim, count, stride) -> pointeur d'ecriture des sommets
#define NGP_EndVertices 927E0A48            // (dev)
#define NGP_DrawVerticesUP 927E0F18         // (dev, prim, count, donnees, stride)
#define NGP_DrawIndexedVerticesUP 927E14A0  // (dev, prim, minVertex, nbSommets, nbIndices, indices, format, sommets, stride [pile +84])
#define NGP_DrawVertices 927E1540           // (dev, prim, premier sommet, nombre)
#define NGP_DrawIndexedVertices 927E1958    // (dev, prim, sommet de base, premier indice, nombre)
#define NGP_ClearF 92195E98                 // (dev, drapeaux, rect*, couleur* (4 float BE), f1 = z, r8 = stencil)
#define NGP_BeginTiling 92196370            // (dev, drapeaux, nombre, rects, couleur*, ...)
#define NGP_Resolve 921A57B8                // (dev, drapeaux, rect source, texture, point destination, ...)
#define NGP_Swap 9218EAB0                   // appelle VdSwap
#define NGP_CreateVertexShader 9219D0F0     // (conteneur) -> objet
#define NGP_CreatePixelShader 9219CF08
#define NGP_SetVertexShader 9219C8C8
#define NGP_SetPixelShader 9219C6C0
#define NGP_VblankHandler 9218E290
#define NGP_WaitFence 92197358              // (dev, fence, ...)
#define NGP_WaitDrain 92196CA8              // (dev, position, taille)
#define NGP_WaitSecond 92196C10             // (dev, a, b)

namespace ngpu_profile {
constexpr uint32_t kFencePtr = 10896;        // dev + : pointeur vers le bloc de retour du GPU
constexpr uint32_t kFenceValue = 10908;      // dev + : derniere position emise
constexpr uint32_t kDrainWord = 60;          // mot +60 du bloc de retour : pointeur de lecture du GPU
constexpr uint32_t kSecondWord = 4;          // mot +4 du bloc de retour : fence secondaire
constexpr uint32_t kVblankHandler = 0x9218E290;
constexpr uint32_t kSwapEntry = 0x9218EAB0;
constexpr uint32_t kSeqIgnore = 0;
// Disposition du peripherique 2.0.20158 : blocs lus par le noyau identiques a la XDK 5632 (fetch constants +0x480, constantes
// +0x780/+0x1780, groupes de registres 10368..10832) ; seuls les objets courants sont decales de 184 octets.
constexpr uint32_t kIndexBuffer = 12612, kPixelShader = 12868, kVertexShader = 12872;
constexpr uint32_t kShaderPhysicalPtr = 32;  // objet shader + : pointeur vers la partie physique (microcode patche par D3D)
}  // namespace ngpu_profile
