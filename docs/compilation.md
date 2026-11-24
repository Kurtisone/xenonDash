# Compilation et environnement de build (XenonDash 13604)

Extrait de `REPRISE.md`. Les commandes de build et d installation du SDK y sont décrites.

## Environnement de compilation (hors poste de développement)

Sur une machine sans `clang`/`cmake`/`ninja` (SteamOS, `/usr` non inscriptible), tout passe par
**podman**, avec une image dérivée de `localhost/xenondash-sdk-builder` :

```sh
podman build -t xd-builder:latest -f - . <<'EOF'
FROM localhost/xenondash-sdk-builder:latest
USER root
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libvulkan-dev libxkbcommon-dev xxhash vulkan-tools xvfb mesa-vulkan-drivers \
      librsvg2-bin fuse file gdb strace && rm -rf /var/lib/apt/lists/*
EOF
```

- **Sous-modules** : `third_party/xenondash-sdk` doit être initialisé *avec les siens* :
  `git -C third_party/xenondash-sdk submodule update --init --recursive --depth 1`. Sans eux, le
  configure échoue sur `install TARGETS given target "xxhash"`.
- **SDK** : `cmake -S third_party/xenondash-sdk -B third_party/xenondash-sdk/out/build/linux-amd64 \
  -G "Ninja Multi-Config" -DCMAKE_INSTALL_PREFIX=$PWD/sdk`, puis
  `cmake --build … --config Release --target install` (≈ 10 min, 8 tâches). Après une modification
  du SDK : relancer `cmake --build third_party/xenondash-sdk/out/build/linux-amd64 --config Release --target install`
  (sur l'hôte : `/usr/bin/clang` et `/usr/bin/clang++` ; supprimer `CMakeCache.txt` si la configuration
  garde un chemin de compilateur du conteneur). Le dash lie les bibliothèques de `sdk/lib/` et le
  `rexglue` de `sdk/bin/`. Le dossier `out/linux-amd64` n'existe plus.
- **Codegen** : `/src/sdk/bin/rexglue codegen dash_manifest.toml` (≈ 1 min, 26 916 fonctions).
- **Dashboard** : configure dans `dashboards/2.0.13604.0-xam/out/build/linux-amd64-relwithdebinfo`
  avec `-DCMAKE_PREFIX_PATH=/src/sdk` (≈ 8 min).
- **AppImage** : `APPIMAGE_EXTRACT_AND_RUN=1 BUILD_DIR=linux-amd64-relwithdebinfo \
  ./tools/make_appimage.sh 2.0.13604.0-xam` — sortie fixe `out/appimage/XenonDash-Kinect.AppImage`
  (une seule AppImage, demande utilisateur ; le nom ne varie plus). `appimagetool` réclame `file`
  et `rsvg-convert` (paquet `librsvg2-bin`). **Dans le conteneur podman**, monter aussi le dossier
  `work/` du dépôt principal : `work` est un lien vers `/home/kurtisone/Projects/xbox360/work`, et sans ce
  montage l'étape d'empaquetage échoue (« cannot create directory … work »). Les modules recompilés
  embarqués sont listés par `module recompilé embarqué : libdash_*.so` dans le journal de build.
- **Fichiers absents du dépôt, nécessaires au build** : `dashboards/2.0.13604.0-xam/assets/sep/Guide.AvatarMiniCreator.xex`
  (copie de l'XEX de la mise à jour, ignorée par git via `*.xex`). Sans lui, le module d'avatar n'est pas compilé.
- **Validation minimale du XMB (procédure éprouvée, 2026-11-22)** : lancer `Xvfb :N -screen 0 1280x720x24
  -fbdir <dossier> &`, puis le dash avec `DISPLAY=:N DASH_AUDIO=nop DASH_XAM_MODULE=2 DASH_PROFILE_MODE=xam
  VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json LD_LIBRARY_PATH=<sdk>/lib
  --game_data_root=~/.local/share/xenondash/2.0.13604.0-xam --gpu_plugin=xenos` (en arrière-plan).
  Attendre **60 s**, puis `xdotool windowfocus $(xdotool search --onlyvisible . | tail -1)`, attendre
  5 s, capturer avec `tools/xwd2png.py <dossier>/Xvfb_screen0 <out.png>` et lire l'image. Sans le focus
  ou avant 60 s, la capture est noire même pour le build de référence. Résultat attendu : XMB (Xbox LIVE,
  My Xbox, Welcome, Getting Started, Kinect for Xbox 360, Family First). Vérifié le 2026-11-22 : build de
  référence `out/build/native/dash` et build du fork (`xenondash-sdk` f2c7948) donnent la même capture.
- **Banc visuel (conteneur)** : `Xvfb :N -screen 0 1280x720x24 -fbdir <dossier>` (créer le dossier
  et `/tmp/.X11-unix` chmod 1777 au préalable), capture par `tools/xwd2png.py <fbdir>/Xvfb_screen0
  <out.png>`, `VK_ICD_FILENAMES=…/lvp_icd.json` (lavapipe). Le GPU réel ne peut pas présenter sur
  Xvfb (`No DRI3 support`) et la capture de la session réelle (`:0`) est noire (KWin composite).
  Focus obligatoire : `xdotool windowfocus $(xdotool search --onlyvisible . | tail -1)` (sans WM,
  les touches tombent dans le vide). Pour reproduire la machine de l'utilisateur, monter ses VRAIS
  jeux (`-v /home/deck/XenonDash/Games:/games:ro`, `XENONDASH_GAMES_DIR=/games`) : une entrée de
  registre synthétique sans fichiers provoque un crash memset artéfact au premier present.
- **Sous gdb** : `handle SIGSEGV nostop noprint pass` EST OBLIGATOIRE — sinon gdb intercepte les
  fautes avant le récupérateur du SDK (`[rec2]`, restauration de page commitée) et fabrique des
  crashes imaginaires (vu sur le remplissage d'image du hub, `base+0xFE…` : n'existe pas hors gdb).

