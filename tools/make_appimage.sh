#!/bin/sh
# Construit out/appimage/XenonDash-<version>-x86_64.AppImage (phase P6 ; voir README, « AppImage »).
# usage: tools/make_appimage.sh <version> [--no-strip]
# Le binaire dash (déjà compilé : preset linux-amd64-relwithdebinfo) et les bibliothèques du SDK sont embarqués ;
# AUCUN fichier Microsoft (dash.xex, polices, modules, shrdres) : import au premier lancement.
# Usage personnel : le binaire contient une traduction du code Microsoft, l'AppImage n'est pas redistribuable.
set -eu
V=${1:?usage: tools/make_appimage.sh <version> [--no-strip]}
STRIP=1; [ "${2:-}" = "--no-strip" ] && STRIP=0
R=$(cd "$(dirname "$0")/.." && pwd)
BIN=$R/dashboards/$V/out/build/linux-amd64-relwithdebinfo/dash
[ -x "$BIN" ] || { echo "binaire absent : $BIN (compiler d'abord le projet)" >&2; exit 1; }
for f in sdk/lib/librexruntime.so sdk/lib/librexgpu-xenos.so tools/xexinfo/build/xexinfo; do
  [ -f "$R/$f" ] || { echo "manquant : $f" >&2; exit 1; }
done
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
for l in librexruntime.so librexgpu-xenos.so; do cp "$R/sdk/lib/$l" "$A/usr/bin/$l"; done
if [ $STRIP = 1 ]; then strip --strip-unneeded "$A/usr/bin/dash" "$A/usr/bin/"*.so; fi
# Dépendances dynamiques non système : SDL3, spdlog, ffmpeg... sont liés statiquement. Reste : libc/libstdc++/X11/
# wayland/xcb/Vulkan (loader et pilote) du système, volontairement non embarqués.
echo "dépendances système restantes :"
LD_LIBRARY_PATH=$A/usr/bin ldd "$A/usr/bin/dash" | awk '{print "  " $1}'

S=$A/usr/share/xenondash
echo "$V" > "$S/version"
cp "$R/games.example.toml" "$S/"
for f in port_dashboard.py stfs.py xex_resource.py xex_rekey.py xzp_alias.py; do cp "$R/tools/$f" "$S/tools/"; done
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
