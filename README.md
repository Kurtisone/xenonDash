# Dashboard Xbox 360 recompilé en natif Linux

Recompilation statique de `dash.xex` (firmware 2.0.6770.0) en C++ natif avec
[ReXGlue](https://github.com/rexglue/rexglue-sdk) (runtime dérivé de Xenia) et des outils de
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp).

## Mise en place

Le dépôt ne contient **aucun fichier Microsoft** (firmware, XEX, polices, ressources) ni le C++
généré à partir de `dash.xex` : ils sont à fournir / régénérer localement.

```sh
# Dépendances (versions testées)
git clone --recursive https://github.com/rexglue/rexglue-sdk.git third_party/rexglue-sdk
git -C third_party/rexglue-sdk checkout c94f5ebdcb3c9d1a460ca48e04f9758448f8d518
git -C third_party/rexglue-sdk apply ../../patches/rexglue-sdk-0001-function-table-heap.patch \
                                     ../../patches/rexglue-sdk-0002-mnk-keystrokes.patch \
                                     ../../patches/rexglue-sdk-0003-u64-arguments.patch
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
| `dash/assets/dash.xex` | `$flash_dash.xex` de la mise à jour 2.0.6770 (`tools/stfs.py` sur `su20076000_00000000`) |
| `dash/assets/flash/*.xex` | modules flash 6770 (mise à jour + flash console) |
| `dash/assets/flash/XenonJKLatin.xtt`, `XenonCLatin.xtt` | polices de la flash 6770 |
| `dash/assets/flash/shrdres.xzp` | ressource `shrdres` de `xam.xex`, **convertie** (voir ci-dessous) |

`shrdres.xzp` (icônes, images de joueur par défaut, tables de chaînes partagées) est une ressource de
`xam.xex`. Celui de la 6770 n'étant pas déchiffrable ici, on prend celui de la mise à jour officielle
2.0.17559 et on le convertit aux formats lus par le Dashboard 6770 :

```sh
tools/xexinfo/build/xexinfo 'firmware/17559/$flash_xam.xex' work/xam17559.pe
python3 tools/xex_resource.py 'firmware/17559/$flash_xam.xex' work/xam17559.pe shrdres work/shrdres_17559.xzp
python3 tools/xzp_alias.py work/shrdres_17559.xzp dash/assets/flash/shrdres.xzp
```

## Arborescence

| Chemin | Contenu |
|---|---|
| `firmware/<version>/` | Mise à jour système extraite (paquet STFS `su20076000_00000000`) |
| `dash/` | Projet ReXGlue : manifeste, corrections d'analyse, code applicatif |
| `dash/assets/dash.xex` | Exécutable d'entrée |
| `dash/assets/flash/` | Contenu de la flash console, monté comme `\SystemRoot` (`media:`) |
| `sdk/` | SDK ReXGlue installé (compilé depuis `third_party/rexglue-sdk`, **patché**, voir plus bas) |
| `tools/` | Outils d'analyse (Python/C++) |
| `work/` | Sorties d'analyse (image PE décompressée, imports, candidates) |

## Construire et lancer

```sh
# 1. Génération du C++ (≈30 s) puis compilation (≈10 min la première fois)
cd dash
../sdk/bin/rexglue codegen dash_manifest.toml
cmake --preset linux-amd64-relwithdebinfo        # une seule fois
cmake --build --preset linux-amd64-relwithdebinfo -j12
ln -sf ../../../../sdk/lib/librexgpu-xenos.so out/build/linux-amd64-relwithdebinfo/

# 2a. Sur un bureau (X11/Wayland)
LD_LIBRARY_PATH=../sdk/lib out/build/linux-amd64-relwithdebinfo/dash --game_data_root=assets

# 2b. Sans écran ni carte son (SSH) : Xvfb + capture dans runs/<nom>.png
./run_headless.sh 40 essai
```

Journaux : `dash/out/build/linux-amd64-relwithdebinfo/logs/`. `--log_level=trace` pour tout voir.
`DASH_AUDIO=nop` active le backend audio silencieux (`src/null_audio.h`).

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

Test sans écran : `./input_test.sh essai 70 "e" "Down" "Return"` (Xvfb + XTest via
`tools/xkeys.py`, une capture `runs/essai_<n>.png` après chaque groupe de touches).

## Profil joueur

Un profil local est présenté au Dashboard (`src/dash_profile.cpp`) : c'est l'utilisateur 0 du
runtime (XUID `B13EBABEBABEBABE`), connecté localement.

- Gamertag : `--dash_gamertag=MonPseudo` (15 caractères max, « Joueur » par défaut).
- Contenu du profil (`DASHUSER:` : `DashStyle`, `ThematicSkin`, `VisionEffect`, `DashPreferences`) :
  dossier inscriptible `dash/assets/profiles/<XUID>/`. Les fichiers n'apparaissent que quand un
  réglage est modifié (non encore vérifié : le rendu logiciel cale avant).
- Gamerscore : calculé depuis les GPD (voir « Succès et jeux joués ») ; `--dash_gamerscore=N` le fixe.
- Réputation : `--dash_reputation=0..5` étoiles, demi-étoiles comprises (0 par défaut).
- Zone : `--dash_gamerzone=0..4` (0 aucune, **1 Recreation** par défaut, 2 Pro, 3 Family, 4 Underground).
- Image du joueur, par ordre de priorité :
  1. `dash/assets/profiles/<XUID>/gamerpic.png` (PNG 64×64 de moins de 16 Ko, la taille du tampon
     du Dashboard ; `gamerpic_small.png` en 32×32) ;
  2. une des **13 images par défaut de la console**, contenues dans `shrdres.xzp`
     (`64_fffe07d10002<n>0001<n>.png`) : `--dash_gamerpic=0..12` (1 par défaut) ;
  3. une image générée à partir du gamertag (`src/dash_gamerpic.h`) si `shrdres.xzp` est absent.

### Succès et jeux joués

Lus dans les fichiers **GPD** du profil (format XDBF, `src/dash_gpd.h`, `src/dash_achievements.cpp`),
placés dans `dash/assets/profiles/<XUID>/` :

| Fichier | Contenu utilisé |
|---|---|
| `FFFE07D1.gpd` | liste des jeux joués (compteurs, nom, date) ; icônes de jeux (image id = TitleID) |
| `<TitleID>.gpd` | succès (nom, descriptions, Gamerscore, obtenu ou non, date) ; images (0x8000 = icône, autres = succès) |

- Ce sont les fichiers d'un **vrai profil de console** : extraire le paquet du profil
  (`Content/<XUID>/FFFE07D1/00010000/…`, paquet STFS `CON`) avec `tools/stfs.py` et copier ses `.gpd`.
- Sans profil, `python3 tools/make_test_gpd.py dash/assets/profiles/B13EBABEBABEBABE` crée un
  profil de démonstration (deux jeux fictifs, succès en partie obtenus, images générées).
- Gamerscore de la carte : somme des jeux (`--dash_gamerscore=-1`, défaut), 0 sans GPD ; une valeur
  ≥ 0 le fixe.
- Écrans vérifiés : **Achievements** (tous jeux : nombre de succès, Gamerscore obtenu/total, images)
  et **Played Games** (liste, succès, Gamerscore, icônes). Le détail d'un jeu (Entrée) ne s'ouvre pas
  encore.
- `XamIsSystemTitleId` est implémentée : le stub du SDK faisait passer tout jeu pour un titre système.

Pour que la carte se remplisse, tous les réglages qu'elle demande doivent exister : le SDK fait
échouer la lecture entière si un seul est inconnu. `ApplyProfileSettings()` déclare donc les
réglages manquants (`0x10040001`, `0x1004003A`). Et la lecture des réglages n'est lancée qu'après
celle de l'image : sans image, pas de Gamerscore.

## Ce qui a été corrigé

### Analyse (`dash/dash_overrides.toml`)
- Fonctions feuilles de *dispatch* sans `.pdata`, enchaînées par des `b` de queue.
- 329 fonctions atteintes seulement via vtables/callbacks (`tools/find_ptr_funcs.py`, en excluant
  les tables de portée SEH `0x920E74EC-0x920E7E10` qui pointent au milieu des fonctions).
- 5 fonctions dont l'adresse est construite par `lis/addi` dans le code
  (`tools/find_ptr_funcs.py` + `tools/filter_inside.py`, qui rejette les 292 continuations de `catch`).
- Hooks de diagnostic (`src/diag_hooks.cpp`).

### Runtime (code du projet)
- `src/kernel_stubs.cpp` : `XUsbcam*` (exclus de la compilation du SDK) et l'import XAM ordinal 2790.
- `src/null_audio.h` : pilote audio silencieux cadencé à 48 kHz. Sans pilote audio, le Dashboard
  désenregistre la classe XUI `XuiSoundXAudio` et toutes ses scènes échouent (`0x80300004`).
- `src/dash_app.h` : charge `dash.xex` au lieu de `default.xex`, monte `assets/flash` en `\SystemRoot`.

### Patch du SDK (`third_party/rexglue-sdk`, `git diff`)
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

Contenu de `dash/assets/flash/` (monté en `\SystemRoot` / `media:`), sans dump NAND :
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
