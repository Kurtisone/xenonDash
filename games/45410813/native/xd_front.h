// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - facade du rendu natif de Skate (recette 45410813) : voir native/front.cpp.
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
