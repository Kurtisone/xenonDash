#!/bin/sh
# Lance dash.xex recompilé dans un écran virtuel, capture l'écran au bout de N secondes, puis arrête.
# usage: ./run_headless.sh <secondes> <nom_du_run>
set -u
P=$(cd "$(dirname "$0")/.." && pwd); D=$P/dash; T=${1:-40}; N=${2:-run}
FB=$D/runs/$N.fb; mkdir -p $FB
Xvfb :77 -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
env ${DASH_ENV:-} DISPLAY=:77 DASH_AUDIO=${DASH_AUDIO:-nop} LD_LIBRARY_PATH=$P/sdk/lib $D/out/build/linux-amd64-relwithdebinfo/dash --game_data_root=$D/assets --gpu_plugin=xenos ${DASH_ARGS:-} > $D/runs/$N.out 2>&1 & DP=$!
i=0; while [ $i -lt $T ] && kill -0 $DP 2>/dev/null; do sleep 1; i=$((i+1)); done
python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $D/runs/$N.png
if kill -0 $DP 2>/dev/null; then echo "dash toujours vivant après ${T}s"; kill -9 $DP; else wait $DP; echo "dash terminé après ${i}s, code $?"; fi
kill $XP 2>/dev/null; ls -t $D/out/build/linux-amd64-relwithdebinfo/logs/*.log | head -1
