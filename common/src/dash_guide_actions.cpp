// xenondash common - raccordement de la lame Guide au Dashboard.
//
// La lame affiche une liste ; ici on lui donne (1) la carte du joueur et (2) les actions.
//
// Choix assume : les actions ne passent pas par XAM. En rendu natif, XAM n'est pas charge (le
// Dashboard repond par le HLE), donc XamShowAchievementsUI & co ne sont pas disponibles. Ce qui
// marche aujourd'hui, c'est le Dashboard lui-meme : on s'appuie dessus.
#include "dash_guide_actions.h"

#include <string>

#include <rex/logging.h>
#include <rex/runtime.h>

namespace dash::achievements {
int32_t GpdGamerscore();
}

namespace dash::xamui {
rex::u32 OpenAchievementsForCurrentTitle();
}

namespace dash::profile {
std::string Gamertag();
bool ProfileExistsOnDisk();
}

namespace dash::guide {
namespace {

// Remplit la carte du joueur. Appelee a chaque image par la lame : on reste tres bon marche
// (pas de lecture disque, seulement les valeurs deja en memoire).
bool FillCard(PlayerCard* card) {
  if (!card) return false;
  const bool have = dash::profile::ProfileExistsOnDisk();
  card->has_profile = have;
  card->gamertag = have ? dash::profile::Gamertag() : std::string();
  card->gamerscore = have ? int(dash::achievements::GpdGamerscore()) : 0;
  return true;
}

}  // namespace

void PerformAction(int action) {
  switch (action) {
    case kActionAchievements: {
      // Meme interface que le crochet XamShowAchievementsUI (dash_xam_ui.cpp) : c'est celle qui
      // affiche les succes du jeu en cours en rendu natif.
      const u32 r = dash::xamui::OpenAchievementsForCurrentTitle();
      REXLOG_INFO("[guide] succes du jeu en cours -> {:08X}", uint32_t(r));
      break;
    }
    case kActionProfile:
      REXLOG_INFO("[guide] creation de profil demandee");
      break;
    case kActionHome:
      REXLOG_INFO("[guide] retour au tableau de bord");
      break;
    case kActionQuitGame:
      REXLOG_INFO("[guide] quitter le jeu");
      break;
    default:
      break;
  }
}

void Attach() {
  g_card_source = &FillCard;
  REXLOG_INFO("[guide] lame branchee (carte du joueur, actions)");
}

}  // namespace dash::guide
