#!/usr/bin/env bash
# XenosRecomp unifié (P8, étape 2b) : clone l'amont (hedge-dev, MIT) dans work/XenosRecomp-unified à la révision
# épinglée, applique la série patches/xenosrecomp/*.patch (Skate1Native + XenonDash + Trials ReVolution) et compile.
# Aucun dépôt à créer : la série de correctifs fait foi. Résultat : work/XenosRecomp-unified/build/XenosRecomp/XenosRecomp
#
# Variables : XENOSRECOMP_URL (amont), XENOSRECOMP_REF (révision, défaut 990d03b), XENOSRECOMP_DIR (destination),
#             XENOSRECOMP_REFERENCE (clone local servant de référence d'objets, accélère le premier clone).
# Comportements facultatifs du binaire (variables d'environnement à l'exécution) :
#   XENOS_RECOMP_MICROCODE_KEY=1  clés de cache = hachage du microcode (Skate)
#   XENOS_RECOMP_NO_ZSTD=1        cache SPIR-V non compressé (Skate)
#   XENOS_RECOMP_TRIALS=1         extensions de Trials (export mémoire, LOD, pixels invités...)
# Sans variable : sortie adaptée au Dashboard 360 (tables de constantes absentes, échantillonneurs non nommés).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
URL="${XENOSRECOMP_URL:-https://github.com/hedge-dev/XenosRecomp}"
REF="${XENOSRECOMP_REF:-990d03b}"
DIR="${XENOSRECOMP_DIR:-$ROOT/work/XenosRecomp-unified}"
REFERENCE="${XENOSRECOMP_REFERENCE:-$ROOT/work/XenosRecomp}"
PATCHES=("$ROOT"/patches/xenosrecomp/*.patch)
WANTED="$(cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)-$REF"
STAMP="$DIR/.unified-patches"

if [[ ! -d $DIR/.git ]]; then
  mkdir -p "$(dirname "$DIR")"
  ref_args=()
  [[ -d $REFERENCE/.git ]] && ref_args=(--reference "$REFERENCE")
  git clone -q --recursive "${ref_args[@]}" "$URL" "$DIR"
fi

if [[ ! -f $STAMP || $(cat "$STAMP") != "$WANTED" ]]; then
  git -C "$DIR" reset -q --hard
  git -C "$DIR" clean -fdq -e build -e .unified-patches
  git -C "$DIR" checkout -q "$REF"
  git -C "$DIR" submodule update -q --init --recursive
  for p in "${PATCHES[@]}"; do
    # git apply : pas d'identité git requise ; le détail de chaque correctif est dans son en-tête
    git -C "$DIR" apply --whitespace=nowarn "$p"
  done
  echo "$WANTED" > "$STAMP"
  rm -rf "$DIR/build"
fi

cd "$DIR"
CC="${CC:-clang}" CXX="${CXX:-clang++}" cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release > build.configure.log 2>&1 \
  || { tail -20 build.configure.log; exit 1; }
cmake --build build -j"$(nproc)" > build.log 2>&1 || { tail -30 build.log; exit 1; }
echo "XenosRecomp unifié prêt : $DIR/build/XenosRecomp/XenosRecomp"
