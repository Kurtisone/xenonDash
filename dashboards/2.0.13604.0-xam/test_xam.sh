#!/bin/sh
# Lance le Dashboard avec XAM recompilé (variante 2.0.13604.0-xam), pour vérifier à l'écran.
#
# usage: ./test_xam.sh [secondes]
#
# HUD=1 active le HUD de XAM (boîte modale, fond atténué) ; défaut 0, qui montre le
# Dashboard tel qu'il est rendu. PROFILE= (vide) revient au mode HLE.
set -u
D=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$D/../.." && pwd)

HUD=${HUD:-0}
PROFILE=${PROFILE:-xam}

ENVV="DASH_PROFILE_MODE=$PROFILE DASH_XAM_MODULE=2"
[ "$HUD" = 0 ] && ENVV="$ENVV DASH_HUD_DISABLE=1"

for f in "assets/dash.xex" "assets/flash/xam.xex"; do
  [ -f "$D/$f" ] || { echo "manquant : $D/$f" >&2; exit 1; }
done

# Le tas invité est un mappage partagé de 4,5 Go. Avec le SDK à jour (memfd_create) la
# taille de /dev/shm n'importe plus ; avec un SDK ancien, il faut ~5 Go libres.
SHM_KB=$(df -k /dev/shm 2>/dev/null | awk 'NR==2 {print $2}')
if [ -n "${SHM_KB:-}" ] && [ "$SHM_KB" -lt 5000000 ]; then
  echo "note : /dev/shm = $((SHM_KB / 1024)) Mo (suffisant avec le SDK à jour)." >&2
fi

echo "profil=$PROFILE  hud=$HUD"
echo "touches : A = Espace ou ';'   B = Retour arrière   Start = Entrée   flèches = stick droit"
echo

if [ -n "${1:-}" ]; then
  env $ENVV LD_LIBRARY_PATH="$P/sdk/lib" timeout "$1" \
    "$D/out/build/linux-amd64-relwithdebinfo/dash" --game_data_root="$D/assets"
else
  exec env $ENVV LD_LIBRARY_PATH="$P/sdk/lib" \
    "$D/out/build/linux-amd64-relwithdebinfo/dash" --game_data_root="$D/assets"
fi
