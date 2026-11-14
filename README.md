# XenonDash

Le Dashboard de la Xbox 360 (l'interface NXE : Welcome, Xbox LIVE, My Xbox, Game Library) qui tourne **nativement sous Linux**, sans émulateur,
avec une bibliothèque de jeux qui se remplit toute seule et des jeux recompilés pour votre machine au premier lancement.

> **Projet en développement.** Le Dashboard NXE (2.0.13604) s'affiche et se pilote ; un seul jeu est validé de bout en bout (voir « Ce qui marche »).
> XenonDash n'est affilié ni à Microsoft ni à Xbox. Il ne contient **aucun fichier Microsoft ni aucun jeu** : vous fournissez les vôtres.

## Ce qu'il vous faut

- Un PC **Linux x86-64** avec un **GPU compatible Vulkan** (pilotes à jour). 16 Go de mémoire vive recommandés pour recompiler les jeux.
- Pour recompiler des jeux : `python3` (3.11 ou plus), `clang`, `cmake` et `ninja`. Sur Arch : `sudo pacman -S python clang cmake ninja`.
- Pour importer le Dashboard : `python3-cryptography`.
- **FUSE 2** pour lancer un AppImage ; sinon lancez-le avec `APPIMAGE_EXTRACT_AND_RUN=1`.
- **Vos propres fichiers** :
  - la **mise à jour système officielle 2.0.13604** (le dossier `$SystemUpdate` ou le paquet `su20076000_00000000`), issue de votre console ou de l'archive publique du firmware ;
  - (facultatif) un dossier de modules et de polices de la flash pour de meilleures polices : sans lui, des polices de remplacement sont utilisées ;
  - vos jeux : ISO, dossiers extraits, jeux XBLA, Games on Demand.

## Installer et lancer

1. Récupérez l'AppImage `XenonDash-2.0.13604.0-x86_64.AppImage` et rendez-la exécutable (`chmod +x`).
2. Lancez-la. **Au premier lancement**, elle vous demande où se trouve la mise à jour système, l'importe, génère les shaders graphiques (une vingtaine de secondes)
   et crée votre **dossier de jeux** : `~/XenonDash/Games`. Les lancements suivants démarrent directement.
3. Les données (profil, importation, journal `dash.log`) sont dans `~/.local/share/xenondash/2.0.13604.0/`.

En ligne de commande, si vous préférez :

```sh
./XenonDash-2.0.13604.0-x86_64.AppImage --import '/chemin/vers/$SystemUpdate'
./XenonDash-2.0.13604.0-x86_64.AppImage --help
```

## Ajouter des jeux

Déposez vos jeux dans `~/XenonDash/Games` (ou dans le dossier indiqué par `XENONDASH_GAMES_DIR`) : dossier extrait contenant `default.xex`, fichier `.xex`, ISO, paquet XBLA,
Games on Demand. Rien n'est copié ni modifié. Au démarrage, le Dashboard les repère (la touche **F6** relance l'analyse) et ils apparaissent dans la **Game Library** avec leur nom et leur icône.

Pour lancer un jeu : Game Library, le jeu, **A**, puis **Play Game**. **Au premier lancement, le jeu est recompilé pour votre machine** : une barre de progression s'affiche
dans le Dashboard (comptez 12 à 25 minutes, environ 1 Go d'espace disque par jeu) ; le jeu démarre ensuite tout seul, et le Dashboard revient quand vous le quittez.
Les lancements suivants sont immédiats.

Un jeu que XenonDash ne sait pas faire fonctionner nativement est **refusé avec une explication** : il n'y a pas de repli sur un émulateur.

## Ce qui marche, et ce qui ne marche pas encore

| | État |
|---|---|
| Dashboard NXE : hub, My Xbox, Xbox LIVE (hors ligne), Game Library, succès, navigation | fonctionne |
| Profil local « Joueur » créé automatiquement | fonctionne |
| Création de profil comme sur une vraie console (clavier virtuel Xbox, fenêtre de connexion) | **en développement**, pas encore dans l'AppImage |
| Éditeur d'avatar | non disponible (nécessite un paquet que le firmware de base ne contient pas) |
| Bibliothèque de jeux : dossiers, XEX, paquets XBLA | fonctionne |
| Bibliothèque de jeux : ISO, Games on Demand | prévu, vérifié sur des fichiers de test seulement |
| Jeux : **Skate** | fonctionne (testé de bout en bout) |
| Jeux : autres titres | à tester ; les jeux très différents de ceux déjà vus peuvent être refusés (ex. Trials Fusion : image noire, donc refusé) |
| Xbox LIVE, achats, succès en ligne | indisponibles |
| Dashboards Blades (2.0.6770, 2.0.6717) et Metro (2.0.17559) | non prioritaires pour l'instant |

## Contrôles

**Manette** : toute manette reconnue par SDL (Xbox, DualShock, DualSense, ...) fonctionne directement.

**Clavier**, sur les positions QWERTY physiques (sur un clavier AZERTY, A est à la place de Q, Z à la place de W) :

| Touche | Manette |
|---|---|
| Flèches | Croix directionnelle |
| Entrée ou Espace | A |
| Échap ou Retour arrière | B |
| X / Y | X / Y |
| Q et E (AZERTY : A et E) | LB / RB, pour changer d'onglet |
| Tab / P | Back / Start |
| WASD (AZERTY : ZQSD) | Stick gauche |
| F6 | Rescanner le dossier de jeux |

Les touches se redéfinissent : `--keybind_a=Return,Space`, `--keybind_right_shoulder=E`, etc.

## En cas de problème

- **Écran noir au lancement** : `./XenonDash-…AppImage --regen-shaders` regénère les shaders ; vérifiez que les pilotes Vulkan sont installés (`vulkaninfo`).
- **L'AppImage ne démarre pas** : `APPIMAGE_EXTRACT_AND_RUN=1 ./XenonDash-…AppImage`.
- **Le jeu n'apparaît pas** : vérifiez le dossier (`~/XenonDash/Games`), appuyez sur F6, et regardez le journal `~/.local/share/xenondash/2.0.13604.0/dash.log`.
- **La construction d'un jeu échoue** : le Dashboard affiche un message et le journal de construction se trouve dans `~/.local/share/xenondash/games/<TitleID>/`. Vérifiez `clang`, `cmake`, `ninja` et l'espace disque.
- **Refaire l'import** : `--reimport` (vos profils sont conservés). `--data-dir` affiche le dossier de données.

## Usage et licence

XenonDash est distribué sous licence **GPL-3.0** (voir `LICENSE`). Les fichiers que vous importez et les programmes générés à partir d'eux (le Dashboard et les jeux recompilés)
contiennent une traduction de code propriétaire : ils sont **réservés à votre usage personnel**, ne les partagez pas et ne les publiez pas. Utilisez uniquement du contenu dont vous êtes propriétaire.

## Pour les développeurs

Compiler depuis les sources, porter une autre version, comprendre la recompilation, le rendu natif, XAM : voir l'[index de la documentation](docs/INDEX.md).
