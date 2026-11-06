// dash - registre des jeux et lancement (voir dash_launch.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dash::launch {

struct Game {
  uint32_t title_id = 0;
  std::string name;
  std::string kind;      // "native" ou "xenia"
  std::string command;   // commande shell complète
  std::string cwd;       // répertoire de travail du jeu (vide = celui du Dashboard)
  bool keep_ld = false;  // conserver LD_LIBRARY_PATH du Dashboard dans l'environnement du jeu
  uint32_t content_type = 0x000D0000;  // catégorie dans Games Library (arcade par défaut)
  bool disc = false;     // `disc = true` : jeu « inséré dans le lecteur » (bouton de disque du Dashboard)
  std::string icon;      // icône PNG (<= 16 Ko) ; à défaut celle du GPD/SPA du jeu (`spa`, ou <TitleID>.gpd du profil)
  std::string spa;       // fichier XDBF (.spa/.gpd) d'où tirer l'icône
};

std::vector<Game> Games();  // relit games.toml à chaque appel
std::optional<Game> Find(uint32_t title_id);
std::optional<Game> DiscGame();      // titre lancé par LaunchTitle(NULL) (disque, sinon seul jeu déclaré)
std::optional<Game> InsertedDisc();  // jeu explicitement dans le lecteur (disc = true, --dash_disc) ; sinon rien
bool LibraryEnabled();
bool LaunchGame(const Game& g);  // ne revient pas si le lancement réussit
void Init();                     // --dash_launch

}  // namespace dash::launch
