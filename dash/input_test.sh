#!/bin/sh
# Scénario de test des entrées sous Xvfb : capture, touches, capture...
# usage: [DASH_GDB=1] [DASH_STEP=3] ./input_test.sh <nom> <attente_initiale_s> "<touches 1>" ...
#   DASH_GDB=1 : lance sous gdb (sans point d'arrêt). Plus lent, mais évite le blocage de
#   lavapipe sur certaines transitions d'écran (condition de course du rendu logiciel).
#   Chaque argument de touches est envoyé d'un bloc (tools/xkeys.py), puis une capture
#   runs/<nom>_<i>.png est prise 3 s plus tard. Capture runs/<nom>_0.png avant toute touche.
set -u
P=$(cd "$(dirname "$0")/.." && pwd); D=$P/dash; N=$1; W=$2; shift 2
FB=$D/runs/$N.fb; mkdir -p $FB
Xvfb :79 -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
env VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} ${DASH_ENV:-} DISPLAY=:79 \
    DASH_AUDIO=${DASH_AUDIO:-nop} LD_LIBRARY_PATH=$P/sdk/lib \
    ${DASH_GDB:+gdb -q -batch -ex "set debuginfod enabled off" -ex "handle SIGSEGV SIG32 SIG33 SIG34 SIG35 SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass" -ex run --args} \
    $D/out/build/linux-amd64-relwithdebinfo/dash --game_data_root=$D/assets ${DASH_ARGS:-} \
    > $D/runs/$N.out 2>&1 & DP=$!
sleep $W
python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $D/runs/${N}_0.png
i=1
for keys in "$@"; do
  # shellcheck disable=SC2086
  DISPLAY=:79 python3 -I $P/tools/xkeys.py $keys
  sleep ${DASH_STEP:-3}
  python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $D/runs/${N}_$i.png
  i=$((i+1))
done
kill -0 $DP 2>/dev/null && echo "dash vivant" || echo "dash a quitté"
kill -9 $DP 2>/dev/null; kill $XP 2>/dev/null
ls -t $D/out/build/linux-amd64-relwithdebinfo/logs/*.log | head -1
