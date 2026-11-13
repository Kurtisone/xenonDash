// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - facade du rendu natif generique (generique) : voir native/front.cpp.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace rex {
class Runtime;
}

// Cree le noyau ngpu_core, le fil de rendu et le GPU nul ; appele une fois apres le demarrage du runtime.
void XdFrontStart(rex::Runtime* runtime);
// Un appel D3D crochete s'est execute : `before` = ses arguments, `result` = r3 (d3d_hooks.generated.inc).
void XdFrontOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base);
// Appele depuis le crochet de Swap, avant le Swap d'origine.
void XdFrontOnSwap(uint8_t* base);
// Un shader vient d'etre construit par la XDK : `object` (resultat) et `container` (conteneur 0x102A11xx) pour le registre de shaders.
void XdFrontOnShaderCreate(uint8_t* base, uint32_t object, uint32_t container);
// GpuBeginShaderConstantF4 vient de reserver `count` vecteurs a partir du registre `start` (sommets ou pixels) : `address` est
// l'adresse invitee ou le jeu va les ecrire.
void XdFrontOnGpuConstants(uint8_t* base, uint32_t pixel, uint32_t start, uint32_t count, uint32_t address);
