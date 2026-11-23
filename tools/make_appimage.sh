#!/bin/sh
# Construit out/appimage/XenonDash-Kinect.AppImage (phase P6 ; voir docs/technique.md, « AppImage »).
# Une seule AppImage (demande du 2026-11-21 : « pas de variantes, une XenonDash-Kinect, point ») :
# l'argument <version> choisit seulement l'arbre de build (dashboards/<version>) dont embarquer le
# binaire et les modules recompilés — le mode XAM s'active tout seul quand libdash_xam.so est là.
# usage: tools/make_appimage.sh <version> [--no-strip] [--no-build-tools] [--no-toolchain]
# --no-build-tools : sans les outils de recompilation de jeux (SDK, ngpu_core, XenosRecomp...) : AppImage légère, sans « Play Game »
# --no-toolchain : sans clang/cmake/ninja embarqués (les jeux devront être recompilés ailleurs) ;
#                 par défaut la toolchain est embarquée si elle est trouvée sur la machine de build
# pour les jeux à recompiler.
# Le binaire dash (déjà compilé : preset linux-amd64-relwithdebinfo) et les bibliothèques du SDK sont embarqués ;
# AUCUN fichier Microsoft (dash.xex, polices, modules, shrdres) : import au premier lancement.
# Usage personnel : le binaire contient une traduction du code Microsoft, l'AppImage n'est pas redistribuable.
# Build embarqué : BUILD_DIR=<nom> (sous dashboards/<v>/out/build/). Par défaut : out/build/native si le CMakeLists de la
# version met DASH_NATIVE_GPU à ON (13604), sinon linux-amd64-relwithdebinfo. Le mode de rendu part dans
# usr/share/xenondash/gpu (native : plus de plugin xenos, shaders SPIR-V générés à l'import ; xenos : émulation, comme avant).
set -eu
USAGE="usage: tools/make_appimage.sh <version> [--no-strip] [--no-build-tools] [--no-toolchain]"
V=${1:?$USAGE}
shift
STRIP=1; BUILD_TOOLS=1; TOOLCHAIN=1
for a in "$@"; do
  case $a in
    --no-strip) STRIP=0;;
    --no-build-tools) BUILD_TOOLS=0;;
    --no-toolchain) TOOLCHAIN=0;;
    *) echo "argument inconnu : $a" >&2; echo "$USAGE" >&2; exit 2;;
  esac
done
R=$(cd "$(dirname "$0")/.." && pwd)
BDIR=$R/dashboards/$V/out/build
# Build par défaut : celui que le projet prévoit (option DASH_NATIVE_GPU ON dans son CMakeLists.txt => native), sinon émulé
if grep -q '^option(DASH_NATIVE_GPU .* ON)' "$R/dashboards/$V/CMakeLists.txt" 2>/dev/null; then WANT=native; else WANT=linux-amd64-relwithdebinfo; fi
BUILD=${BUILD_DIR:-$WANT}
BIN=$BDIR/$BUILD/dash
[ -x "$BIN" ] || { echo "binaire absent : $BIN (compiler d'abord le projet)" >&2; exit 1; }
GPU=xenos
# Le rendu natif est une définition de compilation (target_compile_definitions DASH_NATIVE_GPU=1 dans le CMakeLists du
# Dashboard), pas une option de cache : on la cherche dans build.ninja. Sinon l'AppImage serait marquée xenos à tort
# (--native-shaders jamais passé, shaders du HUD absents).
grep -q '^DASH_NATIVE_GPU:BOOL=ON' "$BDIR/$BUILD/CMakeCache.txt" 2>/dev/null && GPU=native
grep -q 'DDASH_NATIVE_GPU=1' "$BDIR/$BUILD/build.ninja" 2>/dev/null && GPU=native
echo "build embarqué : $BUILD (rendu : $GPU)"
if [ $GPU = native ] && grep -a -q 'librexgpu-xenos' "$BIN"; then
  echo "le binaire natif référence encore le plugin xenos : refusé" >&2; exit 1
fi
for f in sdk/lib/librexruntime.so tools/xexinfo/build/xexinfo; do
  [ -f "$R/$f" ] || { echo "manquant : $f" >&2; exit 1; }
done
[ $GPU = xenos ] && { [ -f "$R/sdk/lib/librexgpu-xenos.so" ] || { echo "manquant : sdk/lib/librexgpu-xenos.so" >&2; exit 1; }; }
TOOLS=$R/work/appimage-tools
mkdir -p "$TOOLS"
# appimagetool : version « continuous » (non figée), sans somme de contrôle vérifiée. APPIMAGETOOL_URL=<url> pour imposer une
# version publiée ; pour vérifier l'outil, le déposer soi-même (après contrôle) dans work/appimage-tools/appimagetool.
APPIMAGETOOL_URL=${APPIMAGETOOL_URL:-https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage}
if [ ! -x "$TOOLS/appimagetool" ]; then
  echo "téléchargement d'appimagetool dans work/appimage-tools/ : $APPIMAGETOOL_URL"
  echo "AVERTISSEMENT : appimagetool téléchargé sans version figée ni somme de contrôle (APPIMAGETOOL_URL pour en imposer une)" >&2
  curl -fL -o "$TOOLS/appimagetool" "$APPIMAGETOOL_URL"
  chmod +x "$TOOLS/appimagetool"
fi

A=$R/work/appimage/AppDir-$V
rm -rf "$A"
mkdir -p "$A/usr/bin" "$A/usr/share/xenondash/tools"
cp "$BIN" "$A/usr/bin/dash"
LIBS="librexruntime.so"; [ $GPU = xenos ] && LIBS="$LIBS librexgpu-xenos.so"
for l in $LIBS; do cp "$R/sdk/lib/$l" "$A/usr/bin/$l"; done
# Modules recompilés de la variante (libdash_xam.so, libdash_hud.so…) : le chargeur les cherche
# à côté de l'exécutable (GetExecutableFolder). Sans eux, l'AppImage retombe en mode HLE : pas de
# Guide, pas de HUD XAM (mesure du 2026-11-21 : lancement neutre, « Registering module » absent).
for m in "$BDIR/$BUILD"/libdash_*.so; do
  [ -f "$m" ] && cp "$m" "$A/usr/bin/" && echo "module recompilé embarqué : $(basename "$m")"
done
if [ $STRIP = 1 ]; then strip --strip-unneeded "$A/usr/bin/dash" "$A/usr/bin/"*.so; fi
# Dépendances dynamiques non système : SDL3, spdlog, ffmpeg... sont liés statiquement. Reste : libc/libstdc++/X11/
# wayland/xcb/Vulkan (loader et pilote) du système, volontairement non embarqués.
echo "dépendances système restantes :"
LD_LIBRARY_PATH=$A/usr/bin ldd "$A/usr/bin/dash" | awk '{print "  " $1}'

S=$A/usr/share/xenondash
echo "$V" > "$S/version"
echo "$GPU" > "$S/gpu"
# Variante XAM : si le module XAM recompilé est embarqué, l'AppRun activera le mode XAM
# (DASH_XAM_MODULE=2 : chargement + point d'entrée ; DASH_PROFILE_MODE=xam : parcours console).
if [ -f "$A/usr/bin/libdash_xam.so" ]; then
  echo 2 > "$S/xam"
  echo "mode XAM activé dans l'AppRun (modules recompilés présents)"
fi
cp "$R/games.example.toml" "$S/"
# Import (xenondash_import.py) : ces modules suffisent aussi à la génération des shaders natifs (dash_shaders, hud_shaders)
for f in port_dashboard.py stfs.py xex_resource.py xex_rekey.py xzp_alias.py d3d_match.py dash_shaders.py hud_shaders.py; do cp "$R/tools/$f" "$S/tools/"; done
# --- Outils de recompilation de jeux (recipe_build.py, bibliothèque de jeux) : la liste ci-dessous est faite pour être étendue
# (tout ce que le moteur de recettes appelle sous tools/ doit y figurer : tous les *.py, les dossiers de données, le gabarit).
TOOLS_DIRS="d3d_signatures templates"          # dossiers de tools/ copiés en entier
ROOT_DIRS="games ngpu_core native_gpu patches" # dossiers du dépôt copiés en entier (recettes, rendu natif, correctifs XenosRecomp)
# Toolchain de compilation (clang, clang++, cmake, ninja) : permet de recompiler un jeu depuis
# l'AppImage, sans rien installer sur l'hôte. ~290 Mo : c'est le prix d'un clang autonome
# (libLLVM + libclang-cpp). --no-toolchain pour l'omettre.
if [ $BUILD_TOOLS = 1 ] && [ $TOOLCHAIN = 1 ]; then
  if command -v clang >/dev/null 2>&1 && command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1; then
    python3 "$R/tools/appimage/embed_toolchain.py" /usr "$S/toolchain" || \
      echo "avertissement : toolchain non embarquée (les jeux ne pourront pas être recompilés depuis l'AppImage)" >&2
  else
    echo "avertissement : clang/cmake/ninja absents de la machine de build : toolchain non embarquée" >&2
  fi
fi
if [ $BUILD_TOOLS = 1 ]; then
  for need in sdk/bin/rexglue sdk/include sdk/lib work/XenosRecomp-unified/build/XenosRecomp/XenosRecomp; do
    [ -e "$R/$need" ] || { echo "manquant : $need (voir tools/build_xenosrecomp.sh et le SDK installé dans sdk/) ; --no-build-tools pour s'en passer" >&2; exit 1; }
  done
  cp "$R"/tools/*.py "$S/tools/"
  for d in $TOOLS_DIRS; do cp -a "$R/tools/$d" "$S/tools/"; done
  for d in $ROOT_DIRS; do cp -a "$R/$d" "$S/"; done
  # xxhsum (XXH3 des shaders, ne dépend que de libc) et dxc-linux (XenosRecomp) : embarqués pour l'import hors système
  XXH=$(command -v xxhsum) || { echo "manquant : xxhsum (paquet xxhash)" >&2; exit 1; }
  cp "$XXH" "$S/tools/xxhsum"
  find "$S" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null
  rm -rf "$S/games/"*/tests            # tests des recettes : inutiles à l'exécution
  cp -a "$R/sdk" "$S/sdk"              # binaire rexglue, en-têtes, bibliothèques statiques, cmake : le projet d'un jeu s'y lie
  XR=work/XenosRecomp-unified
  mkdir -p "$S/$XR/build/XenosRecomp"
  cp "$R/$XR/build/XenosRecomp/XenosRecomp" "$S/$XR/build/XenosRecomp/"
  cp "$R/$XR/thirdparty/dxc-bin/lib/x64/libdxcompiler.so" "$R/$XR/thirdparty/dxc-bin/lib/x64/libdxil.so" "$S/$XR/build/XenosRecomp/"
  mkdir -p "$S/$XR/XenosRecomp"; cp "$R/$XR/XenosRecomp/shader_common.h" "$S/$XR/XenosRecomp/"   # lu par XenosRecomp à l'exécution (3e argument)
  mkdir -p "$S/$XR/thirdparty/dxc-bin/bin/x64"; cp "$R/$XR/thirdparty/dxc-bin/bin/x64/dxc-linux" "$S/$XR/thirdparty/dxc-bin/bin/x64/"
  if [ $STRIP = 1 ]; then strip --strip-unneeded "$S/$XR/build/XenosRecomp/XenosRecomp" "$S/sdk/bin/rexglue" 2>/dev/null || true; fi
  echo "outils de recompilation embarqués : $(du -sh "$S" | cut -f1) (non compressé)"
fi
cp "$R/tools/appimage/xenondash_import.py" "$S/tools/"
mkdir -p "$S/tools/xexinfo/build"; cp "$R/tools/xexinfo/build/xexinfo" "$S/tools/xexinfo/build/"
cp "$R/tools/appimage/AppRun" "$A/AppRun"; chmod +x "$A/AppRun"
sed "s/@VERSION@/$V/g" "$R/tools/appimage/xenondash.desktop" > "$A/xenondash.desktop"
cp "$R/tools/appimage/xenondash.svg" "$A/xenondash.svg"
mkdir -p "$A/usr/share/icons/hicolor/256x256/apps"
rsvg-convert -w 256 -h 256 "$R/tools/appimage/xenondash.svg" -o "$A/xenondash.png"
cp "$A/xenondash.png" "$A/usr/share/icons/hicolor/256x256/apps/xenondash.png"
cp "$A/xenondash.png" "$A/.DirIcon"

mkdir -p "$R/out/appimage"
OUT=$R/out/appimage/XenonDash-Kinect.AppImage
ARCH=x86_64 "$TOOLS/appimagetool" --appimage-extract-and-run --no-appstream "$A" "$OUT"
ls -la "$OUT"; sha256sum "$OUT"
