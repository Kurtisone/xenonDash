// dash 2.0.13604.0 - table des fuseaux horaires du Dashboard, pour rendre le fuseau de l'hôte sous la forme d'une de ses entrées
// (common/src/kernel_xconfig.cpp, SetConsoleTimeZoneTable).
//
// 77 entrées de 32 octets à 0x929531A8, dans l'image chargée : recherche du fuseau courant sub_9225C608 (biais puis nom ; à
// défaut, entrée n° 26 : GMT, Londres), conversion sub_9225C468, vérification au démarrage sub_9225C7C0 (écrit l'entrée trouvée
// par SetTimeZoneInformation, sub_927E08E0, si ses biais ou ses dates diffèrent des réglages).
#include "kernel_xconfig.h"

namespace {
[[maybe_unused]] const bool registered = (dash::xconfig::SetConsoleTimeZoneTable(0x929531A8u, 77), true);
}
