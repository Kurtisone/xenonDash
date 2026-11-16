// xenondash common - lame Guide (superposition au-dessus du Dashboard).
//
// Sur une Xbox 360, le bouton Xbox de la manette fait apparaitre le Guide : un volet qui glisse
// par-dessus l'image, quel que soit l'ecran affiche. Sur console il est dessine par le systeme
// (hud.xex, via XamShowGuideUI). Ici, en rendu natif, aucune superposition ImGui n'est disponible
// (DASH_NATIVE_GPU detache le presentateur du SDK) : cette lame est donc composee dans l'image
// finale, exactement comme le HUD de XAM (native_gpu/src/hud_front.cpp, HudComposite).
//
// Le module ne depend que d'un tampon RGBA8 : il ne sait rien de Vulkan ni de D3D. Le dessin
// (panneau, texte) est fait ici ; l'integration a la presentation se fait dans dash_app_base.cpp.
//
// Contenu prevu, dans l'ordre de la console :
//   - carte du joueur : avatar, pseudo, Gamerscore
//   - acces aux succes du jeu en cours
//   - creation de profil quand aucun profil n'existe
//   - retour au hub / quitter le jeu
#ifndef XENONDASH_COMMON_SRC_DASH_GUIDE_H_
#define XENONDASH_COMMON_SRC_DASH_GUIDE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace dash::guide {

// Une entree du volet : libelle affiche et identifiant d'action (voir kAction*).
struct Item {
  const char* label = "";
  int action = 0;
  bool enabled = true;
};

// Identifiants d'action, traites par HandleAction().
enum Action : int {
  kActionNone = 0,
  kActionClose,         // refermer la lame
  kActionAchievements,  // succes du jeu en cours
  kActionProfile,       // creation de profil (aucun profil sur le disque)
  kActionQuitGame,      // quitter le jeu en cours
  kActionHome,          // revenir au hub
};

// Etat a afficher en tete de la lame, rempli par l'appelant (dash_app_base.cpp) pour ne pas
// coupler ce module au noyau du Dashboard.
struct PlayerCard {
  std::string gamertag;
  int gamerscore = 0;
  bool has_profile = false;
};

// Etat de la lame. Le tampon de composition est fourni a Draw(), pas stocke ici.
struct State {
  bool open = false;
  int selected = 0;
  float anim = 0.0f;  // 0 = refermee, 1 = ouverte (glissement)
  double opened_at = 0.0;
};

// Ouvre ou ferme la lame. Renvoie true si l'etat a change.
bool Toggle(State& st);
void Open(State& st);
void Close(State& st);

// Deplace la selection (boucle). Renvoie true si la selection a change.
bool MoveSelection(State& st, int delta, const std::vector<Item>& items);

// Entree courante.
const Item* SelectedItem(const State& st, const std::vector<Item>& items);

// Compose la lame dans un tampon RGBA8 (pixels pre-multiplies non necessaires : alpha ecrase).
// w, h : dimensions du tampon. dt : secondes depuis la derniere image (animation).
// Renvoie true si quelque chose a ete dessine (l'appelant peut alors garder le tampon).
bool Draw(State& st, uint32_t* pixels, uint32_t w, uint32_t h, float dt,
          const PlayerCard& card, const std::vector<Item>& items);

// Vrai si la lame occupe l'ecran (ouvert ou en cours d'animation) : les entrees du Dashboard
// doivent alors etre suspendues, comme sur console.
bool IsOpen(const State& st);
bool IsAnimating(const State& st);

// Source de la carte du joueur : remplie par dash_guide_actions.cpp (Attach). La lame ne lit pas
// les profils elle-meme ; si rien n'est branche, elle affiche « Aucun profil ».
extern bool (*g_card_source)(PlayerCard*);

// Vrai si la touche/le bouton doit ouvrir la lame : bouton Guide de la manette (0x0400) ou la
// touche configuree par keybind_guide.
bool GuidePressed();

}  // namespace dash::guide

#endif  // XENONDASH_COMMON_SRC_DASH_GUIDE_H_
