# Dashboard Xbox 360 recompilé en natif Linux

Recompilation statique de `dash.xex` (firmware 2.0.6770.0) en C++ natif avec
[ReXGlue](https://github.com/rexglue/rexglue-sdk) (runtime dérivé de Xenia) et des outils de
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

## Mise en place

Le dépôt ne contient **aucun fichier Microsoft** (firmware, XEX, polices, ressources) ni le C++
généré à partir de `dash.xex` : ils sont à fournir / régénérer localement.

```sh
# Dépendances (versions testées)
# SDK ReXGlue : sous-module git (fork Forgejo, branche xenondash = c94f5eb + 3 correctifs)
git submodule update --init --recursive
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git third_party/XenonRecomp
git -C third_party/XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace

# Arch : sudo pacman -S --needed clang cmake ninja libxi gtk3 libxss libxtst libpulse alsa-lib libxkbcommon xorg-server-xvfb
cmake -S third_party/rexglue-sdk --preset linux-amd64 -DCMAKE_INSTALL_PREFIX=$PWD/sdk
cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target install

# Outil d'analyse XEX
cmake -S tools/xexinfo -B tools/xexinfo/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENONRECOMP_DIR=$PWD/third_party/XenonRecomp
cmake --build tools/xexinfo/build
```

Fichiers à placer (depuis sa propre console, ou les mises à jour système officielles) :

| Destination | Source |
|---|---|
| `dashboards/2.0.6770.0/assets/dash.xex` | `$flash_dash.xex` de la mise à jour 2.0.6770 (`tools/stfs.py` sur `su20076000_00000000`) |
| `dashboards/2.0.6770.0/assets/flash/*.xex` | modules flash 6770 (mise à jour + flash console) |
| `dashboards/2.0.6770.0/assets/flash/XenonJKLatin.xtt`, `XenonCLatin.xtt` | polices de la flash 6770 |
| `dashboards/2.0.6770.0/assets/flash/shrdres.xzp` | ressource `shrdres` de `xam.xex`, **convertie** (voir ci-dessous) |

`shrdres.xzp` (icônes, images de joueur par défaut, tables de chaînes partagées) est une ressource de
`xam.xex`. Celui de la 6770 n'étant pas déchiffrable ici, on prend celui de la mise à jour officielle
2.0.17559 et on le convertit aux formats lus par le Dashboard 6770 :

```sh
tools/xexinfo/build/xexinfo 'firmware/17559/$flash_xam.xex' work/xam17559.pe
python3 tools/xex_resource.py 'firmware/17559/$flash_xam.xex' work/xam17559.pe shrdres work/shrdres_17559.xzp
python3 tools/xzp_alias.py work/shrdres_17559.xzp dashboards/2.0.6770.0/assets/flash/shrdres.xzp
```

## Arborescence

| Chemin | Contenu |
|---|---|
| `firmware/<version>/` | Mise à jour système extraite (paquet STFS `su20076000_00000000`) |
| `common/` | Bibliothèque `xenondash_common` (OBJECT CMake), partagée par toutes les versions : classe `xenondash::DashAppBase` (flash, OnPreSetup/OnPostSetup), profil, GPD/succès, entrées, audio silencieux, interfaces XAM ImGui, stubs noyau |
| `dashboards/<version>/` | Un projet ReXGlue par version : manifeste, corrections d'analyse, `src/` propre à la version (`main.cpp`, `app.h`, `version_stubs.cpp`, `diag_hooks.cpp`), scripts de test |
| `dashboards/2.0.6770.0/assets/dash.xex` | Exécutable d'entrée |
| `dashboards/2.0.6770.0/assets/flash/` | Contenu de la flash console, monté comme `\SystemRoot` (`media:`) |
| `games.example.toml` | Modèle du registre de jeux `games.toml` (voir « Lancer des jeux ») |
| `sdk/` | SDK ReXGlue installé (compilé depuis `third_party/rexglue-sdk`, **patché**, voir plus bas) |
| `tools/` | Outils d'analyse (Python/C++) |
| `work/` | Sorties d'analyse (image PE décompressée, imports, candidates) |

## Construire et lancer

```sh
# 1. Génération du C++ (≈30 s) puis compilation (≈10 min la première fois)
cd dashboards/2.0.6770.0
../../sdk/bin/rexglue codegen dash_manifest.toml
cmake --preset linux-amd64-relwithdebinfo        # une seule fois
cmake --build --preset linux-amd64-relwithdebinfo -j12
ln -sf ../../../../../../sdk/lib/librexgpu-xenos.so out/build/linux-amd64-relwithdebinfo/

# 2a. Sur un bureau (X11/Wayland)
LD_LIBRARY_PATH=../../sdk/lib out/build/linux-amd64-relwithdebinfo/dash --game_data_root=assets

# 2b. Sans écran ni carte son (SSH) : Xvfb + capture dans runs/<nom>.png
./run_headless.sh 40 essai
```

Journaux : `dashboards/2.0.6770.0/out/build/linux-amd64-relwithdebinfo/logs/`. `--log_level=trace` pour tout voir.
`DASH_AUDIO=nop` active le backend audio silencieux (`common/src/null_audio.h`).

## Porter une nouvelle version

`tools/port_dashboard.sh` enchaîne tout le portage d'une version (phase P2 du PLAN) :

```sh
tools/port_dashboard.sh <mise_à_jour> <version> [--flash-dir <modules XeBuilds>] [--shrdres-xam <xam.xex complet>]
# ex. (6770, comme à la main) :
tools/port_dashboard.sh '/chemin/$SystemUpdate' 2.0.6770.0 --flash-dir firmware/xebuild-6770
```

`<mise_à_jour>` : dossier `$SystemUpdate` (ses paquets STFS sont extraits avec `tools/stfs.py`), dossier de fichiers
déjà extraits (`$flash_dash.xex`...), paquet STFS ou `dash.xex` seul. Les fichiers intermédiaires vont dans
`work/port/<version>/` (journaux de codegen et de compilation compris), le projet dans `dashboards/<version>/`
(`--dest` pour ailleurs ; `dashboards/*-regen/` est ignoré par git, pratique pour tester).

| Étape (`--stages`) | Action |
|---|---|
| `extract` | extraction STFS, repérage de `dash.xex` (`$flash_dash.xex`) |
| `assets` | `assets/dash.xex`, `assets/flash/` (modules complets de la mise à jour + `--flash-dir`, polices `XenonCLatin.xtt`/`XenonJKLatin.xtt`), `shrdres.xzp` converti (`--shrdres-xam`, défaut `firmware/17559/$flash_xam.xex`, ou `--shrdres` déjà converti) |
| `pe` | `tools/xexinfo` : image PE décompressée, base, sections (`.text`, `.rdata`, `.data`) |
| `scaffold` | gabarit `tools/templates/dashboard/` (`CMakeLists.txt` générique, `CMakePresets.json`, `dash_manifest.toml`, `src/app.h`, `src/main.cpp`, `run_headless.sh`, `dash_overrides.toml` vide). N'écrase jamais un fichier existant (`--refresh-template` pour le forcer) |
| `codegen` | `rexglue codegen` en boucle : chaque erreur *UnresolvedCall* ajoute sa cible aux fonctions ; puis `tools/find_ptr_funcs.py` + `tools/filter_inside.py` (fonctions atteintes par pointeur ou construites par `lis/addi` ; les continuations de `catch` et cibles des tables de portée SEH, situées dans une fonction, sont écartées) jusqu'à ce qu'il n'y ait plus de nouvelle fonction. Résultat : **`overrides.toml`** (généré, versionné, ne pas éditer) |
| `build` | `cmake --preset linux-amd64-relwithdebinfo` + compilation ; si l'édition de liens signale des `sub_XXXXXXXX` non définis (imports d'ordinal inconnu), écrit `src/auto_stubs.cpp` (`REX_STUB_RETURN(..., 0)`) et recommence |
| `smoke` | Xvfb sur un affichage libre + lavapipe (`MESA_VK_WSI_PRESENT_MODE=immediate`), `--smoke-seconds` (40) secondes, capture `runs/<--smoke-name>.png`, échec si `dash` est mort, si une ligne `FATAL`/`[critical]` est dans le journal ou si la capture est uniforme ; `--reference-png` compare à une capture de référence (`--max-diff`) |
| `compare` | avec `--compare-with <projet>` : tables de fonctions et adresses d'overrides |

Code de retour non nul à la première étape en échec (message sur stderr, journaux dans `work/port/<version>/`).
`--from-stage codegen` reprend à une étape, `--no-smoke` saute le test.

**Corrections manuelles** (par version, versionnées à la main) : `dash_overrides.toml` (bornes de fonctions, hooks
`[[midasm_hook]]`...) et `src/*.cpp` supplémentaires (le `CMakeLists.txt` générique compile tous les `src/*.cpp` :
`version_stubs.cpp`, `diag_hooks.cpp`...). Le code commun à toutes les versions est dans `common/` ; les deux
fichiers d'overrides sont listés dans `includes` du manifeste. Pour 2.0.6770.0, seuls `sub_92739D9C`
(`version_stubs.cpp`, redondant avec les stubs automatiques) et `diag_hooks.cpp` (+ ses hooks) sont à la main.

**Critère P2 vérifié sur 6770** : régénéré depuis le paquet STFS `su20076000_00000000` + `firmware/xebuild-6770` dans
`dashboards/2.0.6770.0-regen/` : mêmes 24 669 fonctions, mêmes 342 adresses d'overrides (le projet manuel avait en plus
les deux débuts de dispatchers `0x92249ED8` et `0x92249F38`, sans effet), code C++ généré identique fonction par
fonction (hors les `extern` des 5 hooks de diagnostic), compilation sans erreur, capture équivalente (fond animé : on
compare l'écart moyen, ~0,5/255 contre ~1,7 entre deux lancements du même binaire), aucun FATAL. Essai sur 2.0.6717.0 :
codegen, compilation et smoke test passent sans intervention.

## Versions portées (phase P3)

Chaque version est un projet `dashboards/<version>/` produit par `tools/port_dashboard.sh` (les mises à jour sont
rangées dans `firmware/<version>/`, les modules flash XeBuilds de 6717 et 7258 dans `firmware/xebuild-<n>/` ;
l'archive XeBuilds ne contient pas 5759, 4532 ni 2255, on y utilise les polices et modules 6717). Test à l'écran :
`dashboards/<version>/input_test.sh` (profil de test copié depuis 6770 par l'outil).

| Version | État | Particularités |
|---|---|---|
| 2.0.6770.0 | OK (référence) | voir plus haut |
| 2.0.6717.0 | **OK** : Games avec profil (Gamerscore 155), Achievements (9 succès), Played Games (3 jeux, sauvegardes) | 7 cibles non résolues + 334 fonctions par pointeur ; 1 stub d'import auto ; une continuation de funclet `finally` (`0x9222D1B0`) déclarée par l'outil (sans elle : `FATAL Unresolved call` en ouvrant Games Library). Captures : `dashboards/2.0.6717.0/runs/preuves/` |
| 2.0.7258.0 | **partiel** : démarre et s'affiche (interface « Xbox 360 / Xbox Live » en tuiles, type NXE avant la lettre, **pas** les Blades), profil détecté (« 1 Profile Found, Signed In Locally »), navigation vers Game Library / Recent Games | import de fonction **sans thunk** (`XamVoiceSubmitPacket`, adresse 0) : correctif du SDK (`codegen` l'ignore) sans lequel `No function registered at <entrée>` ; Achievements/Played Games non atteints (écran de profil derrière la tuile « Sign In », non géré en HLE) ; artefacts roses de lavapipe. Captures : `dashboards/2.0.7258.0/runs/preuves/` |
| 2.0.5759.0 | **partiel** : codegen (12 cibles, 299 fonctions par pointeur) et compilation OK, **écran noir** au démarrage | déréférencement nul (invité `0x8`) pendant l'initialisation XUI, après le second `XamProfileCreateEnumerator` (`sub_9226CE00`, message XUI vers un objet dont le champ `+12` est nul) ; non résolu. Ce Dashboard ouvre `media:\gamercrd.xzp` (ressource de `xam.xex`, absente de la flash) : l'outil en extrait désormais une version convertie de XAM 17559 (format de scènes probablement incompatible) |
| 2.0.4532.0 | **partiel** : codegen (10 cibles, 285 fonctions par pointeur) et compilation OK, écran noir | même symptôme que 5759 |
| 2.0.2255.0 | **impossible** | la mise à jour ne contient que des deltas `.xexp` (dash compris) appliqués sur la base 1888, dont les modules sont chiffrés et dont on ne dispose pas de clé ; l'outil s'arrête proprement (« dash.xex introuvable ») |

Les modules `xam.xex` de XeBuilds (6717, 7258) portent la version 1888 : XAM reste émulé (HLE) pour toutes les versions
Blades. Corrections génériques ajoutées à l'outil pendant P3 : continuations de funclets (le funclet `finally` se
termine par un `b` vers sa fonction parente : le point d'arrivée est déclaré comme fonction jusqu'au premier
`blr`/`b`, section « continuations de funclets » d'`overrides.toml`), copie du profil de test et de `input_test.sh`
dans le projet, extraction de la ressource `gamercrd` de XAM. 6770 en avait aussi une (`0x9222D328`), ajoutée à
son `overrides.toml` ; Achievements revérifié à l'écran après reconstruction.

## AppImage (phase P6)

Un AppImage par version, construit à partir du binaire déjà compilé (preset `linux-amd64-relwithdebinfo`) :

```sh
tools/make_appimage.sh 2.0.6770.0      # -> out/appimage/XenonDash-2.0.6770.0-x86_64.AppImage (out/ non versionné)
tools/test_appimage.sh 2.0.6770.0 xebuild-6770   # test : HOME temporaire, import depuis firmware/, Xvfb + lavapipe
```

`appimagetool` est téléchargé dans `work/appimage-tools/` s'il manque. Contenu (≈ 17 Mo) : `usr/bin/dash` (strippé),
`librexruntime.so`, `librexgpu-xenos.so` (SDL3, spdlog, ffmpeg, etc. sont liés statiquement ; libc, libstdc++, X11/xcb/
wayland, loader Vulkan et pilote GPU restent ceux du système), `AppRun`, `.desktop`, icône originale
(`tools/appimage/xenondash.svg`), `games.example.toml`, et les scripts d'import (`port_dashboard.py` pour les étapes
`extract`/`assets`, `stfs.py`, `xex_resource.py`, `xzp_alias.py`, l'outil `xexinfo`). **Aucun fichier Microsoft**
(ni `dash.xex`, ni modules flash, polices, `shrdres`).

**Prérequis** : Python 3 système, un GPU Vulkan, FUSE 2 (sinon `APPIMAGE_EXTRACT_AND_RUN=1`), et vos propres fichiers
Microsoft : la mise à jour système officielle de la version (dossier `$SystemUpdate` ou paquet STFS), idéalement
les modules/polices complets (archive XeBuilds) et, pour les icônes, une mise à jour récente (17559) contenant
`$flash_xam.xex`.

**Premier lancement** : si `~/.local/share/xenondash/<version>/` n'est pas prêt (`dash.xex` + `.ready`), `AppRun`
demande ces chemins par fenêtre (`zenity` ou `kdialog`) ou, à défaut, sur la console ; sans terminal ni
fenêtre, il affiche la commande à utiliser. Il lance ensuite `xenondash_import.py` (étapes `extract` + `assets` de
`tools/port_dashboard.py`), qui remplit ce dossier (`dash.xex`, `flash/` avec `shrdres.xzp`/`gamercrd.xzp` convertis,
`profiles/` vide : le profil est créé vierge par le Dashboard, `games.toml` minimal + `games.example.toml`),
puis démarre le Dashboard avec `--game_data_root=<ce dossier>` ; journal dans `<dossier>/dash.log`. Les lancements
suivants démarrent directement.

```sh
./XenonDash-2.0.6770.0-x86_64.AppImage --import '/chemin/$SystemUpdate' \
    --modules /chemin/xebuild-6770 --shrdres-xam '/chemin/17559/$flash_xam.xex'
./XenonDash-2.0.6770.0-x86_64.AppImage --help      # --reimport, --data-dir, options transmises à dash
```

`XENONDASH_DATA` change la racine des données. Vérifié sous Xvfb + lavapipe, HOME temporaire, import depuis
`firmware/2.0.6770.0` + `xebuild-6770` (+ 17559) : Dashboard affiché, aucun `FATAL` (captures
`dashboards/<version>/runs/preuves/p6_<version>.png`), idem 2.0.6717.0.

**Usage personnel uniquement** : le binaire recompilé contient une traduction du code Microsoft ; ces AppImages ne
sont **pas redistribuables** (ne jamais les versionner ni les publier).

## Lancer des jeux (phase P5)

Code : `common/src/dash_launch.cpp` (+ `dash_launch.h`, lanceur ImGui dans `dash_xam_ui.cpp`), donc commun à toutes
les versions. Exemple de registre versionné : `games.example.toml`.

**Registre** `<game_data_root>/games.toml` (ex. `dashboards/2.0.6770.0/assets/games.toml`, relu à chaque ouverture
du lanceur) : une table `[game.<TitleID hexa>]` par jeu avec `name` et soit `native = "commande shell"` (+ `cwd`,
relatif au répertoire du Dashboard ou absolu), soit `xenia = "chemin absolu du XEX/ISO"` (lancé avec la commande
Xenia : `--dash_xenia=...`, sinon variable `DASH_XENIA`, sinon `[xenia] command`, sinon `xenia_canary`).
Autres clés : `type` (`arcade` par défaut, `demo`, `original`, `xna` ou un nombre), `keep_ld_library_path`, `disc = true`
(ce jeu est « inséré dans le lecteur » : bouton de disque du Dashboard ; `--dash_disc=<TitleID>` ou `disc = "<TitleID>"`
à la racine équivalent), `icon = "fichier.png"` (PNG de 16 Ko au plus) ou `spa = "fichier.spa|.gpd"` (XDBF d'où tirer
l'icône du jeu, sinon `<TitleID>.gpd` du profil) pour l'icône du disque et de « My Games », et à la racine
`library = true` (jeux listés dans « My Games », voir plus bas).

**Comment lancer** :
1. **F5** dans le Dashboard (manette : Back + Y) : liste des jeux du registre, Haut/Bas, Entrée/A lance, Échap/B ferme
   (comme les écrans XAM remplacés : `XN_SYS_UI` et entrées du Dashboard suspendues) ;
2. tout appel du Dashboard à `XamLoaderLaunchTitle` / `XamLoaderLaunchTitleEx` : le titre est retrouvé par un
   TitleID présent dans le chemin, sinon c'est le jeu `disc` (ou le seul jeu déclaré) ; sans correspondance, l'appel
   est journalisé et ignoré ;
3. `--dash_launch=<TitleID> [--dash_launch_delay=5]` : lancement automatique au démarrage (tests).

**Mécanisme retenu** : le Dashboard **ne reste pas en veille**. Il lance un superviseur détaché (`/bin/sh -c`, nouvelle
session) et quitte (`_Exit`), ce qui libère fenêtre, GPU et audio pour le jeu (deux fenêtres Vulkan/SDL et deux
pilotes audio simultanés sont la cause habituelle de blocages). Le superviseur exécute
`( unset LD_LIBRARY_PATH; cd <cwd>; <commande> )` puis relance **le même exécutable avec les mêmes arguments**
(sauf `--dash_launch*`), avec `DASH_RETURNED_FROM=<TitleID>` dans l'environnement (journalisé). Le code de retour du
jeu est sans importance : le Dashboard revient toujours, qu'il se termine normalement ou soit tué. Conséquences :
le PID change, le Dashboard repart sur l'onglet Games (l'état du profil est sur disque), `LD_LIBRARY_PATH` (SDK du
Dashboard) est retiré de l'environnement du jeu, qui doit trouver seul ses bibliothèques (RPATH ou commande du type
`LD_LIBRARY_PATH=. ./skate1`). La commande exacte est dans le journal du Dashboard (« LANCEMENT de ... »,
« superviseur : sh -c ... »).

**Ce que le Dashboard 6770 appelle** (imports relevés dans `work/dash_imports.tsv` et le code recompilé) :
`XamLoaderLaunchTitle(chemin, indicateurs)` (0x9273988C), `XamLoaderLaunchTitleEx` (0x92739FCC),
`XamLoaderSetLaunchData` / `GetLaunchData`, `XamLoaderGetMediaInfo` (0x9273A05C), `XamLoaderGetDvdTrayState`,
`XamLoaderGetPriorTitleId`, `XamLoaderTerminateTitle`. `sub_921152A8` est l'enveloppe de `XamLoaderLaunchTitle` :
chemin NULL = « lancer le disque » (avec des données de lancement de type 16), les chaînes `XLNI_DET_MEDIA` et
`XLNI_DASH_ARCADE` sont des jetons internes. `sub_92274218` / `sub_9229E428` appellent `LaunchTitleEx` avec des
données de lancement (type 38 : relance d'un titre système). Les hooks de `dash_launch.cpp` remplacent
`LaunchTitle`/`LaunchTitleEx` du SDK, qui terminait le titre (donc le Dashboard) sans rien lancer.

**Bouton de disque et « My Games » (phase P5b, fonctionnels dans le 6770).** Analyse du code recompilé :

*Disque* (`common/src/dash_disc.cpp`, avec `XamLoaderGetMediaInfo` dans `dash_launch.cpp`). Le bouton « Unplayable
Disc » vient d'un état global (0x92803A8C) tenu par `sub_9210EB88` / `sub_9210E478` : le Dashboard dépile une
notification système **`0x8000000D`** dont le paramètre est le type de média (**1 = disque de jeu Xbox 360**, 2 = Xbox
d'origine), l'enregistre avec `XamLoaderGetDvdTrayState`, puis `sub_921B2340` lance, pour le type 1, un thread
(`sub_921B0DC8`) qui :
1. ouvre `\Device\Cdrom0\default.xex`, vérifie un en-tête XEX2 (SHA-1 de l'en-tête, résumé SHA-1 de l'image par pages de
   64 Ko, compression « basic », clés d'en-tête `0x40006` informations d'exécution, `0x2FF` ressources, `0x3FF` format de
   fichier ; `XexTransformImageKey` au passage), retrouve la ressource nommée `%08X` du TitleID ;
2. envoie `XMsgStartIORequest(0xFB, 0xB003C, {XDBF, taille, 40, tampon du nom, &taille de l'icône, tampon de l'icône})` :
   XAM renvoie le **nom du titre** (UTF-16, 40 caractères) et l'**icône PNG** (16 384 octets au plus).
Implémentation : un XEX synthétique (non chiffré, sans code, 64 Ko, aucun octet de Microsoft) est généré en temps réel
dans `$TMPDIR/xenondash-cdrom0-<uid>/` et monté comme `\Device\Cdrom0` ; `XexTransformImageKey` renvoie 0 ;
`XMsgStartIORequest(Ex)` est interposé pour le seul message `0xFB/0xB003C` (l'application 0xFB appartient déjà à XGI dans
le SDK, qui journalise « Unimplemented XGI message ») et répond avec le nom et l'icône du registre ;
`XamLoaderGetMediaInfo(&type, &TitleID)` renvoie 1 et le TitleID du disque ; la notification est diffusée **une seule
fois** (1,5 s après la création de l'écouteur de masque `0x23`) : la rediffuser (écouteur créé plus tard par la Games
Library) serait prise pour une nouvelle insertion et le Dashboard lancerait le jeu tout seul (lancement automatique).
Le bouton affiche alors le nom du jeu ; A appelle `XamLoaderLaunchTitleEx(NULL, 0x12)` -> jeu `disc` -> chemin P5.

*My Games* (`common/src/dash_library.cpp`, `dash_content.cpp`). La Games Library est une scène « Arcade:: »
(`2500_LiveArcadeHome.xur`) : pour chaque contenu énuméré de type Arcade (`0xD0000`), une tâche `CLoaderTask`
(vtable 0x9201F8B8, thread de travail ; la méthode par type est `sub_9222C608` pour Arcade, `sub_9222B550` fait la
vérification commune) :
1. `XamContentGetAttributesInternal(contenu, attributs, 0)` doit **réussir** (36 octets d'attributs, mis à zéro ici ;
   le stub qui renvoyait son argument faisait échouer la tâche, d'où « My Games » grisé) ;
2. `XamContentCreateInternal(« Content<n> », contenu, 3 = ouvrir l'existant, ...)` monte le paquet ;
3. lecture de `Content<n>:\ArcadeInfo.xml`, en **UTF-16 big-endian avec BOM** (`sub_92224fe0` saute le BOM puis lit des
   caractères larges : un fichier UTF-8 est lu comme du bruit) : `<ArcadeInfo><TitleInfo Id Name Path ImagePath MaxCred
   ScoreName CameraEnabled ContentNotDownloadable><Description>` (+ `Locale`/`GameInfo` optionnels) ;
4. `XamContentClose`.
Les jeux marqués dans le registre (avec `library = true`) deviennent des éléments virtuels de type `type` dans les
énumérations de contenu ; `XamContentCreateInternal` est interposé pour ces éléments (un répertoire temporaire
`xenondash-content-<uid>/<TitleID>/` avec l'`ArcadeInfo.xml` généré et l'icône est monté sous le nom de racine demandé,
démonté par `XamContentClose`) ; les autres contenus passent au SDK. Résultat : « My Games : You have 2 games on your
console », « Last Played Game » avec le nom et l'icône, liste « All Games » (icône, nom), page « My Games - <jeu> »
avec **Play Game** ; ce bouton appelle `XamContentLaunchImageInternal(contenu, indicateurs)` (`sub_921A6128`), qui
lance le jeu du registre (même chemin que P5 : superviseur et retour au Dashboard). Si le jeu n'est pas dans le
registre, l'appel échoue et le Dashboard affiche « Game Not Found ».
Stored Game Items : les jeux virtuels sont des contenus Arcade installés comme les autres : ils apparaissent dans les
énumérations agrégées (type `-1`) que le Dashboard utilise aussi pour la gestion de la mémoire, comme les jeux Arcade
installés d'une console. `library = false` (défaut) les masque partout.
*Dernier joué* : au lancement, la date « dernier joué » du jeu est mise à jour sur place dans `FFFE07D1.gpd` des profils
(entrée « titres » existante : on ne crée pas d'entrée, le fichier garde sa taille).

**Test** (`dashboards/2.0.6770.0/launch_test.sh <nom> <attente> "F5" "Return" "sleep:45" "kill:^./skate1" ...`, `DASH_GDB=1 [DASH_GDB_SCRIPT=...]` pour gdb, Xvfb
sur un affichage libre, lavapipe, tout est tué à la fin) :
- commande factice (`sleep 8`, TitleID `DEAD0001`) : lancée depuis F5, le Dashboard quitte, le jeu s'exécute, le
  Dashboard est relancé (`DASH_RETURNED_FROM=DEAD0001`), vérifié aussi avec `--dash_launch` (pas de boucle) ;
- **Skate** (port natif `../Skate1Recompiled/out/pc-profile/skate1`, TitleID 45410813) : lancé depuis le Dashboard,
  il tourne sous Xvfb + lavapipe (écran-titre « Appuie sur START pour continuer », ~26 FPS), puis, le jeu tué, le
  Dashboard revient. Captures : `dashboards/2.0.6770.0/runs/preuves/p5_*.png` (dossier ignoré par git).

## Contrôles

**Manette** : toute manette reconnue par SDL (Xbox, DualShock/DualSense, etc.) fonctionne directement.

**Clavier** (activé par défaut, `src/dash_input.h`). Noms = positions **QWERTY** physiques :

| Touche (QWERTY) | Sur AZERTY | Manette |
|---|---|---|
| Flèches | Flèches | Croix directionnelle |
| Entrée, Espace | Entrée, Espace | A |
| Échap, Retour arrière | Échap, Retour arrière | B |
| X / Y | X / Y | X / Y |
| Q / E | **A** / E | LB / RB (changer d'onglet) |
| 1 / 3 | & / " | LT / RT |
| Tab / P | Tab / P | Back / Start |
| WASD | **ZQSD** | Stick gauche |
| Maj + flèches | Maj + flèches | Stick droit |

Chaque touche reste modifiable : `--keybind_a=Return,Space`, `--keybind_right_shoulder=E`, etc.
(une valeur passée en ligne de commande n'est jamais écrasée). F3 / F4 / F7 : panneaux du runtime.

Test sans écran (depuis `dashboards/2.0.6770.0/`) : `./input_test.sh essai 70 "e" "Down" "Return"` (Xvfb + XTest via
`tools/xkeys.py`, une capture `runs/essai_<n>.png` après chaque groupe de touches).
Le Dashboard ignore les appuis pendant ses animations : un appui par étape. Avec lavapipe, certaines
transitions d'écran se figent ; `DASH_GDB=1 DASH_STEP=4` (exécution sous gdb, plus lente) les évite.

## Profil joueur

Un profil local est présenté au Dashboard (`src/dash_profile.cpp`) : c'est l'utilisateur 0 du
runtime (XUID `B13EBABEBABEBABE`), connecté localement.

- Gamertag : `--dash_gamertag=MonPseudo` (15 caractères max, « Joueur » par défaut).
- Contenu du profil (`DASHUSER:` : `DashStyle`, `ThematicSkin`, `VisionEffect`, `DashPreferences`) :
  dossier inscriptible `dashboards/2.0.6770.0/assets/profiles/<XUID>/`. Les fichiers n'apparaissent que quand un
  réglage est modifié (non encore vérifié : le rendu logiciel cale avant).
- Gamerscore : calculé depuis les GPD (voir « Succès et jeux joués ») ; `--dash_gamerscore=N` le fixe.
- Réputation : `--dash_reputation=0..5` étoiles, demi-étoiles comprises (0 par défaut).
- Zone : `--dash_gamerzone=0..4` (0 aucune, **1 Recreation** par défaut, 2 Pro, 3 Family, 4 Underground).
- Image du joueur, par ordre de priorité :
  1. `dashboards/2.0.6770.0/assets/profiles/<XUID>/gamerpic.png` (PNG 64×64 de moins de 16 Ko, la taille du tampon
     du Dashboard ; `gamerpic_small.png` en 32×32) ;
  2. une des **13 images par défaut de la console**, contenues dans `shrdres.xzp`
     (`64_fffe07d10002<n>0001<n>.png`) : `--dash_gamerpic=0..12` (1 par défaut) ;
  3. une image générée à partir du gamertag (`src/dash_gamerpic.h`) si `shrdres.xzp` est absent.

### Succès et jeux joués

Lus dans les fichiers **GPD** du profil (format XDBF, `src/dash_gpd.h`, `src/dash_achievements.cpp`),
placés dans `dashboards/2.0.6770.0/assets/profiles/<XUID>/` :

| Fichier | Contenu utilisé |
|---|---|
| `FFFE07D1.gpd` | liste des jeux joués (compteurs, nom, date) ; icônes de jeux (image id = TitleID) |
| `<TitleID>.gpd` | succès (nom, descriptions, Gamerscore, obtenu ou non, date) ; images (0x8000 = icône, autres = succès) |

- Ce sont les fichiers d'un **vrai profil de console** : extraire le paquet du profil
  (`Content/<XUID>/FFFE07D1/00010000/…`, paquet STFS `CON`) avec `tools/stfs.py` et copier ses `.gpd`.
- Sans profil, `python3 tools/make_test_gpd.py dashboards/2.0.6770.0/assets/profiles/B13EBABEBABEBABE` crée un
  profil de démonstration (deux jeux fictifs, succès en partie obtenus, images générées).
- Gamerscore de la carte : somme des jeux (`--dash_gamerscore=-1`, défaut), 0 sans GPD ; une valeur
  ≥ 0 le fixe.
- Un vrai jeu : `tools/spa_to_gpd.py` construit le GPD d'un jeu à partir de ses données de titre
  (ressource SPA `<TitleID>` de son XEX : succès, descriptions, images, icône) et l'ajoute aux jeux
  joués ; `--earned id,id` marque des succès comme obtenus (progression fictive). Exemple avec Skate :
  ```sh
  tools/xexinfo/build/xexinfo ../Skate1Recompiled/game/default.xex work/skate.pe
  python3 tools/xex_resource.py ../Skate1Recompiled/game/default.xex work/skate.pe 45410813 work/45410813.spa
  python3 tools/spa_to_gpd.py work/45410813.spa dashboards/2.0.6770.0/assets/profiles/B13EBABEBABEBABE --earned 7,8,12,16,24
  ```
- Écrans vérifiés : **Achievements** (tous jeux), **Played Games** (liste, succès, Gamerscore,
  icônes), **Game Summary** d'un jeu (icône, derniers succès avec leurs images, totaux).
- **Achievements d'un jeu** (Game Summary > Achievements) : sur la console, c'est l'interface de XAM
  (`XamShowAchievementsUI`), que l'émulation haut niveau n'a pas. `src/dash_xam_ui.cpp` la remplace
  par un écran ImGui : tous les succès du jeu (obtenus d'abord, puis verrouillés en grisé, secrets
  masqués), images, points, progression ; Haut/Bas pour parcourir, Échap/B pour fermer. Comme XAM,
  il envoie `XN_SYS_UI` et suspend les entrées du Dashboard. Test direct :
  `DASH_ENV="DASH_TEST_XAMUI=45410813" ./input_test.sh essai 70 "Down" "Escape"`.
- `XamIsSystemTitleId` et `XamIsXbox1TitleId` sont implémentées : leurs stubs renvoyaient l'identifiant
  du jeu (donc « vrai »), si bien que tout jeu passait pour un titre système (exception C++ fatale
  dans Achievements) ou pour un jeu Xbox originale (sauvegardes cherchées dans `fusion:\UDATA`).
- Images : `XamReadTile` (listes) et `XamReadTileEx` (Game Summary) partagent la même recherche.

### Contenu des jeux : sauvegardes, téléchargements (Stored Game Items, Downloads)

Comme sur le disque dur de la console, le contenu est un arbre de **paquets STFS** (`CON `, `LIVE`, `PIRS`),
lu par `common/src/dash_content.cpp` dans `dashboards/2.0.6770.0/assets/profiles/<XUID>/` :

```
profiles/<XUID>/Content/<TitleID>/<ContentType>/<paquet>            contenu du profil
profiles/0000000000000000/Content/<TitleID>/<ContentType>/<paquet>   contenu partagé (DLC, démos...)
```

Types usuels : `00000001` sauvegarde, `00000002` contenu téléchargé, `00080000` démo, `00030000` thème,
`00020000` image de joueur. Le nom du dossier fait foi ; l'en-tête du paquet fournit le nom affiché, la description,
la miniature PNG, le propriétaire et la taille. Remplace les stubs/implémentations du SDK pour :

| Fonction (r3, r4...) | Rôle |
|---|---|
| `XamContentAggregateCreateEnumerator(xuid, appareil, type, titre, &handle)` | énumère le contenu (titre 0 / type -1 = tous). Élément de 0x150 octets : `XCONTENT_AGGREGATE_DATA` + taille BE64 à `+0x148`. Sert aux compteurs « N saves - N downloads » de Played Games et aux écrans de stockage |
| `XamContentCreateEnumeratorInternal(utilisateur, appareil, type, titre, indicateurs, nombre, &taille, &handle)`, `XamContentCreateEnumerator(...)` | même contenu, éléments `XCONTENT_DATA` (0x134) |
| `XamContentGetMetaDataInternal(élément, tampon, 0)` | copie l'en-tête STFS `0x344..0x971A` (0x93D6 octets) : nom, description, miniature, type, TitleID |
| `XamContentGetLocalizedString(méta, nom, n, description, n)` | nom affiché et description en UTF-16BE |
| `XamContentDeleteInternal(élément, overlapped)` | supprime le paquet |
| `XamContentCopyInternal` / `XamContentMoveInternal(&nouveau_propriétaire ou 0, &appareil ou 0, élément, routine, contexte, 0, overlapped)` | copie / déplace un paquet vers un autre propriétaire (même disque) ; appareil non inscriptible (lecteur optique factice) : `ERROR_ACCESS_DENIED` ; même propriétaire et même appareil : `ERROR_ALREADY_EXISTS` |
| `XamProfileLoadAccountInfo(appareil, xuid, &compte)` | compte (gamertag) d'un profil : le Dashboard bâtit sa liste de profils avec les contenus de type « profil » (0x10000, un paquet virtuel du profil local est donc ajouté à l'énumération) et en tire le nom du propriétaire d'un contenu |
| `XamUniSortCmpString(a, b, nb_car, indicateurs)` | comparaison UTF-16BE (1 = sans casse) : l'explorateur regroupe les paquets par jeu avec une recherche dichotomique par nom |

Écrans : **Played Games** (compteurs), **Game Summary > Stored Game Items**. L'entrée **Downloads** ouvre le
Marketplace en ligne (invite « Sign in to Xbox LIVE », non simulé) : le contenu téléchargé local est dans Stored Game Items.
Stored Game Items ouvre l'explorateur de stockage du Dashboard (Storage Devices > All Devices > Games > <jeu>) ;
il liste le contenu du jeu avec nom, type (« Saved Game », « Downloaded Content »), miniature, taille et propriétaire
(le gamertag) ; Y = options de la liste, A sur un paquet = Copy / Move / Delete.

**Stored Game Items n'est pas filtré par jeu, et c'est le comportement du Dashboard 6770** (vérifié dans le code
recompilé) : le bouton de `CGameDetailsScene` navigue vers la scène `DeviceSelector.xur` sans aucun paramètre (le
TitleID courant n'est lu que par les boutons Achievements et Downloads), et `DeviceSelectorScene` ignore ses
données d'initialisation. Rien à corriger côté XAM.

Points appris en lisant le Dashboard (les trois piliers de l'explorateur, module `0x92257B50..0x92264948`) :
- le propriétaire d'un paquet vient d'une liste de profils bâtie à partir des éléments de type `0x10000`
  (`XamProfileLoadAccountInfo(appareil, xuid, &compte)` pour chacun) ; sans paquet de profil dans l'énumération,
  « Unknown Profile ». Un élément virtuel (titre `FFFE07D1`, type `0x10000`, nom = gamertag) est donc ajouté ;
- la liste **Games** regroupe les paquets en cherchant le nom du jeu (recherche dichotomique triée) avec
  `XamUniSortCmpString` ; le stub du SDK, qui renvoie r3 (jamais 0), donnait une ligne par paquet ;
- les boîtes de confirmation sont des boîtes ImGui du SDK que le clavier ne pilote pas : en test, cliquer
  (`click:X,Y` accepté par `tools/xkeys.py` et donc par `input_test.sh`, p. ex. `"click:396,372"` pour
  « Yes, delete content » à 1280x720 ; le clic dure 2,5 s car le rendu logiciel peut mettre une seconde par image).
- **Suppression vérifiée** : Stored Game Items > All Devices > Games > Skate > A sur « Skate - Carrière » > Delete >
  « Yes, delete content » : le Dashboard appelle `XamContentDeleteInternal(élément, overlapped)`, le paquet disparaît
  du profil (`profiles/<XUID>/Content/45410813/00000001/`), la liste se rafraîchit avec 2 éléments, journal
  `XamContentDeleteInternal : ... -> 0x0`. Copy/Move vers « Dummy HDD » (même appareil) : le Dashboard refuse
  lui-même (« The destination device was the same as the current device »), sans appel XAM.
- Sous lavapipe + gdb, les touches se perdent si elles se suivent de trop près ou arrivent pendant un changement
  d'écran : une touche par commande, au moins 7 s entre deux, et vérifier l'écran avant de continuer
  (`input_test.sh` accepte aussi `sleep:<s>` et `wait:<motif du journal>`, ce dernier avec
  `DASH_ARGS="--log_flush_interval=1"` pour que le journal soit écrit au fil de l'eau).

**Import d'un vrai paquet** (sauvegarde ou DLC extrait d'une console, fichier sans extension) :

```sh
python3 tools/content_import.py info <paquet>...                           # type, TitleID, nom, description, miniature
python3 tools/content_import.py import dashboards/2.0.6770.0/assets/profiles/<XUID> <paquet>...
python3 tools/content_import.py import dashboards/2.0.6770.0/assets/profiles/<XUID> --common <paquet>...   # contenu partagé
python3 tools/content_import.py list dashboards/2.0.6770.0/assets/profiles/<XUID>
python3 tools/stfs.py <paquet> [dossier]                                   # lister / extraire les fichiers du paquet
```

**Données de test** : `python3 tools/make_test_content.py dashboards/2.0.6770.0/assets/profiles/B13EBABEBABEBABE`
crée pour Skate (45410813) deux sauvegardes (type 1) et un téléchargement (type 2) : paquets `CON ` sans signature
RSA, avec en-tête complet (offsets : type `0x344`, TitleID `0x360`, propriétaire `0x371`, noms `0x411` et
descriptions `0xD11` en 18 langues de 0x80 octets UTF-16BE, éditeur `0x1611`, titre `0x1691`, taille de miniature
`0x1712`, miniature `0x171A`) et un volume STFS minimal lisible par `tools/stfs.py`.

Pour que la carte se remplisse, tous les réglages qu'elle demande doivent exister : le SDK fait
échouer la lecture entière si un seul est inconnu. `ApplyProfileSettings()` déclare donc les
réglages manquants (`0x10040001`, `0x1004003A`). Et la lecture des réglages n'est lancée qu'après
celle de l'image : sans image, pas de Gamerscore.

## Ce qui a été corrigé

### Analyse (`dashboards/2.0.6770.0/overrides.toml`, généré par `tools/port_dashboard.sh`)
- Fonctions feuilles de *dispatch* sans `.pdata`, enchaînées par des `b` de queue.
- 329 fonctions atteintes seulement via vtables/callbacks (`tools/find_ptr_funcs.py`, en excluant
  les tables de portée SEH `0x920E74EC-0x920E7E10` qui pointent au milieu des fonctions).
- 5 fonctions dont l'adresse est construite par `lis/addi` dans le code
  (`tools/find_ptr_funcs.py` + `tools/filter_inside.py`, qui rejette les 292 continuations de `catch`).
- Hooks de diagnostic (`src/diag_hooks.cpp`).

### Runtime (code du projet)
- `src/kernel_stubs.cpp` : `XUsbcam*` (exclus de la compilation du SDK) et l'import XAM ordinal 2790.
- `common/src/null_audio.h` : pilote audio silencieux cadencé à 48 kHz. Sans pilote audio, le Dashboard
  désenregistre la classe XUI `XuiSoundXAudio` et toutes ses scènes échouent (`0x80300004`).
- `src/dash_app.h` : charge `dash.xex` au lieu de `default.xex`, monte `assets/flash` en `\SystemRoot`.

### Patch du SDK (fork `rexglue-sdk`, branche `xenondash`)
Ces correctifs sont 3 commits du fork (sous-module `third_party/rexglue-sdk`). Les fichiers
`patches/rexglue-sdk-000*.patch` n'en sont que l'export, conservés comme référence (applicables
avec `git apply` sur un clone du commit c94f5eb).
- `src/system/xmemory.cpp` : la table des fonctions indirectes était toujours allouée dans le tas
  `0x8xxxxxxx` (jeux). `dash.xex` est chargé en `0x92000000` → on choisit le tas qui contient l'adresse.
- `src/input/mnk/` (patch 0002) : le pilote clavier ne produisait **aucun** événement de touche
  (`EnqueueKeystroke` n'était jamais appelé). Or les interfaces XUI, dont le Dashboard, ne lisent
  que `XamInputGetKeystrokeEx`. Les événements sont maintenant synthétisés à partir des transitions
  de l'état manette (avec répétition), et les frappes brèves (appui + relâchement traités dans le
  même lot d'événements) sont mémorisées jusqu'à l'interrogation suivante.
- `include/rex/ppc/function.h` (patch 0003) : les arguments entiers des fonctions noyau/XAM étaient
  lus sur 32 bits, y compris les `u64` (un XUID `B13EBABEBABEBABE` arrivait en `BABEBABE`). Le
  registre complet est maintenant lu ; les pointeurs restent pris sur les 32 bits bas (adresses
  étendues en signe).

## État actuel

**Le Dashboard s'affiche et se pilote** (onglets Blades, menus, carte joueur complète).

Contenu de `dashboards/2.0.6770.0/assets/flash/` (monté en `\SystemRoot` / `media:`), sans dump NAND :
- `shrdres.xzp` : ressource de `xam.xex` 2.0.17559, convertie (voir « Mise en place »).
- `XenonJKLatin.xtt`, `XenonCLatin.xtt` et les modules `*.xex` de la flash 6770 : archive
  *XeBuilds* (dossier `6770`, sans aucune donnée propre à une console). Seul son `xam.xex` ne se
  déchiffre ni avec la clé retail ni avec la clé devkit (modifié par xeBuild ?) ; il n'est pas utilisé.

Ressources partagées : le Dashboard construit `sharedres://X` via `XamBuildLegacySystemResourceLocator`.
Le localisateur du SDK, `file://media:/shrdres.xzp#X`, est coupé à la barre oblique par ce XUI
ancien ; le projet renvoie `file://media:\shrdres.xzp#X` (`src/dash_profile.cpp`).

Sous Xvfb, RADV ne peut pas présenter (pas de DRI3) : utiliser lavapipe :
`DASH_ENV="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" ./run_headless.sh 90 essai`
(sur un vrai bureau, RADV marche directement).

## Problèmes connus
- `0x9222D378 → 0x9222D328` : retour d'un *funclet* `catch` dans sa fonction parente, non géré
  (ne s'exécute que si une exception C++ est levée). Piste : `enable_exception_handlers`.
- Avec **lavapipe** (rendu logiciel, tests sous Xvfb), le GPU émulé se bloque sur un *fence* après
  quelques changements d'onglet ; le Dashboard attend alors le GPU indéfiniment. Non observé hors
  lavapipe pour l'instant (à vérifier sur un vrai bureau avec RADV).
- `XboxKrnlBaseVersion` (variable noyau) non implémentée.
- Un seul profil, toujours connecté localement (pas d'écran de connexion ni de création de profil :
  `XamShowSigninUIp`, `XamShowCreateProfileUI`, `XamUserLogon` restent en stub). Réputation et zone
  sont des valeurs fixées par option.
- Icônes partagées (Ⓖ, boutons, zones) : ce sont celles du XAM **17559**, d'un style plus récent que
  la 6770. Celles d'origine sont dans le `xam.xex` de base 2.0.1888 (+ `xam.xexp` de la 6770) ;
  XeBuilds le fournit (`firmware/xebuild-xam/`, identique pour toutes les versions) mais il ne se
  déchiffre ni avec la clé retail ni avec la clé devkit.
- Une exception C++ dans le Dashboard reste fatale (`RtlUnwind` non implémenté côté SDK, retour de
  *funclet* `catch` non géré) ; vue une fois, causée par le stub de `XamIsSystemTitleId`.
- Stubs : `XamGetLanguage`, `XGetAudioFlags`, `HalGetCurrentAVPack`, `XamSetDashContext`, messages XMP.
