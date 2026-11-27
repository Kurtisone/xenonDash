// xenondash common - session transmise d'un processus à l'autre (Dashboard <-> titre natif lancé par le superviseur).
//
// Sur console, XAM vit dans le processus système : les utilisateurs connectés et les données de lancement (XamLoaderSetLaunchData)
// survivent au changement de titre. Ici chaque titre natif (l'éditeur d'avatar, 2e exécutable) est un processus séparé dont
// XAM recompilé démarre sans utilisateur. Le processus qui cède la place écrit donc un fichier de session dans le dossier d'état de
// XAM (DASH_XAM_STATE, sinon ~/.local/share/xenondash/xam/session.txt) ; le suivant le lit, le supprime, rejoue la connexion des
// comptes dans son XAM (XamUserLogon, mêmes arguments que signin.xex) et pose les données de lancement avant le démarrage du titre.
//
// Format (texte) : une ligne `xuid<i>=<XUID hexa>` par emplacement 0..3 occupé, `launch=<octets hexa>` pour les données de lancement.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

#include <rex/ppc/context.h>

namespace rex::system {
class KernelState;
}

namespace dash::session {

struct Session {
  std::array<uint64_t, 4> xuid{};  // XUID connecté à chaque emplacement (0 : vide)
  std::vector<uint8_t> launch;     // données de lancement (XamLoaderSetLaunchData) ; vide : aucune
  bool Any() const { return xuid[0] || xuid[1] || xuid[2] || xuid[3]; }
};

std::filesystem::path Path();
bool Write(const Session& s);

// XUID des quatre emplacements du XAM recompilé de ce processus (appel invité : ctx = contexte de l'appelant).
std::array<uint64_t, 4> SignedInXuids(const PPCContext& ctx, uint8_t* base);

// Au démarrage : lit et supprime le fichier de session, pose les données de lancement, connecte les comptes dans XAM recompilé et
// attend la fin de la connexion (30 s au plus) : le titre démarre ensuite avec l'utilisateur déjà connecté. Sans fichier : ne fait rien.
void Restore(rex::system::KernelState* ks);

}  // namespace dash::session
