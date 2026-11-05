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
git -C third_party/rexglue-sdk apply ../../patches/rexglue-sdk-0001-function-table-heap.patch
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
| `dash/assets/flash/shrdres.xzp` | ressource `shrdres` de `xam.xex` (ici celle de la 2.0.17559) |

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
LD_LIBRARY_PATH=../sdk/lib out/build/linux-amd64-relwithdebinfo/dash \
    --game_data_root=assets --gpu_plugin=xenos

# 2b. Sans écran ni carte son (SSH) : Xvfb + capture dans runs/<nom>.png
./run_headless.sh 40 essai
```

Journaux : `dash/out/build/linux-amd64-relwithdebinfo/logs/`. `--log_level=trace` pour tout voir.
`DASH_AUDIO=nop` active le backend audio silencieux (`src/null_audio.h`).

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

## État actuel

**Le Dashboard s'affiche** (onglets Blades, carte joueur, menus Games) : voir `dash/runs/run16.png`.

Contenu de `dash/assets/flash/` (monté en `\SystemRoot` / `media:`), sans dump NAND :
- `shrdres.xzp` : ressource `shrdres` extraite de `$flash_xam.xex` de la mise à jour officielle
  **2.0.17559** (`firmware/17559/`, XZP v3 ; le Dashboard 6770 l'accepte).
- `XenonJKLatin.xtt` / `XenonCLatin.xtt` : copies de `XenonSCLatin.xtt` (police complète de la 17559).
  Substitut : il manque des glyphes (« G » de Gamerscore, icônes des boutons A/B/X/Y).
  Les vraies polices sont la base NAND 1888 + les deltas `$flash_xenon*latin.xttp`.
- `systemupdate.xex`, `ximecore.xex`, `xmsgr.xex`, `dash.xex` : copiés de la mise à jour 6770.

Sous Xvfb, RADV ne peut pas présenter (pas de DRI3) : utiliser lavapipe :
`DASH_ENV="VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json" ./run_headless.sh 90 essai`
(sur un vrai bureau, RADV marche directement).

## Problèmes connus
- `0x9222D378 → 0x9222D328` : retour d'un *funclet* `catch` dans sa fonction parente, non géré
  (ne s'exécute que si une exception C++ est levée). Piste : `enable_exception_handlers`.
- `XboxKrnlBaseVersion` (variable noyau) non implémentée.
- `DASHUSER:` (thèmes, styles) non monté ; profils (`XamProfileOpen`, `XamReadTileEx`…) en stub.
- Stubs : `XamGetLanguage`, `XGetAudioFlags`, `HalGetCurrentAVPack`, `XamSetDashContext`, messages XMP.
