# XenonDash

Le Dashboard de la Xbox 360 (l'interface Kinect : Welcome, Xbox LIVE, My Xbox, Game Library) qui tourne **nativement sous Linux**, sans émulateur,
avec une bibliothèque de jeux qui se remplit toute seule et des jeux recompilés pour votre machine au premier lancement.

> **Projet en développement.** Le Dashboard Kinect (2.0.13604) s'affiche et se pilote ; un seul jeu est validé de bout en bout.
> XenonDash n'est affilié ni à Microsoft ni à Xbox. Il ne contient **aucun fichier Microsoft ni aucun jeu** : vous fournissez les vôtres.

## Ce qu'il vous faut

- Un PC **Linux x86-64** avec un **GPU Vulkan** (pilotes à jour) ; 16 Go de mémoire vive recommandés pour recompiler les jeux.
- Pour recompiler des jeux : `python3` (3.11 ou plus), `clang`, `cmake`, `ninja` (Arch : `sudo pacman -S python clang cmake ninja`). Pour importer le Dashboard : `python3` seul.
- **FUSE 2** pour lancer l'AppImage ; sinon `APPIMAGE_EXTRACT_AND_RUN=1`.
- **Vos propres fichiers** : la mise à jour système officielle 2.0.13604 (dossier `$SystemUpdate` ou paquet `su20076000_00000000`), facultativement les modules et polices de la flash, et vos jeux.

## Installer et lancer

1. Récupérez `XenonDash-Kinect.AppImage` et rendez-la exécutable (`chmod +x`).
2. Lancez-la. Au premier lancement, elle demande où se trouve la mise à jour système (fenêtre `zenity`/`kdialog`, sinon le terminal), l'importe, génère les shaders (une vingtaine de secondes)
   et crée le dossier de jeux `~/XenonDash/Games`. Ensuite, elle démarre directement.
3. Données (profil, import, journal `dash.log`) : `~/.local/share/xenondash/2.0.13604.0-xam/`.

En ligne de commande : `--import '/chemin/vers/$SystemUpdate'`, `--help`, `--reimport` (profils conservés), `--data-dir`, `--games-dir`, `--regen-shaders`.

## Ajouter des jeux

Déposez-les dans `~/XenonDash/Games` (ou `XENONDASH_GAMES_DIR`) : dossier extrait avec `default.xex`, `.xex`, ISO, paquet XBLA, Games on Demand. Rien n'est copié ni modifié.
Le Dashboard les repère au démarrage (**F6** relance l'analyse) et ils apparaissent dans la **Game Library**.

Pour lancer : Game Library, le jeu, **A**, **Play Game**. Au premier lancement, le jeu est **recompilé pour votre machine** (12 à 25 minutes, environ 1 Go par jeu), puis démarre ; le Dashboard revient
quand vous le quittez. Un jeu non rendu nativement est refusé avec une explication : aucun repli sur un émulateur. Journal : `~/.local/share/xenondash/games/<TitleID>/<clé>/build.log`.

## Ce qui marche

| | État |
|---|---|
| Dashboard Kinect : hub, My Xbox, Xbox LIVE (hors ligne), Game Library, succès, navigation | fonctionne |
| Profil local, création de profil et connexion comme sur console (clavier virtuel, fenêtres XAM) | fonctionne (hors ligne) |
| Bouton Guide (`G`), icône Gamerscore, avatar prédéfini sur le XMB (éditeur d'avatar complet : non disponible) | fonctionne (au banc ; à confirmer sur Steam Deck) |
| Bibliothèque : dossiers, XEX, XBLA | fonctionne ; ISO et GOD vérifiés sur fichiers de test seulement |
| Jeux : Skate | fonctionne ; autres titres à tester (Trials Fusion : image noire, refusé) |
| Xbox LIVE, achats, succès en ligne ; Dashboards Blades (6770, 6717) et Metro (17559) | indisponibles ; non prioritaires |

## Contrôles

Manette SDL (Xbox, DualShock, DualSense...) directement. Clavier, positions QWERTY physiques (AZERTY : `A` à la place de `Q`) :
| Touche | Manette | Touche | Manette |
|---|---|---|---|
| `Espace`, `Entrée` | **A** | `Q` / `E` | LB / RB |
| `Retour arrière`, `Échap` | **B** | `1` / `3` | LT / RT |
| `X` / `Y` | X / Y | `Tab` | Back |
| Flèches | croix | `P` | **Start** (valide « Done ») |
| `WASD` (AZERTY `ZQSD`) | stick gauche | `Shift` + flèches | stick droit |
| `G` | **Guide** | `F6` | rescanner les jeux |

`Entrée` envoie **A**, pas Start. Redéfinition : `--keybind_a=Return,Space`, `--keybind_right_shoulder=E` (noms X11 : `space`, `Return`, `BackSpace`, `Tab`, `Escape`, `Down`...).

## En cas de problème

- **Écran noir** : `--regen-shaders` ; vérifiez Vulkan (`vulkaninfo`). **L'AppImage ne démarre pas** : `APPIMAGE_EXTRACT_AND_RUN=1 ./XenonDash-Kinect.AppImage`.
- **Un jeu n'apparaît pas** : vérifiez le dossier, **F6**, puis `dash.log`. **La construction échoue** : `build.log`, `clang`, `cmake`, `ninja`, espace disque.

## Usage et licence
Licence **GPL-3.0** (`LICENSE`). Les fichiers importés et les programmes générés à partir d'eux (Dashboard et jeux recompilés) contiennent une traduction de code propriétaire : usage
personnel uniquement, ne les partagez pas. Utilisez du contenu dont vous êtes propriétaire.

## Pour les développeurs

Compiler, porter une version, XAM, rendu natif, reprise du travail : [docs/REPRISE.md](docs/REPRISE.md) (carte de la documentation) et [PLAN.md](PLAN.md) (décisions, To-Do).
