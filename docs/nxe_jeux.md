# Jeux depuis le Dashboard NXE 2.0.13604 (lot C)

Date : 2026-11-12. Tout se passe depuis le Dashboard : l'AppImage NXE crée un dossier de jeux, les jeux déposés y sont reconnus (outils
`tools/xcontainer.py` et `tools/game_library.py`, voir [bibliotheque.md](bibliotheque.md)), A sur un jeu le recompile en natif si nécessaire
(progression à l'écran) puis le lance ; on revient au Dashboard à la fin du jeu. Aucune ligne de commande, aucun `games.toml` à écrire, aucune émulation.

## 1. Parcours utilisateur

1. Lancer l'AppImage (`XenonDash-2.0.13604.0-x86_64.AppImage`). Premier lancement : import de la mise à jour système (voir README, « AppImage »), puis
   `AppRun` crée le **dossier de jeux** `~/XenonDash/Games` (ou `$XENONDASH_GAMES_DIR`) et le dit (console et `notify-send` s'il existe) ;
   `AppRun --games-dir` l'affiche.
2. Déposer les jeux dans ce dossier : ISO, dossiers extraits (avec `default.xex`), paquets XBLA (STFS), GOD, liens symboliques (formats : bibliotheque.md §1).
3. Le Dashboard lance `game_library.py init` puis `scan` à chaque démarrage (cache incrémental du scan : une fraction de seconde si rien n'a changé ; au-delà
   de 4 s le scan continue en arrière-plan et le registre est relu à sa fin). **F6** relance un scan ; **F5** ouvre la liste des jeux (Haut/Bas, A pour lancer).
   Une petite fenêtre en haut à gauche indique l'état du scan, le nombre de jeux et le dossier.
4. Le jeu apparaît dans **My Xbox** (tuile de disque « Play <jeu> », « Latest Games ») et **Game Library > Recent Games** avec nom et icône (voir §3 pour la limite).
5. A sur la tuile « Play <jeu> », ou Game Library > <jeu> > **Play Game**, ou Y dans Recent Games : si le jeu n'a pas encore de binaire en cache, le Dashboard lance
   la recompilation (processus détaché), reste utilisable et affiche une fenêtre : étape *n / 8*, barre de progression, temps écoulé / estimé / restant, journal.
   À la fin le jeu se lance seul (le Dashboard cède la place puis est relancé au retour : `$APPIMAGE` est relancé, pas le montage qui a disparu).
6. Refus (jeu non pris en charge par le mode générique : `supported=false` + `reason` de `recipe_build.py launch-info`) ou échec (espace disque, compilateur absent...) :
   la même fenêtre affiche le message en rouge (Échap / B la ferme), le Dashboard reste utilisable (les fonctions de lancement renvoient `E_FAIL` au hub).
   Compilateur de l'hôte absent : le message nomme les paquets (`clang`, `cmake`, `ninja` ; Arch `pacman -S clang cmake ninja`, Debian/Ubuntu `apt install clang cmake ninja-build`).

## 2. Registre fusionné (`common/src/dash_launch.cpp`)

`Games()` charge `<game_data_root>/games.toml` (manuel, **prioritaire** : un TitleID déjà vu n'est pas écrasé) puis `games.auto.toml` de la bibliothèque
(`--dash_library_toml`, `$XENONDASH_LIBRARY_TOML`, défaut `${XDG_DATA_HOME:-~/.local/share}/xenondash/library/games.auto.toml`). Le résultat est mis en cache
(date et taille des deux fichiers) : relu seulement si l'un d'eux change. Les jeux de `games.auto.toml` sont `from_library`, listés comme contenu installé
(`LibraryEnabled()` est vrai dès que le fichier existe), icônes en chemin absolu acceptées. Le scan au démarrage n'est actif que sous AppImage
(`XENONDASH_AUTOSCAN=1`, posé par `AppRun`) ou avec `--dash_library_scan` : un lancement de développement ne crée ni dossier ni données.

## 3. Ce que fait « Play Game » dans le hub 13604 (investigation)

Méthode : gdb en mode batch avec un script Python qui pose un point d'arrêt (non bloquant) sur chacun des 353 imports `__imp__X*` utilisés par `dash.xex`
(`dash_init.cpp`) et journalise horodaté `r3..r6` ; on appuie sur A, on lit la tranche de journal. Le bruit (XAudio, `XamUserGetXUID`, `XNotifyGetNext`...) est filtré.

**Cause trouvée : `XamLoaderGetMediaInfoEx` était un stub du SDK** (`REX_EXPORT_STUB`, r3 inchangé, sorties non écrites). Le hub l'appelle
(`sub_92236178`, `sub_922447F8`, `sub_92244748`, `sub_923133E0`) avec trois pointeurs de sortie avant toute action sur le disque : A sur la tuile, Play Game, Eject.
Il lit la 3e sortie et la compare à 0 et à -2 ; avec des valeurs de pile aléatoires il abandonnait en silence (aucune fonction XAM de lancement journalisée,
d'où le constat du README). Le trace de l'appui A sur la tuile de disque montrait, juste après `XamContentGetDeviceData`, `XamLoaderGetMediaInfo` puis `XamLoaderGetMediaInfoEx`
(r3=&a, r4=&b, r5=&c), puis plus rien.

**Correctif** (`dash_launch.cpp`) : `XamLoaderGetMediaInfoEx(&a, &b, &c)` écrit a = type de média (1 = disque de jeu Xbox 360, 0 sans disque), b = TitleID du disque,
c = 0 ; retour 0. Résultat vu à l'écran et au journal : A sur « Play Skate » et Game Library > Skate > Play Game appellent tous deux
`XamLoaderLaunchTitleEx(NULL, 0x12)` (chemin NULL = « lancer le disque » ; r6 = 0x12), que `Resolve()` associe au jeu du disque -> `LaunchGame`.
Les trois imports de lancement (`XamLoaderLaunchTitle`, `...Ex`, `...ForReason`) renvoient maintenant `E_FAIL` s'ils reviennent (construction en cours, refus).

**Liste de la Game Library : décidée côté natif (vu, lot C2).** Le hub ne liste un jeu que s'il a un enregistrement de titre, créé par une tâche de
chargement (`CLoaderTask`) pour chaque contenu de l'énumération agrégée (`XamContentAggregateCreateEnumerator`). Cette tâche n'est lancée que si l'élément passe deux
contrôles : type de contenu accepté (0x4000, 0x5000, 0x7000, 0x80000, 0xD0000) et bits 0 et 1 du mot à +506 nuls. Le Dashboard lit 512 octets par élément ;
notre énumérateur n'en écrivait que 0x150, le reste était de la pile non initialisée. `common/src/dash_content.cpp` écrit désormais des éléments de 512 octets,
remplis à zéro. Résultat vu : sans disque, Recent Games liste les 4 jeux du registre avec nom et icône (`shots/t1_lib.png` du lot C2). Il n'y a pas de script Lua
en cause : la version précédente de ce paragraphe, qui le supposait, était fausse.

Type affiché : tous les jeux restent « Arcade Game » (0xD0000). Le type 0x7000 pour les jeux disque (« Xbox 360 Game ») a été essayé puis retiré : les icônes de Skate
disparaissaient, et le journal ne montre plus de montage du contenu (`XamContentCreateInternal`) pour lui (`shots/t3_lib.png`). Cause non établie.

**Repli dans l'interface** (ce qui est livré) :
- le dernier jeu lancé de la bibliothèque (sinon le premier par ordre alphabétique) est présenté comme **disque inséré** (`dash_auto_disc`, vrai par défaut ;
  `disc = true` dans `games.toml` l'emporte) : tuile « Play <jeu> », Recent Games, page du jeu et Play Game fonctionnent ;
- les **autres jeux** sont lançables par **F5** (liste de tous les jeux du registre, A lance) ; après un lancement, le jeu devient le disque du démarrage suivant.

Jeux jamais lancés : le profil local est neuf (aucun GPD). `AllTitles()` (`dash_achievements.cpp`) ajoute en mémoire à la liste des jeux joués les jeux de la bibliothèque absents
de `FFFE07D1.gpd` (nom UTF-8 -> UTF-16, 0 succès, icône du scan servie par `XamReadTile(Ex)` type 1) ; la date de dernier lancement va dans `<profil>/library_played.txt`
(`TitleID FILETIME`) quand le jeu n'a pas d'entrée GPD. Rien n'est écrit dans les GPD. Vu à l'écran (profil vide) : « Latest Games » avec l'icône et Recent Games « Skate / Xbox 360 Game ».

## 4. AppImage autonome pour recompiler

`tools/make_appimage.sh 2.0.13604.0` embarque, sous `usr/share/xenondash/` : outils Python (`tools/*.py`, `d3d_signatures/`, `templates/`, `xexinfo`), `games/` (recettes, sans `tests/`),
`ngpu_core/`, `native_gpu/`, `patches/`, le SDK installé `sdk/` (rexglue, en-têtes, bibliothèques, cmake), XenosRecomp et `libdxcompiler.so`/`libdxil.so`. Les listes `TOOLS_DIRS` / `ROOT_DIRS` du script
sont faites pour être étendues. `--no-build-tools` produit l'AppImage légère (sans recompilation de jeux).

Lecture seule : `AppRun` crée un lien stable `~/.local/share/xenondash/runtime-<version>` vers `usr/share/xenondash` et l'exporte comme `XENONDASH_ROOT` (`ToolsRoot()` du Dashboard) ;
`recipe_build.py` déduit sa racine de son propre chemin (le lien), donc les projets compilés gardent des chemins valides d'un lancement à l'autre ; tout ce qui s'écrit
va dans `~/.local/share/xenondash/games/<TitleID>/<clé>/` (cache) ou le dossier de jeux. `XD_SDK`, `XD_XENOSRECOMP`, `LD_LIBRARY_PATH` (dxc) sont posés par `AppRun`.
Prérequis hôte : python3 (3.11+ pour `tomllib`), clang, cmake, ninja (non embarqués : clang 115 Mo + llvm-libs 165 Mo + cmake 101 Mo installés sur cette machine, soit environ 380 Mo non compressés),
un GPU Vulkan, de l'espace disque (la recette demande 40 Go libres, la construction de Skate en pèse 0,6).

Taille mesurée : **49,9 Mo** (AppImage complète, 142 Mo non compressés d'outils) ; **19,8 Mo** avec `--no-build-tools` (contre 48,7 Mo avant que `XenosRecomp` n'embarque `shader_common.h`, lu à l'exécution : sans lui XenosRecomp plante en SIGSEGV).

## 5. Test de bout en bout

`dashboards/2.0.13604.0/e2e_jeux.sh` (Xvfb `:94`, lavapipe, HOME temporaire, import du firmware, dossier de jeux avec un lien vers Skate, `taskset` 6 cœurs) : voir §6 pour les résultats.

## 6. Résultats mesurés et limites

Test de bout en bout `e2e_jeux.sh` (AppImage complète, HOME vierge, firmware importé, lien vers `Skate1Recompiled/game` dans `~/XenonDash/Games`, Xvfb :94 + lavapipe, 6 cœurs, machine chargée par d'autres compilations) :
- scan au démarrage : « scan terminé (1 jeux) » 2 s après le lancement ; profil vide ;
- My Xbox > Game Library : Recent Games « skate. » (nom lu dans le XEX, icône du scan, « Xbox 360 Game ») ; page du jeu (Play Game / Install Game) ; A sur Play Game ;
- recompilation à froid **1 517 s (25 min)** au lieu de 12 min mesurées sans charge (lavapipe du Dashboard à 400 % CPU + autres compilations sur la machine) : fenêtre ImGui visible sur le Dashboard
  (étape 5/8 codegen, barre 52 %, écoulé 1 min 51 s sur 44 min estimées, journal), 26 captures `06_progression_*` ; à la fin « LANCEMENT de 45410813 » tout seul ;
- Skate natif à l'écran (`07_jeu.png`), jeu tué par PID, « retour au Dashboard après le jeu 45410813 » et Dashboard affiché (`08_retour_dashboard.png`), 0 FATAL ;
- refus : jeu dont la source est inutilisable -> fenêtre rouge sur un Dashboard relancé et utilisable (capture hors AppImage, Dashboard de développement).

Relance du Dashboard : le hub NXE prend `XamLoaderLaunchTitle*` pour un lancement réel et reste noir (écran noir de plusieurs minutes pendant la construction, constaté), quoi que renvoie le
hook (`E_FAIL`). Pour un appel venu du hub, `LaunchGame` relance donc le Dashboard tout de suite (superviseur détaché, comme au retour d'un jeu) et lui passe l'état par
`~/.local/share/xenondash/current_build.json` (pid, cache de la construction en cours, ou message de refus à afficher une fois) ; la nouvelle instance reprend le suivi (`ResumeBuild`, `progress.json`)
et lance le jeu seule à la fin. Un lancement par F5 garde le suivi dans le même processus. Vérifié : reprise du suivi puis échec simulé (construction tuée par PID : message, fichier d'état supprimé).

Limites et reste :
- liste Game Library : sans disque, tous les jeux du registre sont listés (vu) ; tous sont « Arcade Game », jeu disque compris (voir §3) ;
- lancement non vérifié de bout en bout : A puis Play Game sur Skate (`45410899`) envoie la requête au hook (`XamContentLaunchImageInternalEx`, journal), mais la
  construction échoue faute d'espace (« 12 Go libres, 40.0 Go necessaires », /tmp en tmpfs de 16 Go) ; le hub affiche « Can't Play Game ». Pas de « LANCEMENT » vu.
  Le cache de Skate (`45410813`) n'existe pas sur cette machine : pas de test de bout en bout de Skate ;
- Trials Fusion (`58411401`) : pas de refus `supported=false` (`launch-info` ne renvoie pas ce champ) : la construction tente, puis échoue sur l'espace disque ;
  message rouge lisible (`shots/t5_after.png`) ; retour au Dashboard pilotable non vérifié (le dialogue XUI reste affiché) ;
- jeu disque simulé (`dash_auto_disc`, vrai par défaut) : pas de doublon dans Recent Games (Trials une fois) ; la tuile de My Xbox et « All Games » ne sont pas vérifiées ;
- les autres jeux via F5 (voir §3) ;
- pourcentage et estimation (44 min) viennent de `recipe_build.py` (pas de ma compétence) ; le pourcentage saute à 51 % au début du codegen ;
- relance par `$APPIMAGE` : les options données à l'AppImage (hors `AppRun`) ne sont pas rejouées ; sous `APPIMAGE_EXTRACT_AND_RUN=1` l'extraction est conservée tant que l'AppImage tourne ;
- jeux GOD, ISO, STFS : reconnus par le scan (lot A, fichiers synthétiques) mais seule la source « dossier » a été recompilée ici ; ISO/GOD/STFS passent par `xcontainer.py extract` dans `recipe_build.py` (lot B), non testés de bout en bout ;
- `XamLoaderGetMediaInfoEx` : la sémantique des trois sorties est déduite des comparaisons du hub (0 / -2), non documentée ; sans disque, a = b = c = 0 (la valeur -2 faisait afficher « aucun jeu ») ;
- notification de média sans disque (paramètre 0) diffusée aussi ; 13604 et 6770 recompilés ; smoke 6770 repassé (0 FATAL, Skate dans Games) ; 6717/17559 non recompilés ici ;
- clang/cmake/ninja non embarqués ; ImGui du Dashboard en police par défaut (pas de fenêtre XUI d'origine) ;
- l'AppImage de test écrit son cache dans le HOME de test ; `e2e_jeux.sh` demande un disque (la tmpfs de 16 Go est refusée par la vérification des 40 Go de la recette).
