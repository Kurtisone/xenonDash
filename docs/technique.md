# XenonDash : référence technique

Document pour les développeurs : architecture commune, portage d'une version, noyau et HLE, profils, AppImage, état par version.
Utilisation : [README](../README.md). Reprise du travail : [REPRISE.md](REPRISE.md). Version prioritaire : **Kinect 2.0.13604** (rendu natif, XAM et HUD
recompilés) : [xam_recompile.md](xam_recompile.md), [ngpu_core.md](ngpu_core.md), [jeux.md](jeux.md). Les sections « Contrôles » et « Profil joueur » décrivent surtout le 6770 (HLE).

Recompilation statique de `dash.xex` en C++ avec [ReXGlue](https://github.com/rexglue/xenondash-sdk) (runtime dérivé de Xenia) et [XenonRecomp](https://github.com/hedge-dev/XenonRecomp).
Le dépôt ne contient **aucun fichier Microsoft** (firmware, XEX, polices, ressources), aucun jeu ni code généré : tout est fourni ou régénéré localement.

## Architecture commune

| Chemin | Contenu |
|---|---|
| `common/` | bibliothèque CMake OBJECT `xenondash_common` (les `REX_HOOK(__imp__...)` doivent être liés) partagée par toutes les versions : `xenondash::DashAppBase` (flash, `OnPreSetup`/`OnPostSetup`), profil, GPD/succès, contenu, entrées, audio silencieux, lanceur de jeux, redirection vers XAM, stubs noyau |
| `dashboards/<version>/` | un projet ReXGlue par version : manifeste, `overrides.toml` (généré, ne pas éditer), `dash_overrides.toml` (manuel), `src/` propre à la version, scripts de test. Le `CMakeLists.txt` générique compile tous les `src/*.cpp` |
| `ngpu_core/`, `native_gpu/` | noyau de rendu natif Vulkan et façades D3D ([ngpu_core.md](ngpu_core.md)) |
| `games/<TitleID>/` | recettes de jeux ([jeux.md](jeux.md)) ; `games.example.toml` : modèle du registre `games.toml` |
| `firmware/<version>/` | mises à jour système extraites (non versionné) ; modules flash XeBuilds dans `firmware/xebuild-<n>/` |
| `sdk/` | SDK ReXGlue installé depuis le sous-module `third_party/xenondash-sdk` (branche `main` du fork) ; `patches/` : historique des correctifs et `patches/xenosrecomp/` |
| `tools/`, `work/` | outils Python/C++ ; sorties d'analyse (non versionné) |

Partagé : tout `common/`, le noyau de rendu, le gabarit `tools/templates/dashboard/`. Propre à une version : manifeste, bornes de fonctions, `version_stubs.cpp`, `diag_hooks.cpp`,
adresses des profils D3D. Les Dashboards ne communiquent avec XAM recompilé que par redirection sélective des imports ([xam_recompile.md](xam_recompile.md)).

## Mise en place et construction

Dépendances (Arch) : `clang cmake ninja libxi gtk3 libxss libxtst libpulse alsa-lib libxkbcommon xorg-server-xvfb`. Hors poste de développement : [compilation.md](compilation.md).

```sh
git submodule update --init --recursive
git clone --recursive https://github.com/hedge-dev/XenonRecomp.git third_party/XenonRecomp
git -C third_party/XenonRecomp checkout ddd128bcca99fe8bfbb99bea583c972351fa6ace
cmake -S third_party/xenondash-sdk --preset linux-amd64 -DCMAKE_INSTALL_PREFIX=$PWD/sdk
cmake --build third_party/xenondash-sdk/out/build/linux-amd64 --config Release --target install
cmake -S tools/xexinfo -B tools/xexinfo/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENONRECOMP_DIR=$PWD/third_party/XenonRecomp
cmake --build tools/xexinfo/build          # outil d'analyse XEX
cd dashboards/<version> && ../../sdk/bin/rexglue codegen dash_manifest.toml      # ~30 s à 2 min
cmake --preset linux-amd64-relwithdebinfo && cmake --build --preset linux-amd64-relwithdebinfo -j12   # ~10 min la première fois
./run_headless.sh 40 essai      # Xvfb + capture runs/essai.png ; DASH_AUDIO=nop : audio silencieux (common/src/null_audio.h)
```

Journaux : `out/build/<preset>/logs/` ; `--log_level=trace` pour tout voir. Sous Xvfb, RADV ne peut pas présenter (pas de DRI3) : lavapipe
(`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`, `MESA_VK_WSI_PRESENT_MODE=immediate`, via `DASH_ENV`), sinon la capture reste noire même si le rendu est correct.
Les touches XTest (`tools/xkeys.py`) demandent `XKEYS_HOLD=0.8` avec lavapipe (entrées lues à quelques images par seconde) ; le Dashboard ignore les appuis pendant ses animations.
Banc de test du Kinect : `tools/banc.py` ([REPRISE.md](REPRISE.md)).

## Porter une nouvelle version

`tools/port_dashboard.sh <mise_à_jour|STFS|dash.xex> <version> [--flash-dir <modules XeBuilds>] [--shrdres-xam <xam.xex complet>]` enchaîne les étapes (`--stages`,
`--from-stage`, `--no-smoke`, `--dest`) ; fichiers intermédiaires et journaux dans `work/port/<version>/`, code de retour non nul à la première étape en échec.

| Étape | Action |
|---|---|
| `extract` | extraction STFS (`tools/stfs.py`), repérage de `$flash_dash.xex` |
| `assets` | `assets/dash.xex`, `assets/flash/` (modules, polices), `shrdres.xzp`/`gamercrd.xzp` convertis (`xex_resource.py`, `xzp_alias.py`) ; pour 13604 et 17559, les XZP viennent du `$flash_xam.xex` de la mise à jour |
| `pe` | `tools/xexinfo` : image PE décompressée, base, sections |
| `scaffold` | gabarit `tools/templates/dashboard/` ; n'écrase rien (`--refresh-template` pour forcer) |
| `codegen` | `rexglue codegen` en boucle : chaque *UnresolvedCall* ajoute sa cible ; `find_ptr_funcs.py` + `filter_inside.py` ajoutent les fonctions atteintes par pointeur ou `lis/addi` (continuations de `catch` et tables SEH écartées) ; sortie `overrides.toml` |
| `build` | compilation ; imports d'ordinal inconnu : `src/auto_stubs.cpp` (`REX_STUB_RETURN(..., 0)`) puis nouvelle compilation |
| `smoke` | Xvfb + lavapipe, 40 s, capture ; échec si `dash` meurt, ligne `FATAL`/`[critical]` ou capture uniforme (`--reference-png`) |
| `compare` | avec `--compare-with <projet>` : tables de fonctions et overrides |

Corrections manuelles : `dash_overrides.toml` (bornes, `[[midasm_hook]]`, `setjmp_address`/`longjmp_address`) et `src/*.cpp`. Les continuations de funclets `finally`
(terminées par un `b` vers la fonction parente) sont déclarées par l'outil. Critère P2 vérifié : 6770 régénéré = mêmes 24 669 fonctions, code identique, capture équivalente, aucun FATAL.
`libcrypto`/`python3-cryptography` : `xex_rekey.py` en dépend (repli sur libcrypto puis Python pur).

## Noyau et HLE : correctifs et causes

**SDK (fork, sous-module)** :
- `xmemory.cpp` : la table des fonctions indirectes allait toujours dans le tas `0x8xxxxxxx` ; `dash.xex` est en `0x92000000` : choisir le tas qui contient l'adresse.
- Clavier (`src/input/mnk/`) : aucun événement de touche n'était produit ; les XUI ne lisent que `XamInputGetKeystrokeEx` : événements synthétisés depuis l'état manette, appuis brefs mémorisés.
- `function.h` : arguments entiers lus sur 32 bits (un XUID `B13EBABEBABEBABE` devenait `BABEBABE`) : registre complet pour les `u64`, pointeurs sur 32 bits.
- `KeInsertQueueDpc` ne dispatchait pas (`assert_always`, DPC seulement mise en file) : le D3D 17559 soumet ses tampons imbriqués par DPC, d'où l'écran noir et le fence 0x0D jamais publié ; la DPC s'exécute maintenant sur le fil « Kernel Dispatch » (`8eb253f`).
- `XeCryptHmacSha*` implémentés (étaient des stubs) ; imports sans thunk (adresse 0) ignorés au codegen (7258) ; `LoadUserModule` charge `libXXX.so` à côté de l'exécutable ; `KeBugCheck` fatal ([REPRISE.md](REPRISE.md)).
- SDK unifié (`main`, ex-`unified`, 11 commits au-dessus de `c94f5eb`) : travaux de Skate1Native et de Trials (`linux-android/0001..0008`) ; conflit `mnk_input_driver` résolu en gardant la file de touches liées de Skate puis la synthèse depuis la manette ; D3D12 écarté.
  Reste : isoler l'émulation GPU (`REX_GPU_EMULATION=OFF`).
- XenosRecomp unifié : `patches/xenosrecomp/` (série) + `tools/build_xenosrecomp.sh` (clone amont `990d03b`, ≈ 30 s) : clés de microcode et SPIR-V brut (Skate), tables de constantes absentes (Dashboard), export mémoire/LOD/`cube` (Trials, `XENOS_RECOMP_TRIALS=1`).
  Les clés de cache doivent inclure l'empreinte de la série et ces variables.

**Runtime (`common/`)** :
- Audio : sans pilote, le Dashboard désenregistre `XuiSoundXAudio` et ses scènes échouent (`0x80300004`) : `null_audio.h`, cadencé à 48 kHz.
- `DashAppBase::OnPostSetup` remet l'image de l'exécutable en lecture/écriture (le 17559 écrit dans ses ressources XEX : faute en boucle).
- `KeGetImagePageTableEntry` : le Dashboard teste le bit 0 de l'entrée, le SDK y mettait l'indice de page (`KeBugCheck(0xF7)` une page sur deux) ; `KeQueryInterruptTime` renvoyait 0 : horloge monotone en 100 ns (`kernel_stubs.cpp`).
- `setjmp`/`longjmp` du CRT (Lua) à déclarer dans `dash_overrides.toml` (sinon `longjmp` « revient » et le Dashboard appelle `exit()`) : 13604 `0x9288D320`/`0x9288CEF0`, 17559 `0x9291C760`/`0x9291C330`.
- Stubs du SDK qui renvoient leur 1er argument (valeur aléatoire) à remplacer : `XamGetLanguage`, `XamGetDashContext`/`XamSetDashContext`, `XamIsNuiAutomationEnabled`, `XamNuiIsDeviceReady`, `XamGetStagingMode`,
  `XamGetLanguageLocaleFallbackString` (`en-US`), `XamGetLanguageTypeface` (`file://media:/SegoeXbox-Light.xtt`), `XamIsSystemTitleId`/`XamIsXbox1TitleId` (le « vrai » faisait de tout jeu un titre système : exception C++ fatale dans Achievements).
- `XamBuildLegacySystemResourceLocator` : le localisateur `file://media:/shrdres.xzp#X` est coupé à la barre oblique par ce XUI ancien ; renvoyer `file://media:\shrdres.xzp#X` (`dash_profile.cpp`).
- 17559 : ressources `langpack://L..<nom>.xzp` servies par `XamPackageManagerGetFilePathW` (`media:\<nom>.xzp`), XZP extraits de `dash.xex` à l'import (35 fichiers).
- Profil et Recent Games (13604, 17559) : le hub sonde toutes les 5 s les réglages `0x63E80044`, `0x7008004F`, `0x61180050` (le SDK échoue en bloc si l'un est inconnu), puis `XamUserGetUserTenure`, `XamUserGetSubscriptionType`,
  `XamProfileFindAccount`, `XamUserGetIndexFromXUID`. Sans profil « reconnu », la Games Library n'énumère pas. `dash_profile.cpp` déclare les réglages, `dash_user.cpp` répond (profil local hors ligne, `xuid_online = 0`).
- `XLIVEBASE 0xFC/0x58003` (17559) : répondre « succès » laisse le hub sur un fond vide ; l'échec est conservé.
- Polices : le Dashboard ouvre `XenonJKLatin.xtt` ; sans elle, 20 000 échecs `0xC000000F` et tuiles sans texte. 13604 : la mise à jour n'a que des `.xttp` (jamais lus) : polices 6770 ou XeBuilds en substitution.
  Glyphe Gamerscore (U+E00A) : seulement dans `XenonSCLatin.xtt` (13604) ; `dash_font_patch.cpp` ([REPRISE.md](REPRISE.md)).
- `xam.xex` 13604 chiffré avec la clé XEX1 : `tools/xex_rekey.py` la déchiffre (vérifiée par le SHA-1 du premier bloc) et rechiffre avec la clé retail dans un XEX dérivé sous `work/`.
  Les `xam_*.xex` de `firmware/xebuild-xam/` (6683 à 7357) se déchiffrent de même : piste pour recompiler XAM des versions Blades (non explorée ; 6770 reste base 1888 + delta).
- Fuseau horaire, KeBugCheck, stubs de sortie, fibres : [REPRISE.md](REPRISE.md) et [xam_recompile.md](xam_recompile.md).

## XAM recompilé

Palier 2 du Dashboard 17559 (prototype, `dashboards/2.0.17559.0-xam/`) : `xam.xex` 17559 se recompile (`tools/codegen_module.py`, 19 972 fonctions, 906 imports noyau tous fournis par `librexruntime.so`) en module `[[modules]]`
(`libdash_xam.so`) ; `DASH_XAM_MODULE=1` charge `game:\xam.xex`, `=2` exécute en plus son point d'entrée (`0x81696AF0`, sept fils système). Bloquant mesuré : le premier fil système lit `0x274` (singleton non
initialisé : périphériques `Harddisk0\Partition1`, `Flash`, `Mu` et stubs noyau absents). Les imports du Dashboard sont liés au HLE à la compilation (`__imp__<Nom>`) : servir un import par XAM demande un `REX_HOOK_RAW`
qui appelle l'export (`XexModule::GetProcAddress`). L'architecture aboutie (13604, redirection par ordinal, paliers A à N5) est dans [xam_recompile.md](xam_recompile.md) ; le 17559 est reporté (Metro).

## Lancer des jeux

Code : `common/src/dash_launch.cpp`. Registre `<game_data_root>/games.toml` (modèle `games.example.toml`, relu à chaque ouverture) : `[game.<TitleID hexa>]` avec `name` et `native = "commande"` (+ `cwd`), `xenia = "chemin"`
(`--dash_xenia`, `DASH_XENIA`, `[xenia] command`) ou `recipe = "<dossier | default.xex | ISO>"` ; autres clés : `type` (`arcade` par défaut), `icon`/`spa`, `keep_ld_library_path`, `library = true` (jeux listés dans « My Games »).
La clé `disc` n'existe plus (simulation de disque supprimée le 2026-11-21 : `XamLoaderGetMediaInfo` rend « aucun média », `XamLoaderGetDvdTrayState` 2, notification `0x8000000D` jamais envoyée).
Déclencheurs : **F5** (liste du registre ; Back + Y à la manette), tout appel à `XamLoaderLaunchTitle`/`Ex` (titre retrouvé par un TitleID dans le chemin), `--dash_launch=<TitleID> [--dash_launch_delay=5]`.

**Mécanisme** : le Dashboard ne reste pas en veille (deux fenêtres Vulkan et deux pilotes audio bloquent). Il lance un superviseur détaché (`/bin/sh -c`, nouvelle session) et quitte (`_Exit`) ; le superviseur exécute
`( unset LD_LIBRARY_PATH; cd <cwd>; <commande> )` puis relance le même exécutable avec les mêmes arguments (sauf `--dash_launch*`) et `DASH_RETURNED_FROM=<TitleID>`. Le code de retour du jeu est sans importance ; le Dashboard repart
sur Games (état du profil sur disque) ; le jeu trouve seul ses bibliothèques. Journal : « LANCEMENT de ... », « superviseur : sh -c ... ». Les hooks remplacent `LaunchTitle`/`LaunchTitleEx` du SDK (qui terminait le Dashboard sans rien lancer).
Imports du Dashboard 6770 : `XamLoaderLaunchTitle` (`0x9273988C`), `LaunchTitleEx` (`0x92739FCC`), `GetMediaInfo` (`0x9273A05C`), `SetLaunchData`/`GetLaunchData`, `GetDvdTrayState`, `GetPriorTitleId`, `TerminateTitle` ; `sub_921152A8` enveloppe `LaunchTitle`
(chemin NULL = lancer le disque ; jetons `XLNI_DET_MEDIA`, `XLNI_DASH_ARCADE`) ; `sub_92274218`/`sub_9229E428` : données de lancement de type 38 (relance d'un titre système).
Disque réel (à faire, `\Device\CdRom0`) : le Dashboard dépile `0x8000000D` (paramètre = type de média, 1 = jeu), ouvre `\Device\Cdrom0\default.xex`, vérifie l'en-tête XEX2 (clés `0x40006`, `0x2FF`, `0x3FF`, `XexTransformImageKey`) et demande
à XAM (`XMsgStartIORequest(0xFB, 0xB003C)`) le nom (UTF-16, 40 caractères) et l'icône PNG (16 384 octets au plus).

**Bouton de disque et « My Games »** (`dash_library.cpp`, `dash_content.cpp` ; 6770 puis 13604) : la Games Library est une scène « Arcade:: » (`2500_LiveArcadeHome.xur`) ; pour chaque contenu Arcade (`0xD0000`), une tâche `CLoaderTask`
(vtable `0x9201F8B8` ; `sub_9222C608` par type, `sub_9222B550` vérification commune) appelle (1) `XamContentGetAttributesInternal` (doit **réussir**, 36 octets : le stub qui renvoyait son argument grisait My Games) ; (2) `XamContentCreateInternal`
(`3` = ouvrir l'existant) ; (3) lit `Content<n>:\ArcadeInfo.xml` en **UTF-16 big-endian avec BOM** (`sub_92224fe0` ; un UTF-8 est lu comme du bruit) : `TitleInfo` (`Id Name Path ImagePath MaxCred ScoreName CameraEnabled ContentNotDownloadable`), `Description` ;
(4) `XamContentClose`. Les jeux `library = true` deviennent des éléments virtuels (répertoire temporaire `xenondash-content-<uid>/<TitleID>/` monté par `XamContentCreateInternal`). **Play Game** appelle `XamContentLaunchImageInternal`
(`sub_921A6128`) ; hors registre : « Game Not Found ». La date « dernier joué » est mise à jour sur place dans `FFFE07D1.gpd`. Suite 13604 : [jeux.md](jeux.md).

## Contrôles

Manette : toute manette SDL. Clavier (positions QWERTY ; `common/src/dash_input.h`, remplace le mappage du SDK) : flèches = croix, Entrée/Espace = A, Échap/Retour arrière = B, X/Y, Q/E = LB/RB, 1/3 = LT/RT, Tab = Back,
P = Start (valide « Done » du clavier virtuel), WASD = stick gauche, Maj + flèches = stick droit, G = Guide, F5/F6 = liste et scan des jeux. Redéfinition : `--keybind_a=Return,Space`, `--keybind_right_shoulder=E`.

## Profil joueur, succès et contenu

Profil local (`common/src/dash_profile.cpp`) : utilisateur 0, XUID `B13EBABEBABEBABE`, connecté localement ; `--dash_gamertag` (15 caractères, « Joueur »), `--dash_gamerscore=N` (-1 : somme des GPD), `--dash_reputation=0..5`,
`--dash_gamerzone=0..4`, `--dash_gamerpic=0..12` (13 images par défaut de `shrdres.xzp`) ; données dans `<game_data_root>/profiles/<XUID>/` (`gamerpic.png` 64x64 de moins de 16 Ko). Le Gamerscore n'est lu qu'après l'image
(sans image, pas de Gamerscore) et tous les réglages demandés par la carte doivent exister (`ApplyProfileSettings` ajoute `0x10040001`, `0x1004003A`).

Succès et jeux joués : fichiers **GPD** (XDBF ; `dash_gpd.h`, `dash_achievements.cpp`) : `FFFE07D1.gpd` (jeux joués, icônes) et `<TitleID>.gpd` (succès, images `0x8000` = icône). Sources : paquet de profil réel extrait par `tools/stfs.py`,
`tools/make_test_gpd.py` (démonstration), `tools/spa_to_gpd.py` (SPA du XEX d'un jeu ; `--earned id,id`). `XamReadTile`/`XamReadTileEx` partagent la même recherche d'images. Achievements d'un jeu = interface de XAM
(`XamShowAchievementsUI`, vers XAM : [To-Do](../PLAN.md)).

Contenu (`dash_content.cpp`) : arbre de paquets STFS `profiles/<XUID>/Content/<TitleID>/<ContentType>/<paquet>` (partagé : `profiles/0000000000000000/`) ; types `00000001` sauvegarde, `00000002` téléchargement, `00080000` démo, `00030000` thème,
`00020000` image de joueur, `00010000` profil. Remplace les implémentations du SDK : `XamContentAggregateCreateEnumerator` (élément 0x150 : `XCONTENT_AGGREGATE_DATA` + taille BE64 en `+0x148`), `XamContentCreateEnumerator(Internal)` (0x134),
`GetMetaDataInternal` (en-tête STFS `0x344..0x971A`), `GetLocalizedString`, `DeleteInternal`, `CopyInternal`/`MoveInternal` (appareil non inscriptible : `ERROR_ACCESS_DENIED` ; même appareil : `ERROR_ALREADY_EXISTS`),
`XamProfileLoadAccountInfo` (liste de profils du Dashboard bâtie sur les contenus `0x10000` : un paquet virtuel du profil local est ajouté, sinon « Unknown Profile »), `XamUniSortCmpString` (UTF-16BE ; le stub renvoyait r3 et donnait une ligne par paquet).
Stored Game Items n'est pas filtré par jeu : c'est le comportement du Dashboard 6770 (scène `DeviceSelector.xur` sans paramètre), pas un défaut. Downloads ouvre le Marketplace en ligne (non simulé). Suppression vérifiée de bout en bout.
Import : `tools/content_import.py info|import|list` ; données de test : `tools/make_test_content.py`.

## AppImage

`tools/make_appimage.sh 2.0.13604.0-xam` produit **`out/appimage/XenonDash-Kinect.AppImage`** (une seule AppImage ; le binaire vient de `out/build/native`, rendu natif sans plugin GPU). Contenu : `usr/bin/dash`, `librexruntime.so`,
`AppRun`, `.desktop`, icône, `games.example.toml`, scripts d'import (`port_dashboard.py` étapes `extract`/`assets`, `stfs.py`, `xex_resource.py`, `xzp_alias.py`, `xexinfo`, `dash_shaders.py`, `hud_shaders.py`, XenosRecomp, `dxc-linux`, `xxhsum`)
et la chaîne de recompilation des jeux. **Aucun fichier Microsoft.** Prérequis : Python 3, GPU Vulkan, FUSE 2 (sinon `APPIMAGE_EXTRACT_AND_RUN=1`), la mise à jour système officielle de la version.
**Premier lancement** : si `~/.local/share/xenondash/2.0.13604.0-xam/` n'a pas `dash.xex` + `.ready`, `AppRun` demande le chemin (zenity, kdialog ou console), lance `xenondash_import.py` (étapes `extract` + `assets`), génère les shaders SPIR-V
(`shaders/`, ≈ 20 s ; `--regen-shaders`), crée `profiles/` vide et `games.toml`, puis démarre avec `--game_data_root=<dossier>` ; journal `dash.log`. Options : `--import <chemin>`, `--modules <dossier>`, `--shrdres-xam`,
`--reimport`, `--data-dir`, `--games-dir`, `--help` ; `XENONDASH_DATA` change la racine. Tests : `tools/test_appimage.sh <version> xebuild-6770` (HOME temporaire, Xvfb + lavapipe), `dashboards/2.0.13604.0-xam/e2e_natif.sh`.
**Usage personnel uniquement** : le binaire contient une traduction du code Microsoft ; ne jamais versionner ni publier les AppImages.

## État par version

| Version | État | Notes |
|---|---|---|
| **2.0.13604.0 (Kinect)** | **prioritaire** : projet `dashboards/2.0.13604.0-xam`, XAM recompilé par défaut, rendu natif, HUD, profil, Guide, avatar, jeux | codegen 32 640 fonctions (10 cibles non résolues, 407 par pointeur) ; paquet `su20076000_00000000` avec `$flash_dash.xex` et `$flash_xam.xex` complets ; `DASHUSER:\ThematicSkin`/`VisionEffect` absents ; suite : [xam_recompile.md](xam_recompile.md), To-Do de [PLAN.md](../PLAN.md) |
| 2.0.6770.0 (Blades) | OK en HLE, rendu natif possible (`-DDASH_NATIVE_GPU=ON`) ; reporté | 24 669 fonctions ; XAM 6770 = base 1888 indéchiffrable + delta : XAM reste HLE ; `shrdres.xzp` pris dans la 17559 puis converti ; un profil local, pas d'écran de connexion |
| 2.0.17559.0 (Metro) | palier 1 OK (HLE, interface Metro, profil, lancement de Skate) ; variante `-xam` en prototype ; reporté | 38 105 fonctions, 516 imports XAM, 284 imports noyau ; noms de fuseau laissés vides (plantage de Clock, [REPRISE.md](REPRISE.md)) ; A sur « Play Skate » : scène vide ; onglets du hub non vérifiés |
| 2.0.6717.0 | OK (Games, Achievements, Played Games) | continuation de funclet `finally` `0x9222D1B0` déclarée (sans elle : `FATAL Unresolved call`) |
| 2.0.7258.0 | partiel : démarre, tuiles, profil détecté | import sans thunk (`XamVoiceSubmitPacket`) ; écran de profil derrière « Sign In » non géré |
| 2.0.5759.0, 2.0.4532.0 | partiel : écran noir | déréférencement nul (`0x8`) pendant l'initialisation XUI après le 2e `XamProfileCreateEnumerator` (`sub_9226CE00`) ; `media:\gamercrd.xzp` absent |
| 2.0.2255.0 | impossible | deltas `.xexp` sur la base 1888 chiffrée, sans clé |

Portage du 13604 : `tools/port_dashboard.sh firmware/2.0.13604.0 2.0.13604.0` (paquet `su20076000_00000000`, sha256 `a2e9b70c…`), 3 itérations de codegen, aucun stub d'import supplémentaire, ≈ 4 min ;
aucune correction de `common/` ni du SDK n'était nécessaire au départ (les stubs et le correctif DPC du palier 17559 suffisaient). Écran de départ : blades Welcome, Xbox LIVE, My Xbox, Game Library.
Ces versions restent en l'état (une version par interface : Blades 6770, Kinect 13604, Metro 17559). Polices et modules flash des versions Blades : archive XeBuilds (`--flash-dir`) ; 5759 et 4532 empruntent ceux de 6717.

## Inventaire des fonctions

`python3 tools/function_inventory.py <module> --summary|--find <adresse|préfixe>|--role <rôle>|--top N|--top-calls N|--tsv|--json` (modules `dash`, `xam`, `dash/hud`, `dash/signin`, `dash/createprofile`, `dash/gamerprofile`,
`dash/deviceselector`, `dash/ximecore`, `dash/vk`) classe les fonctions du code généré par rôle (imports noyau ou graphe d'appels) et donne les appelants. Dashboard 13604 : 32 640 fonctions, 129 fichiers générés ; 20 257 « divers »,
6 167 feuilles muettes, 2 268 appels virtuels. Tables non versionnées. Points d'entrée connus du 13604 : `0x9218EAB0` présentation (appelle VdSwap), `0x921AE620` boucle de rendu/événements (`ExCreateThread`, contexte `0x40004F18`),
`0x92172B48` pilote d'image, `0x92173B80` entrée de la pompe, `0x9217F108` trampoline de fil invité. Les noms établis sont aussi dans `symbols/2.0.13604.0/` ([ghidra.md](ghidra.md)).

## Licences et ports de jeux

Fusion des ports (inventaire de novembre 2026) : XenonDash est **GPL-3.0** (`LICENSE`) car il reprend le code de Skate1Native (GPL-3.0, lui-même dérivé d'UnleashedRecomp) ; l'utilisateur a l'accord de TekRantGaming pour le code de TrialsReVolution ;
composants MIT/BSD/zlib/Apache-2.0 et FFmpeg/libmspack (LGPL) compatibles, avis dans `ngpu_core/THIRD_PARTY.md`. Les fichiers Microsoft, EA, RedLynx et `generated/` ne sont jamais versionnés ni distribués. Skate1Native
et Trials reposent sur ReXGlue 0.10.0 (`c94f5eb`) : leurs correctifs sont dans la branche `main` du fork (voir « SDK unifié »). Le rendu de Skate est devenu `ngpu_core` ([ngpu_core.md](ngpu_core.md)) ; les recettes, le moteur
`recipe_build.py` et la recompilation générique : [jeux.md](jeux.md). Décision : zéro émulation, un jeu sans rendu natif est refusé.

## Problèmes connus

- `0x9222D378 -> 0x9222D328` (6770) : retour d'un funclet `catch` dans sa fonction parente, non géré (seulement si une exception C++ est levée) ; une exception C++ du Dashboard reste fatale (`RtlUnwind` absent du SDK).
- Lavapipe : le GPU émulé se bloque sur un fence après quelques changements d'onglet ; non observé sur vrai GPU.
- `XboxKrnlBaseVersion` (variable noyau) non implémentée. Stubs : `XGetAudioFlags`, `HalGetCurrentAVPack` (écrit pour 13604 dans `kernel_video_output.cpp`), messages XMP du HLE (dans `-xam`, ceux du Dashboard vont à XAM).
- Icônes partagées (Ⓖ, boutons, zones) du 6770 : celles du XAM 17559, plus récentes que la 6770 (les originales sont dans le `xam.xex` de base 1888, déchiffrable avec la clé XEX1).
- 13604 natif : pas de superposition ImGui (lanceur F5, succès de remplacement non affichés) ; progression de construction par une boîte de XAM ; shaders et limites du rendu : [ngpu_core.md](ngpu_core.md).
- 17559 (Metro) : plantage de Clock avec des noms de fuseau renseignés, cause non trouvée ([REPRISE.md](REPRISE.md), « Faits récents »).

Historique détaillé avant condensation :
- `git show d96c252:docs/technique.md`
- `git show d96c252:docs/fusion_ports.md`
- `git show d96c252:docs/inventaire_dashboard.md`
