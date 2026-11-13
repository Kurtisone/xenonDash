// native_gpu : D3D9 2.0.2099.16 lie dans xam.xex 13604 (rendu du HUD : boites de message, clavier, connexion...), adresses et decalages
// du peripherique. Retrouves a la main (desassemblage tools/ppc_dis.py du code genere de XAM, traces des appels, comparaison des groupes
// de registres du peripherique avec les tampons de commandes), voir docs/xam_recompile.md palier N2. Des adresses et des nombres seulement.
#pragma once

#include <cstdint>

namespace hud_profile {
// Fonctions D3D de XAM (adresses invitees ; le code genere les appelle sub_<adresse>, crochets de src_xam_d3d/xam_d3d_hooks.cpp).
constexpr uint32_t kFrame = 0x818A1368;           // fonction d'image du HUD (boucle libre, ~2400/s sans vsync)
constexpr uint32_t kBeginVertices = 0x819C8E38;   // (dev, prim, nombre, pas) -> pointeur d'ecriture des sommets (dans le tampon de commandes)
constexpr uint32_t kEndVertices = 0x819C90B8;     // (dev)
constexpr uint32_t kDrawVerticesUP = 0x819C90D8;  // (dev, prim, nombre, donnees, pas) : Begin + copie + End fondu
constexpr uint32_t kResolve = 0x819CA3E0;         // (dev, drapeaux, rect*, texture, point*, ..., f1 = z) : destination RB_COPY_DEST_* de l'ombre
constexpr uint32_t kSubmit = 0x819CC490;          // (dev, texture, ...) -> VdSetSystemCommandBuffer(descripteur) : fin d'image du HUD

// Disposition du peripherique (D3D 2099). Les blocs lus par le noyau sont a la place canonique, sauf les groupes de registres
// (decales de 1088 octets) et les objets shader courants.
constexpr uint32_t kRegisterShift = 1088;  // groupes de registres : canonique + 1088 (0x2000 a 11456, 0x2100 a 11532, ...)
constexpr uint32_t kPixelShader = 12944;   // dev + : objet pixel shader courant
constexpr uint32_t kVertexShader = 12948;  // dev + : objet vertex shader courant
constexpr uint32_t kIndexBuffer = 12940;   // sans objet (le HUD ne dessine aucun indexe) : emplacement quelconque lu par Canonicalize

// Objet shader : le conteneur (ancien format 0x102A0E0x, partie virtuelle) est copie dans l'objet, qui porte aussi le pointeur du
// microcode (copie physique que D3D complete : vfetch).
constexpr uint32_t kVsContainer = 592, kVsMicrocodePtr = 0x28;
constexpr uint32_t kPsContainer = 52, kPsMicrocodePtr = 0x0C;
}  // namespace hud_profile
