# Compilation et environnement de build (XenonDash 13604)

Commandes de build et d'installation du SDK. Mise en place depuis les sources : [technique.md](technique.md).

## Hors poste de développement

Sur une machine sans `clang`/`cmake`/`ninja` (SteamOS, `/usr` non inscriptible), tout passe par **podman**, avec une image dérivée de `localhost/xenondash-sdk-builder` :

```sh
podman build -t xd-builder:latest -f - . <<'EOF'
FROM localhost/xenondash-sdk-builder:latest
USER root
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libvulkan-dev libxkbcommon-dev xxhash vulkan-tools xvfb mesa-vulkan-drivers \
      librsvg2-bin fuse file gdb strace && rm -rf /var/lib/apt/lists/*
EOF
```

## Étapes

- **Sous-modules** : `git -C third_party/xenondash-sdk submodule update --init --recursive --depth 1` (sans eux, le configure échoue sur `install TARGETS given target "xxhash"`).
- **SDK** (≈ 10 min) : `cmake -S third_party/xenondash-sdk -B third_party/xenondash-sdk/out/build/linux-amd64 -G "Ninja Multi-Config" -DCMAKE_INSTALL_PREFIX=$PWD/sdk`, puis
  `cmake --build third_party/xenondash-sdk/out/build/linux-amd64 --config Release --target install` (à refaire après toute modification du SDK ; sur l'hôte `/usr/bin/clang` et `clang++`,
  supprimer `CMakeCache.txt` s'il garde un compilateur du conteneur). Le Dashboard lie `sdk/lib/` et utilise `sdk/bin/rexglue`.
- **Codegen** (≈ 1 min) : `sdk/bin/rexglue codegen dash_manifest.toml` dans `dashboards/2.0.13604.0-xam` (dans le conteneur, le dépôt est monté sur `/src`).
- **Dashboard** (≈ 8 min) : configure dans `dashboards/2.0.13604.0-xam/out/build/linux-amd64-relwithdebinfo`. Le `CMakeLists.txt` impose le SDK du dépôt (`sdk/`) et sa version exacte, lue dans
  `sdk/lib/cmake/rexglue/rexglueConfigVersion.cmake` ; `-DREXSDK_DIR=` utilise un arbre source du SDK à la place.
- **AppImage** : `APPIMAGE_EXTRACT_AND_RUN=1 BUILD_DIR=linux-amd64-relwithdebinfo ./tools/make_appimage.sh 2.0.13604.0-xam` ; sortie fixe `out/appimage/XenonDash-Kinect.AppImage`. `appimagetool` demande
  `file` et `rsvg-convert` (`librsvg2-bin`). Dans le conteneur, monter aussi le dossier `work/` du dépôt principal (`work` est un lien : sans lui l'empaquetage échoue).
  Le journal liste les modules embarqués (`module recompilé embarqué : libdash_*.so`).
- **Fichier absent du dépôt, nécessaire au build** : `dashboards/2.0.13604.0-xam/assets/sep/Guide.AvatarMiniCreator.xex` (copie de l'XEX de la mise à jour, ignorée par git) ; sans lui, le module d'avatar n'est pas compilé.

## Banc visuel (conteneur ou hôte)

`Xvfb :N -screen 0 1280x720x24 -fbdir <dossier>` (créer le dossier et `/tmp/.X11-unix` en `chmod 1777`), capture `tools/xwd2png.py <fbdir>/Xvfb_screen0 <out.png>`, `VK_ICD_FILENAMES=…/lvp_icd.json` (lavapipe).
Un vrai GPU ne présente pas sur Xvfb (`No DRI3 support`) et la capture de la session réelle (`:0`) est noire. Le focus est obligatoire : `xdotool windowfocus $(xdotool search --onlyvisible . | tail -1)`.
Pour reproduire la machine de l'utilisateur, monter ses vrais jeux (`-v /home/deck/XenonDash/Games:/games:ro`, `XENONDASH_GAMES_DIR=/games`) : une entrée synthétique sans fichiers plante en memset au premier present.
Lancement minimal : `DISPLAY=:N DASH_AUDIO=nop VK_ICD_FILENAMES=… LD_LIBRARY_PATH=<sdk>/lib dash --game_data_root=~/.local/share/xenondash/2.0.13604.0-xam`, attendre **60 s** avant la capture (noire avant).
Résultat attendu : XMB (Xbox LIVE, My Xbox, Welcome, Getting Started, Kinect for Xbox 360, Family First). Pour un banc pas à pas : `tools/banc.py` ([REPRISE.md](REPRISE.md)).

**Sous gdb** : `handle SIGSEGV nostop noprint pass` est obligatoire (sinon gdb intercepte les fautes avant le récupérateur du SDK, `[rec2]`, et fabrique des plantages qui n'existent pas hors gdb).
