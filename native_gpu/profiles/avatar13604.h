// native_gpu : XDK D3D 2.0.20158 liee dans AvatarEditor.xex 2.0.13604 (titre 584D07D1, editeur d'avatar). MEME bibliotheque que dash.xex
// (jetons de code identiques a 1.0 sur toutes les fonctions comparees, tools/recipe_build.py identify + comparaison directe) : memes decalages
// du peripherique, seules les adresses changent. Aucun contenu Microsoft : des adresses et des nombres.
#pragma once

#include <cstdint>

// Symboles des fonctions invitees (`sub_<adresse>`) : le jeton d'adresse sert aux crochets (NGPU_HOOK, ngpu.h).
#define NGP_BeginVertices 92129BE8          // (dev, prim, count, stride) -> pointeur d'ecriture des sommets
#define NGP_EndVertices 92129BD8            // (dev)
#define NGP_DrawVerticesUP 9212A0A8         // (dev, prim, count, donnees, stride)
#define NGP_DrawIndexedVerticesUP 9212A630  // (dev, prim, minVertex, nbSommets, nbIndices, indices, format, sommets, stride [pile +84])
#define NGP_DrawVertices 9212A6D0           // (dev, prim, premier sommet, nombre)
#define NGP_DrawIndexedVertices 9212AAE8    // (dev, prim, sommet de base, premier indice, nombre)
#define NGP_ClearF 9212DC70                 // (dev, drapeaux, rect*, couleur* (4 float BE), f1 = z, r8 = stencil)
#define NGP_BeginTiling 92128C90            // (dev, drapeaux, nombre, rects, couleur*, ...)
#define NGP_Resolve 9212B990                // (dev, drapeaux, rect source, texture, point destination, ...)
#define NGP_Swap 92126DD0                   // appelle VdSwap
#define NGP_CreateVertexShader 92126468     // (conteneur) -> objet
#define NGP_CreatePixelShader 92126280
#define NGP_SetVertexShader 92125C88
#define NGP_SetPixelShader 92125A80
#define NGP_VblankHandler 92126618
#define NGP_WaitFence 9212E618              // (dev, fence, ...)
#define NGP_WaitDrain 9212DF68              // (dev, position, taille)
#define NGP_WaitSecond 9212DED0             // (dev, a, b)
#define NGP_WaitBatches 9212FB08            // (dev) : boucle tant que dev+11012 != 0

namespace ngpu_profile {
constexpr uint32_t kFencePtr = 10896;        // dev + : pointeur vers le bloc de retour du GPU
constexpr uint32_t kBatchCounter = 11012;    // dev + : compteur de lots, incremente a chaque fin de tuilage (sub_921AE7B8, r7=1), attendu a 0 par sub_92198978 ; jamais decremente sans file reelle
constexpr uint32_t kFenceValue = 10908;      // dev + : derniere position emise
constexpr uint32_t kDrainWord = 60;          // mot +60 du bloc de retour : pointeur de lecture du GPU
constexpr uint32_t kSecondWord = 4;          // mot +4 du bloc de retour : fence secondaire
constexpr uint32_t kVblankHandler = 0x92126618;
constexpr uint32_t kSwapEntry = 0x92126DD0;
constexpr uint32_t kSeqIgnore = 0;
// Disposition du peripherique 2.0.20158 : blocs lus par le noyau identiques a la XDK 5632 (fetch constants +0x480, constantes
// +0x780/+0x1780, groupes de registres 10368..10832) ; seuls les objets courants sont decales de 184 octets.
constexpr uint32_t kIndexBuffer = 12612, kPixelShader = 12868, kVertexShader = 12872;
constexpr uint32_t kShaderPhysicalPtr = 32;  // objet shader + : pointeur vers la partie physique (microcode patche par D3D)
}  // namespace ngpu_profile
