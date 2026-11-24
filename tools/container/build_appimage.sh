#!/bin/bash
# Script hôte : construit (si besoin) l'image de build puis lance la construction incrémentale de l'AppImage.
# Usage : tools/container/build_appimage.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd "$HERE/../.." && pwd)"
WORK="${XD_WORK_DIR:-/home/kurtisone/Projects/xbox360/work}"
CCACHE_HOST="${XD_CCACHE_DIR:-$HOME/.cache/xenondash-ccache}"
IMAGE=localhost/xenondash-appimage-builder:latest

mkdir -p "$CCACHE_HOST"
SUM="$(sha256sum "$HERE/Containerfile" | cut -d' ' -f1)"
CUR="$(podman image inspect --format '{{ index .Labels "xd.containerfile" }}' "$IMAGE" 2>/dev/null || true)"
if [ "$CUR" != "$SUM" ]; then
  echo "== Construction de l'image $IMAGE"
  podman build -t "$IMAGE" --label "xd.containerfile=$SUM" -f "$HERE/Containerfile" "$HERE"
fi

# Worktree : le fichier .git pointe vers le dépôt commun (…/.git/worktrees/<nom>) : il doit être monté au même chemin,
# sinon git rev-parse échoue dans le conteneur.
GITDIR="$(cd "$R" && cd "$(git rev-parse --git-common-dir)" && pwd)"
exec podman run --rm --userns=keep-id:uid=0,gid=0 \
  -v "$R:$R" -v "$WORK:$WORK" -v "$GITDIR:$GITDIR" -v "$CCACHE_HOST:/ccache" \
  -e CCACHE_DIR=/ccache -e XD_JOBS="$(nproc)" -e XD_ROOT="$R" \
  -w "$R" "$IMAGE" bash "$R/tools/container/in_container.sh"
