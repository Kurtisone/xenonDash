# Guide du joueur

Ce guide décrit ce que vous pouvez faire avec XenonDash aujourd'hui, comment le lancer, et ce qui
ne marche pas encore. Il ne demande aucune connaissance du projet : pour la compilation et
l'architecture, voir `docs/technique.md` et `docs/REPRISE.md`.

---

## 1. Lancer le Dashboard

L'AppImage contient tout (moteur de rendu, toolchain de recompilation). Placez-la où vous voulez et
rendez-la exécutable :

```sh
chmod +x XenonDash-2.0.13604.0-x86_64.AppImage
./XenonDash-2.0.13604.0-x86_64.AppImage
```

**Les fichiers du Dashboard doivent être à côté de l'AppImage.** Le programme les cherche tout seul,
il n'y a pas de dialogue au premier lancement. Sont reconnus :

| Fichier ou dossier | Rôle |
|---|---|
| `$SystemUpdate/` | mise à jour système extraite (contient `dash.xex`, `shrdres.xzp`) |
| `su20076000_00000000` | archive de mise à jour, non extraite |
| `*.zip` | archive de mise à jour, non extraite |
| un dossier de polices | fichiers `.xtt` du Dashboard |
| `xam.xex` | XAM recompilé (voir plus bas) |

Si rien n'est trouvé, le programme s'arrête en indiquant ce qui manque. Il faut alors le contenu du
firmware 2.0.13604 : `dash.xex` et `shrdres.xzp` au minimum.

> XenonDash ne fournit **aucun** fichier Microsoft. Il faut les extraire vous-même de votre propre
> console.

---

## 2. Se déplacer dans le Dashboard

**À la manette** : toute manette reconnue par SDL fonctionne directement (Xbox, DualShock,
DualSense...).

**Au clavier**, sur les positions QWERTY physiques (sur AZERTY, `A` est à la place de `Q`) :

| Touche | Manette |
|---|---|
| `Espace` ou `Entrée` | **A** (valider) |
| `Échap` ou `Retour arrière` | **B** (retour) |
| `X` / `Y` | X / Y |
| `Q` / `E` | LB / RB (changer d'onglet) |
| `1` / `3` | LT / RT |
| `Tab` | Back |
| `P` | Start |
| `W` `A` `S` `D` | Stick gauche |
| Flèches | Croix directionnelle |
| `Maj` + flèches | Stick droit |
| `F5` | Liste des jeux |
| `F6` | Rescanner le dossier de jeux |

Ces touches se redéfinissent : `--keybind_a=Return,Space`, `--keybind_right_shoulder=E`, etc. Le nom
d'une touche suit les *keysyms* X11 (`space`, `Return`, `BackSpace`, `Escape`, `Down`...).

---

## 3. Le Guide (bouton Xbox)

Sur une Xbox 360, le bouton Xbox fait glisser le Guide par-dessus l'image. XenonDash fait de même :

Le volet occupe la bande gauche de l'écran (environ un tiers de la largeur) ; le Dashboard reste
visible à droite et s'assombrit légèrement sous le volet. Un filet vert Xbox souligne le haut, et la
selection courante est encadrée.

### L'ouvrir

**Le bouton Guide est filtré par défaut.** Deux façons de l'atteindre :

- **À la manette** : lancer avec `--guide_button`. Le bouton Xbox est alors transmis au Dashboard.
- **Au clavier** : assigner une touche, `--keybind_guide=G`. Sans cette option, `keybind_guide` est
  vide et le Guide est inatteignable au clavier.

```sh
./XenonDash-2.0.13604.0-x86_64.AppImage --guide_button --keybind_guide=G
```

### S'en servir

| Touche / bouton | Effet |
|---|---|
| Croix haut / bas | changer d'entrée |
| A | valider |
| B | refermer le Guide |

Le contenu dépend de l'état du profil :

- **sans profil** : « Aucun profil », puis « Créer un profil », « Succès », « Retour au tableau de
  bord », « Fermer le Guide » ;
- **avec un profil** : le pseudo et le Gamerscore s'affichent en tête, et « Créer un profil »
  disparaît.

### État honnête de cette fonction

| Élément | État |
|---|---|
| Affichage du volet, animation, assombrissement | **fonctionne** |
| Navigation croix, A, B | **fonctionne** |
| Carte du joueur (pseudo, Gamerscore) | **fonctionne** quand un profil existe |
| Action « Succès » | ouvre l'interface de succès du jeu en cours |
| Action « Créer un profil » | **journalise seulement**, ne crée rien pour l'instant |
| Action « Retour au tableau de bord » | **journalise seulement** |
| Texte | police bitmap intégrée 5×7, lisible mais grossière |

Le Guide est dessiné dans l'image finale, sans ImGui : en rendu natif, le présentateur du SDK est
détaché et aucune superposition ImGui n'existe. Le texte utilise une petite police intégrée ; une
version ultérieure pourra rasteriser la vraie police du Dashboard (freetype est disponible).

---

## 4. Les profils

Sans profil, le Dashboard se comporte comme une console neuve : bandeau sans nom, et le Guide
propose « Créer un profil ».

Un profil déjà créé est reconnu au démarrage : pseudo et Gamerscore s'affichent. Le marqueur est un
fichier `.created` dans `profiles/<XUID>/` — la simple présence du dossier ne suffit pas, car le
Dashboard le crée lui-même dès le démarrage.

**La création de profil « comme sur une console »** (assistant Xbox, clavier virtuel) n'est pas
encore disponible : elle dépend du XAM recompilé, dont l'affichage ne fonctionne pas (voir §6).

---

## 5. Les jeux

Placez vos jeux dans `~/XenonDash/Games/`, un dossier par jeu :

```
~/XenonDash/Games/
├── MonJeu/
│   └── default.xex
└── AutreJeu/
    └── default.xex
```

`F5` ouvre la liste des jeux, `F6` rescanne le dossier.

Au premier lancement d'un jeu, XenonDash **le recompile** : cela prend du temps (plusieurs minutes)
et la progression s'affiche. Les lancements suivants sont immédiats.

**Tous les jeux ne passent pas.** Le projet refuse délibérément un jeu quand la confiance dans le
rendu est insuffisante (< 0,85) plutôt que de produire une image fausse. C'est un choix assumé, pas
un bug : « aucun repli sur l'émulation ».

---

## 6. Ce qui ne marche pas encore

| | État |
|---|---|
| Dashboard Kinect : hub, My Xbox, Xbox LIVE (hors ligne), Game Library, succès, navigation | fonctionne |
| **Guide (bouton Xbox)** | fonctionne (voir §3) |
| Création de profil comme sur console | **écran noir** : le XAM recompilé se charge mais n'affiche rien |
| Éditeur d'avatar | non disponible (paquet absent du firmware de base) |
| Xbox LIVE, achats, succès en ligne | indisponibles (hors ligne assumé) |
| Jeux très différents de ceux déjà vus | peuvent être refusés (ex. Trials Fusion) |

### L'écran noir du XAM recompilé

Le mode « profil piloté par XAM » (`DASH_PROFILE_MODE=xam` + `DASH_XAM_MODULE=2`) charge XAM
recompilé, mais l'écran reste noir : XAM s'initialise puis n'émet plus rien, sans erreur ni blocage.
Le Dashboard en mode HLE (celui décrit dans ce guide) n'est pas affecté.

C'est le chantier en cours. Détail technique : `docs/xam_13604_compilation.md`.

---

## 7. En cas de problème

| Symptôme | Cause probable |
|---|---|
| « --game_data_root cannot be read » | les fichiers du Dashboard ne sont pas à côté de l'AppImage |
| Le Dashboard ne répond pas au clavier | `Entrée` est **Start**, pas A ; `Échap` n'est lié à rien ; utilisez `Espace` / `Retour arrière` |
| Rien ne s'affiche, écran noir | carte graphique sans Vulkan fonctionnel ; essayez `--gpu_plugin=...` |
| Pas de son | `DASH_AUDIO=nop` désactive le son ; sans option, un backend par défaut est choisi |
| Le Guide ne s'ouvre pas | le bouton Guide est filtré par défaut : voir §3 |

Le journal complet s'écrit dans `logs/` à côté de l'exécutable. Les lignes `[ngpu]` vont sur la
sortie d'erreur, pas dans ce journal : pour les voir, lancez depuis un terminal.

---

## 8. Options utiles

| Option | Effet |
|---|---|
| `--guide_button` | transmettre le bouton Guide de la manette |
| `--keybind_guide=G` | touche d'ouverture du Guide |
| `--keybind_a=...` | redéfinir A (et de même pour toutes les touches) |
| `--game_data_root=<dir>` | dossier des fichiers du Dashboard |
| `--gpu_plugin=...` | choisir le moteur graphique |
| `DASH_GUIDE_OPEN=1` | ouvrir le Guide au démarrage (diagnostic) |
| `DASH_AUDIO=nop` | désactiver le son |

---

## Licence et usage

XenonDash ne contient aucun code, fichier ou ressource de Microsoft. Vous devez fournir votre propre
dump du firmware. Le projet ne contourne aucune protection : pas de clé de console, pas de connexion
aux services Xbox.
