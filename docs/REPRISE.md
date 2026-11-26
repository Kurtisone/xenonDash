# Reprise du projet par une autre session ou un autre modèle

Ce document permet de reprendre le travail sans l'historique de conversation. Dernière mise à jour : 2026-11-26.
**État bref** : boot propre, hub Kinect navigable au clavier, Game Library stable, boîtes système et HUD rendus, Guide, création de profil, connexion et avatar prédéfini fonctionnels (banc),
une seule AppImage `XenonDash-Kinect.AppImage`. Non vérifié : l'AppImage sur le Deck avec les derniers correctifs, l'éditeur d'avatar complet.
Détail avant condensation : `git show d96c252:docs/REPRISE.md`.

## Comment reprendre

Ordre de lecture : ce document, la section To-Do de [PLAN.md](../PLAN.md), puis [xam_recompile.md](xam_recompile.md) (XAM, HUD, profil, avatar, Guide), [ngpu_core.md](ngpu_core.md) (rendu), [jeux.md](jeux.md)
(bibliothèque, lancement, recettes), [technique.md](technique.md) (architecture, portage, AppImage), [compilation.md](compilation.md) (build), [ghidra.md](ghidra.md) (décompilation). `git log --oneline | head -40` pour le dernier état.
Notes d'agents non versionnées : `work/notes/`. Objectif en cours : le Dashboard Kinect reste dans l'interface Xbox (plus de fenêtres ImGui) avec les vraies fenêtres du HUD de XAM ; **rendu natif, zéro émulation**
(Xenia et `--gpu_plugin=xenos` seulement comme oracle de comparaison). Blades (6770) et Metro (17559) : à la demande de l'utilisateur.

## Carte de la documentation

| Document | Contenu |
|---|---|
| [../README.md](../README.md) | utilisateurs : installation, jeux, contrôles, dépannage |
| [../PLAN.md](../PLAN.md) | objectif, décisions, phases P0 à P8, **To-Do** (seule liste du projet) |
| [REPRISE.md](REPRISE.md) | ce document : règles, méthode, pièges |
| [technique.md](technique.md) | architecture commune, portage d'une version, noyau et HLE, profils, AppImage, état par version, inventaire des fonctions |
| [compilation.md](compilation.md) | build hors poste de développement : SDK, conteneur, AppImage, banc visuel |
| [xam_recompile.md](xam_recompile.md) | XAM, HUD et modules XUI 13604 recompilés : architecture, paliers, correctifs, profil, Guide, avatar |
| [ngpu_core.md](ngpu_core.md) | noyau de rendu natif Vulkan, façades D3D (6770, 13604, HUD), shaders, parité mesurée |
| [jeux.md](jeux.md) | bibliothèque, formats, lancement, recettes, recompilateur générique |
| [ghidra.md](ghidra.md) | projet Ghidra de XAM et de `dash.xex` : noms partagés, décompilation |

Interfaces du Dashboard (les confondre fausse la documentation) : **Blades** 2.0.1888 à 6770, **NXE** 2.0.7357 à 9199 (aucune version portée), **Kinect** 2.0.12611 à 13604, **Metro** 2.0.14699 à 17559.
Le 2.0.13604 est un **Kinect**. `dashboards/2.0.13604.0-xam` est la seule variante (depuis le 2026-11-26, XAM recompilé par défaut ; `main.cpp` pose `DASH_XAM_MODULE=2` et `DASH_PROFILE_MODE=xam`, HLE du SDK en repli).
Règle : le README reste court et destiné aux utilisateurs ; tout détail technique va dans `docs/`.

## Règles de l'utilisateur

- **Lecteur CD vide et muet** : aucune simulation de disque (supprimée le 2026-11-21 : `disc = true` ignoré, `XamLoaderGetMediaInfo` rend « aucun média », `XamLoaderGetDvdTrayState` 2). Les jeux passent par le dossier de jeux.
- Commits **uniquement sous son nom** (Kurtisone, noreply GitHub), **jamais de Co-Authored-By ni de lien de session**. Sous-module SDK d'abord s'il change.
- Aucun fichier Microsoft, de jeu, shader traduit, capture ou binaire dans le dépôt (`generated/`, `out/`, `work/` sont ignorés).
- Pas de clés de console fabriquées, pas de contournement de protection de contenu : profil local hors ligne, données de test.
- Confirmer avant toute action destructrice ou sortante (force-push, suppression, publication).

## Méthode

- Établir la cause avec preuves avant de coder (gdb en lot, watchpoints sur les alias mémoire, `--log_flush_interval=1`, traces par `REX_HOOK_RAW`). Code PowerPC : `tools/ppc_dis.py` ; fonctions D3D : `tools/d3d_match.py`.
  **Chaque hypothèse se mesure avant d'être annoncée ; une hypothèse réfutée se retire du dossier.**
- **Banc** `tools/banc.py` (`start`, `keys`, `shot`, `wait`, `cpu`, `grep`, `stop`) : lance le Dashboard Kinect (ou une autre version avec `--build`, `--unset DASH_XAM_MODULE --unset DASH_PROFILE_MODE` pour 6770/17559) comme l'AppImage,
  sur une COPIE du dossier de données et une COPIE de la bibliothèque (`XENONDASH_LIBRARY_DIR` : sinon le scan réécrit `~/.local/share/xenondash/library`), Xvfb sans pointeur, arrêt par PID. `cpu` : charge par fil (écarter `llvmpipe-*`).
  Plantage sans trace : `start --gdb` écrit dans `out.log` la pile du fil fautif, l'instruction et les registres ; `--gdb-pre F` (commandes avant le lancement, points d'arrêt, surveillance matérielle), `--gdb-x F` après la pile.
- **Suite** `tools/banc_suite.py run` (~4 min, code de sortie 0/1) : parcours fixe (Welcome, My Xbox, Game Library, Video Library, Console Settings, Screen Resolution, Guide, Sign In, Guide connecté), captures comparées aux références,
  journal (aucun `*** STOP` ni `[fibre]`, découverte de l'affichage présente, aucune ligne `[error]` hors `ERROR_ALLOWED`, vide : une erreur est un défaut à corriger ou un cas normal à journaliser plus bas), charge par fil après fermeture du Guide.
  Le rendu lavapipe est presque déterministe : tolérances serrées. Régressions connues détectées : `--env DASH_VIDEO_DISCOVERY=0`, `--env DASH_XAM_FIBER_FIX=0`. Références : `<copie principale>/work/banc/refs/2.0.13604.0-xam` ;
  après un changement VOULU : deux `run`, regarder les captures, puis `refs <exécution> --calib <autre>` ; `init` refait l'instantané.
- **Point d'arrêt matériel en écriture, dans le processus** (qui a écrit cette valeur ?) : `perf_event_open` `PERF_TYPE_BREAKPOINT`, `HW_BREAKPOINT_W`, `bp_addr` = adresse HÔTE (base invitée + adresse), `sigtrap = 1`, `remove_on_exec = 1`,
  `inherit = 1`, `inherit_thread = 1`, ouvert sur CHAQUE fil de `/proc/self/task` ; le gestionnaire SIGTRAP ne traite que `si_code == TRAP_PERF` (`backtrace_symbols_fd`) et chaîne les autres. Marche avec `ptrace_scope = 1` et
  `perf_event_paranoid = 2`, sans gdb ; pile dans `out.log`. Code temporaire dans `dashboards/<variante>/src/` (pris par GLOB), à retirer ensuite. A trouvé l'écriture de `XamLoaderGetDvdTrayState` (prototype faux) dans une vtable.
- **Ghidra** : `tools/ghidra/xd_ghidra.py decompile xam <fonction>` ([ghidra.md](ghidra.md)) ; y consigner tout nom établi dans `symbols/2.0.13604.0/`.
- Vérifier chaque résultat visuel par **capture regardée** (Xvfb + lavapipe), jamais par le seul journal. Écrans X dédiés par tâche, `ulimit -c 0`, `XKEYS_HOLD=0.12`.
- Shaders natifs : `DASH_NGPU_SHADERS`, `DASH_HUD_SHADERS` (sinon `<game_data_root>/shaders/{dash,hud}`, générés à l'import) ; 258/258 dash et 34/34 HUD traduits.
  Modules XAM/HUD : `python3 tools/regen_xam_modules.py` après tout changement de manifeste ou d'overrides, puis compilation (il lance déjà `xam_unimpl_patch.py`).
- Builds longs en arrière-plan, attente BORNÉE ; jamais deux builds lourds simultanés ; un seul agent par groupe de fichiers.

## Faits récents

- **Une seule variante Kinect** `dashboards/2.0.13604.0-xam`, XAM recompilé par défaut (l'ancienne variante HLE est supprimée).
- **Fuseau horaire** : XConfig 3/1..7 vient de l'hôte (`TZ` ou `/etc/localtime`, `common/src/host_timezone.h`), rapproché de la table des fuseaux du Dashboard (cherchée dans l'image chargée, `timezone_table.h`). Un nom absent de la table
  (« CET ») faisait réécrire GMT au démarrage. **17559 : noms laissés vides** (`kernel_xconfig.cpp`) : plantage de Clock non expliqué (cause non trouvée), détail dans `git show d96c252:docs/REPRISE.md`.
- **Police** : `dash_font_patch.cpp` ne remplace la police principale (JKLatin) par XenonSCLatin que si `flash/XenonSCLatin.xtt` est importée ; sinon tout le texte disparaissait. Le glyphe Gamerscore U+E00A n'est que dans SCLatin ; `DASH_FONT_PATCH=0` désactive.
- **KeBugCheck fatal** (SDK `7f60045f`) : `[FATAL] *** STOP: 0x<code> ... appelant lr=0x…` puis `abort()` ; `xd_ghidra.py decompile <module> 0x<lr>` donne l'appelant. Le recompilateur termine aussi le bloc après un `bl` vers KeBugCheck.
- **Fibres de XAM** (`src_xam_patch/xam_fiber_switch.cpp`) : `sub_819BF2E0` bascule vraiment de pile (ucontext) ; sans cela un fil tourne à 99 % d'un cœur ; `DASH_XAM_FIBER_FIX=0` désactive.

## Pièges

- **« Enter pass code »** : `signin.xex` compare `RtlCompareMemoryUlong` à 4 ; une `librexruntime.so` sans le correctif SDK `7151265f` renvoie 1. Le binaire a `RUNPATH=$ORIGIN` sans bibliothèque voisine : il prend `LD_LIBRARY_PATH`. Utiliser `sdk/lib/`.
- **Stubs du SDK qui n'écrivent pas leurs sorties** (`REX_EXPORT_STUB` laisse r3, souvent le pointeur de sortie = « succès » ; l'appelant lit des résidus de pile) : écran qui varie d'un build à l'autre ; chercher d'abord les `STUB` du
  journal. Sans argument, r3 garde `this` (< 0x80000000), lu comme HRESULT de succès (`XamIsIptvEnabled`). Un prototype FAUX est pire (`XamLoaderGetDvdTrayState` écrivait dans l'objet de l'appelant). Cas : liste des résolutions
  (`kernel_video_output.cpp`).
- **Touches** : le mappage est celui du Dashboard (`common/src/dash_input.h`) : `Entrée` = A, `P` = Start, `Tab` = Back. Avant de conclure à un gel, vérifier qu'une autre touche change l'image.
- **Journaux** : `dash.log` n'est pas stdout (`[ngpu]` va sur stderr ; lire les deux, `> run.out 2>&1`). Demander le FICHIER `~/.local/share/xenondash/2.0.13604.0-xam/dash.log` à l'utilisateur, pas un collage de console.
  `DASH_GUIDE_TRACE=1` (ouverture du Guide), `DASH_LOGON_TRACE=1` (tâche de connexion `sub_818B2BF8`), `DASH_HUD_RESOLVES=1` (formes de Resolve du HUD).
- Tas invités 0x8… et 0x9… : deux vues du même fichier (`REX_ALIAS_XEX_HEAPS=1` rétablit l'ancien mode) ; une `function_table_base` par module, sans recouvrement en adresses PHYSIQUES.
- **Pas de `pgrep -f`/`pkill -f`** (ils se retrouvent eux-mêmes) : `pkill -9 -x dash` ou PID, toute attente `while pgrep` bornée. Plusieurs instances de dash : touches vers la mauvaise fenêtre et OOM (~2 Go d'arène chacune) ;
  `pgrep -c -x dash` avant d'interagir.
- `tools/dash_shaders.py` attend `xxhsum` ≤ 0.8.1. Les instrumentations dans `dashboards/*/generated/**` sont perdues à la régénération : sondes temporaires seulement, le correctif durable va dans `common/src`, `native_gpu` ou le SDK.
- Fork du SDK : sous-module `third_party/xenondash-sdk`, branche `main` ; après modification : reconstruire, réinstaller dans `sdk/`, commiter le sous-module PUIS le dépôt. Dans un worktree, le sous-module peut être une copie sans git :
  `git init`, `git fetch` du commit épinglé ET des étiquettes `v*` (sinon la version devient `0.10.0.0-dev.unknown`), `git reset` sur ce commit.

## Suite

Voir la section **To-Do** de [PLAN.md](../PLAN.md), seule liste du projet.
