#!/bin/sh
# Construit out/appimage/XenonDash-<version>-x86_64.AppImage (phase P6 ; voir README, « AppImage »).
# usage: tools/make_appimage.sh <version> [--no-strip] [--no-build-tools]
# --no-build-tools : sans les outils de recompilation de jeux (SDK, ngpu_core, XenosRecomp...) : AppImage légère, sans « Play Game »
# pour les jeux à recompiler.
# Le binaire dash (déjà compilé : preset linux-amd64-relwithdebinfo) et les bibliothèques du SDK sont embarqués ;
# AUCUN fichier Microsoft (dash.xex, polices, modules, shrdres) : import au premier lancement.
# Usage personnel : le binaire contient une traduction du code Microsoft, l'AppImage n'est pas redistribuable.
# Build embarqué : BUILD_DIR=<nom> (sous dashboards/<v>/out/build/). Par défaut : out/build/native si le CMakeLists de la
# version met DASH_NATIVE_GPU à ON (13604), sinon linux-amd64-relwithdebinfo. Le mode de rendu part dans
# usr/share/xenondash/gpu (native : plus de plugin xenos, shaders SPIR-V générés à l'import ; xenos : émulation, comme avant).
set -eu
V=${1:?usage: tools/make_appimage.sh <version> [--no-strip] [--no-build-tools]}
STRIP=1; BUILD_TOOLS=1
for a in "${2:-}" "${3:-}"; do
  [ "$a" = "--no-strip" ] && STRIP=0
  [ "$a" = "--no-build-tools" ] && BUILD_TOOLS=0
done
R=$(cd "$(dirname "$0")/.." && pwd)
BDIR=$R/dashboards/$V/out/build
# Build par défaut : celui que le projet prévoit (option DASH_NATIVE_GPU ON dans son CMakeLists.txt => native), sinon émulé
if grep -q '^option(DASH_NATIVE_GPU .* ON)' "$R/dashboards/$V/CMakeLists.txt" 2>/dev/null; then WANT=native; else WANT=linux-amd64-relwithdebinfo; fi
BUILD=${BUILD_DIR:-$WANT}
BIN=$BDIR/$BUILD/dash
[ -x "$BIN" ] || { echo "binaire absent : $BIN (compiler d'abord le projet)" >&2; exit 1; }
GPU=xenos
grep -q '^DASH_NATIVE_GPU:BOOL=ON' "$BDIR/$BUILD/CMakeCache.txt" 2>/dev/null && GPU=native
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
if [ ! -x "$TOOLS/appimagetool" ]; then
  echo "téléchargement d'appimagetool dans work/appimage-tools/"
  curl -fL -o "$TOOLS/appimagetool" \
    https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
  chmod +x "$TOOLS/appimagetool"
fi

A=$R/work/appimage/AppDir-$V
rm -rf "$A"
mkdir -p "$A/usr/bin" "$A/usr/share/xenondash/tools"
cp "$BIN" "$A/usr/bin/dash"
LIBS="librexruntime.so"; [ $GPU = xenos ] && LIBS="$LIBS librexgpu-xenos.so"
for l in $LIBS; do cp "$R/sdk/lib/$l" "$A/usr/bin/$l"; done
if [ $STRIP = 1 ]; then strip --strip-unneeded "$A/usr/bin/dash" "$A/usr/bin/"*.so; fi
# Dépendances dynamiques non système : SDL3, spdlog, ffmpeg... sont liés statiquement. Reste : libc/libstdc++/X11/
# wayland/xcb/Vulkan (loader et pilote) du système, volontairement non embarqués.
echo "dépendances système restantes :"
LD_LIBRARY_PATH=$A/usr/bin ldd "$A/usr/bin/dash" | awk '{print "  " $1}'

S=$A/usr/share/xenondash
echo "$V" > "$S/version"
echo "$GPU" > "$S/gpu"
cp "$R/games.example.toml" "$S/"
# Import (xenondash_import.py) : ces modules suffisent aussi à la génération des shaders natifs (dash_shaders, hud_shaders)
for f in port_dashboard.py stfs.py xex_resource.py xex_rekey.py xzp_alias.py d3d_match.py dash_shaders.py hud_shaders.py; do cp "$R/tools/$f" "$S/tools/"; done
# --- Outils de recompilation de jeux (recipe_build.py, bibliothèque de jeux) : la liste ci-dessous est faite pour être étendue
# (tout ce que le moteur de recettes appelle sous tools/ doit y figurer : tous les *.py, les dossiers de données, le gabarit).
TOOLS_DIRS="d3d_signatures templates"          # dossiers de tools/ copiés en entier
ROOT_DIRS="games ngpu_core native_gpu patches" # dossiers du dépôt copiés en entier (recettes, rendu natif, correctifs XenosRecomp)
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
OUT=$R/out/appimage/XenonDash-$V-x86_64.AppImage
ARCH=x86_64 "$TOOLS/appimagetool" --appimage-extract-and-run --no-appstream "$A" "$OUT"
ls -la "$OUT"; sha256sum "$OUT"
