// xenondash common - ce que la lame Guide demande au reste du Dashboard.
//
// La lame (dash_guide.cpp) ne connait ni les profils ni les succes : elle affiche une liste et
// signale une action. Ce fichier declare le raccordement, implemente dans dash_guide_actions.cpp
// pour garder les deux cotes separes.
#ifndef XENONDASH_COMMON_SRC_DASH_GUIDE_ACTIONS_H_
#define XENONDASH_COMMON_SRC_DASH_GUIDE_ACTIONS_H_

#include "dash_guide.h"

namespace dash::guide {

// Branche la lame sur le Dashboard : carte du joueur et actions. A appeler une fois, au demarrage
// (dash_app_base.cpp, OnPostSetup).
void Attach();

}  // namespace dash::guide

#endif  // XENONDASH_COMMON_SRC_DASH_GUIDE_ACTIONS_H_
