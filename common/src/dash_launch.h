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
};

std::vector<Game> Games();  // relit games.toml à chaque appel
std::optional<Game> Find(uint32_t title_id);
std::optional<Game> DiscGame();
bool LibraryEnabled();
bool LaunchGame(const Game& g);  // ne revient pas si le lancement réussit
void Init();                     // --dash_launch

}  // namespace dash::launch
