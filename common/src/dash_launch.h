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
  bool from_library = false;  // vient de games.auto.toml (bibliothèque), pas de games.toml
  std::string recipe;    // `recipe = "<source>"` : source du jeu (dossier avec default.xex, .xex ou .iso) ; recette games/<TitleID>/
};

// État de la construction d'un jeu par le moteur de recettes (tools/recipe_build.py), pour l'écran de progression.
struct BuildStatus {
  bool active = false;   // une construction est en cours
  bool failed = false;   // la dernière construction a échoué (error)
  uint32_t title_id = 0;
  std::string name, stage, message, error;
  int index = 0, total = 0;  // étape courante / nombre d'étapes
  double percent = 0.0;  // 0..100
  double elapsed = 0.0;  // secondes
  double estimate = 0.0; // secondes (estimation de la recette)
  std::string log_path;
};
BuildStatus GetBuildStatus();

// Bibliothèque de jeux (tools/game_library.py) : état du dernier scan.
struct LibraryStatus {
  bool scanning = false;
  int count = 0;            // jeux venant de games.auto.toml
  double done_at = 0;       // horloge monotone (s) de la fin du dernier scan
  std::string message, games_dir;
};
LibraryStatus GetLibraryStatus();
void RescanLibrary(bool wait, int timeout_s = 4);  // `game_library.py init` (1re fois) puis `scan`

std::vector<Game> Games();  // relit games.toml à chaque appel
std::optional<Game> Find(uint32_t title_id);
std::optional<Game> DiscGame();      // titre lancé par LaunchTitle(NULL) (disque, sinon seul jeu déclaré)
std::optional<Game> InsertedDisc();  // jeu explicitement dans le lecteur (disc = true, --dash_disc) ; sinon rien
bool LibraryEnabled();
bool LaunchGame(const Game& g);  // ne revient pas si le lancement réussit
void Init();                     // --dash_launch

}  // namespace dash::launch
