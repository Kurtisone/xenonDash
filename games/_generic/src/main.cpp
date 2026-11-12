// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - point d'entree du jeu recompile en mode generique (games/_generic).
#include "xd_project.h"

#include "generic_app.h"

REX_DEFINE_APP(xdgame, GenericApp::Create)
