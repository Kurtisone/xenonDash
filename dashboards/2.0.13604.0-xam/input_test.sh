#!/bin/sh
# Scénario de test des entrées sous Xvfb : capture, touches, capture...
# usage: [DASH_GDB=1] [DASH_STEP=3] ./input_test.sh <nom> <attente_initiale_s> "<touches 1>" ...
#   DASH_GDB=1 : lance sous gdb (sans point d'arrêt). Plus lent, mais évite le blocage de
#   lavapipe sur certaines transitions d'écran (condition de course du rendu logiciel).
#   Jetons spéciaux : "wait:<motif du journal>" et "sleep:<s>" (voir plus bas).
#   Chaque argument de touches est envoyé d'un bloc (tools/xkeys.py), puis une capture
#   runs/<nom>_<i>.png est prise 3 s plus tard. Capture runs/<nom>_0.png avant toute touche.
set -u
# Écran X : DASH_DISPLAY (défaut 92) ; captures : DASH_RUNS (défaut runs/ du projet).
P=$(cd "$(dirname "$0")/../.." && pwd); D=$(cd "$(dirname "$0")" && pwd); N=$1; W=$2; shift 2
DN=${DASH_DISPLAY:-92}; RUNS=${DASH_RUNS:-$D/runs}
FB=$RUNS/$N.fb; mkdir -p $FB
Xvfb :$DN -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
env VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} ${DASH_ENV:-} DISPLAY=:$DN \
    DASH_AUDIO=${DASH_AUDIO:-nop} LD_LIBRARY_PATH=$P/sdk/lib \
    ${DASH_GDB:+gdb -q -batch -ex "set debuginfod enabled off" -ex "handle SIGSEGV SIG32 SIG33 SIG34 SIG35 SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass" -ex run --args} \
    $D/out/build/linux-amd64-relwithdebinfo/dash --game_data_root=$D/assets ${DASH_ARGS:-} \
    > $RUNS/$N.out 2>&1 & DP=$!
sleep $W
python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $RUNS/${N}_0.png
i=1
LOGDIR=$D/out/build/linux-amd64-relwithdebinfo/logs
# Jetons spéciaux (au lieu de touches), pour synchroniser le scénario sur l'avancement réel du Dashboard :
#   wait:<motif>  attend (300 s au plus) une nouvelle ligne du journal contenant ce motif (grep -E) ; le journal
#                 n'est écrit au fil de l'eau qu'avec DASH_ARGS="--log_flush_interval=1" ;
#   sleep:<s>     attend s secondes ;
#   chaque jeton spécial prend aussi une capture.
for keys in "$@"; do
  case "$keys" in
    wait:*)
      LOG=$(ls -t $LOGDIR/*.log | head -1); N0=$(wc -l < "$LOG"); T=0
      until [ "$(tail -n +$((N0+1)) "$LOG" | grep -a -cE "${keys#wait:}")" -gt 0 ] || [ $T -ge 300 ]; do sleep 2; T=$((T+2)); done
      echo "wait '${keys#wait:}' : ${T}s"
      python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $RUNS/${N}_$i.png; i=$((i+1)); continue ;;
    sleep:*)
      sleep "${keys#sleep:}"
      python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $RUNS/${N}_$i.png; i=$((i+1)); continue ;;
  esac
  # shellcheck disable=SC2086
  DISPLAY=:$DN python3 -I $P/tools/xkeys.py $keys
  sleep ${DASH_STEP:-3}
  python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $RUNS/${N}_$i.png
  i=$((i+1))
done
kill -0 $DP 2>/dev/null && echo "dash vivant" || echo "dash a quitté"
kill -9 $DP 2>/dev/null; kill $XP 2>/dev/null
ls -t $D/out/build/linux-amd64-relwithdebinfo/logs/*.log | head -1
