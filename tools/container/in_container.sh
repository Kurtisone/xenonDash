#!/bin/bash
# Exécuté DANS le conteneur (image xenondash-appimage-builder). Le worktree est monté au MÊME chemin que sur l'hôte.
# Compilateur : clang-20 + libstdc++ 14 (--gcc-install-dir), via ccache. Construction incrémentale.
set -e
R="${XD_ROOT:?}"
J="${XD_JOBS:-$(nproc)}"
G14=/usr/lib/gcc/x86_64-linux-gnu/14
cd "$R"
git config --global --add safe.directory '*'
FL="--gcc-install-dir=$G14"
export CC=clang CXX=clang++ CFLAGS="$FL" CXXFLAGS="$FL" LDFLAGS="$FL"
COMMON=(-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
  -DCMAKE_CXX_FLAGS="$FL" -DCMAKE_C_FLAGS="$FL" -DCMAKE_EXE_LINKER_FLAGS="$FL" -DCMAKE_SHARED_LINKER_FLAGS="$FL")
SDKB=third_party/xenondash-sdk/out/build/linux-amd64
DASHB=dashboards/2.0.13604.0-xam/out/build/native

SDK_HASH="$(git rev-parse HEAD:third_party/xenondash-sdk)"
MARK=sdk/.installed-from
if [ -f "$MARK" ] && [ "$(cat "$MARK")" = "$SDK_HASH" ] && [ -d "$SDKB" ]; then
  echo "== SDK inchangé ($SDK_HASH) : étape ignorée"
else
  echo "== SDK configure"
  cmake -S third_party/xenondash-sdk -B "$SDKB" -G "Ninja Multi-Config" "${COMMON[@]}" \
    -DCMAKE_INSTALL_PREFIX="$R/sdk"
  echo "== SDK install"
  cmake --build "$SDKB" --config Release --target install -j"$J"
  mkdir -p sdk && echo "$SDK_HASH" > "$MARK"
fi

echo "== dash configure + build"
cmake -S dashboards/2.0.13604.0-xam -B "$DASHB" -G Ninja "${COMMON[@]}" -DCMAKE_PREFIX_PATH="$R/sdk"
cmake --build "$DASHB" -j"$J"
ccache -s || true
echo "== AppImage"
APPIMAGE_EXTRACT_AND_RUN=1 BUILD_DIR=native ./tools/make_appimage.sh 2.0.13604.0-xam
echo "== DONE"
