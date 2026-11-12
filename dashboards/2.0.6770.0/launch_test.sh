#!/bin/sh
# Test P5 sous Xvfb + lavapipe : lance le Dashboard, envoie des touches, capture, et suit le relancement
# après un jeu (le Dashboard quitte, un superviseur lance le jeu puis relance le Dashboard).
# usage: ./launch_test.sh <nom> <attente_initiale_s> "<touches|sleep:s|wait:motif>" ...
#   DASH_GDB=1 [DASH_GDB_SCRIPT=fichier] : exécution sous gdb (point d'arrêt/traces, plus lent)
#   DASH_ARGS="--dash_launch=45410813 ..." : options supplémentaires du Dashboard
# Tous les processus du test (Xvfb, Dashboard, superviseur, jeu) sont tués à la fin : on les retrouve par leur
# variable DISPLAY (affichage Xvfb propre au test).
set -u
P=$(cd "$(dirname "$0")/../.." && pwd); D=$(cd "$(dirname "$0")" && pwd); N=$1; W=$2; shift 2
n=${XVFB_BASE:-100}; while [ -e /tmp/.X11-unix/X$n ] || [ -e /tmp/.X$n-lock ]; do n=$((n+1)); done
DISP=:$n
FB=$D/runs/$N.fb; mkdir -p $FB
Xvfb $DISP -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
cleanup() {
  for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ "$pid" = "$$" ] && continue
    if cat $p/environ 2>/dev/null | tr '\0' '\n' | grep -qx "DISPLAY=$DISP"; then kill -9 $pid 2>/dev/null; fi
  done
  kill $XP 2>/dev/null
}
trap cleanup EXIT INT TERM
BIN=$D/out/build/linux-amd64-relwithdebinfo/dash
LOGDIR=$D/out/build/linux-amd64-relwithdebinfo/logs
env VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} MESA_VK_WSI_PRESENT_MODE=immediate \
    ${DASH_ENV:-} DISPLAY=$DISP DASH_AUDIO=${DASH_AUDIO:-nop} LD_LIBRARY_PATH=$P/sdk/lib \
    ${DASH_GDB:+gdb -q -batch -ex "set debuginfod enabled off" -ex "handle SIGSEGV SIG32 SIG33 SIG34 SIG35 SIGUSR1 SIGUSR2 SIGPIPE nostop noprint pass" ${DASH_GDB_SCRIPT:+-x $DASH_GDB_SCRIPT} -ex run --args} \
    $BIN --game_data_root=$D/assets --log_flush_interval=1 ${DASH_ARGS:-} > $D/runs/$N.out 2>&1 &
sleep $W
python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $D/runs/${N}_0.png
i=1
for keys in "$@"; do
  case "$keys" in
    wait:*)
      LOG=$(ls -t $LOGDIR/*.log | head -1); N0=$(wc -l < "$LOG"); T=0
      until [ "$(tail -n +$((N0+1)) "$LOG" | grep -a -cE "${keys#wait:}")" -gt 0 ] || [ $T -ge 300 ]; do sleep 2; T=$((T+2)); LOG=$(ls -t $LOGDIR/*.log | head -1); done
      echo "wait '${keys#wait:}' : ${T}s" ;;
    sleep:*) sleep "${keys#sleep:}" ;;
    kill:*)  # tue (dans ce test seulement) les processus dont la ligne de commande contient le motif
      for p in /proc/[0-9]*; do
        pid=${p#/proc/}
        if cat $p/environ 2>/dev/null | tr '\0' '\n' | grep -qx "DISPLAY=$DISP" && tr '\0' ' ' < $p/cmdline 2>/dev/null | grep -q -- "${keys#kill:}"; then
          echo "kill $pid $(tr '\0' ' ' < $p/cmdline | cut -c1-100)"; kill $pid 2>/dev/null; fi
      done ;;
    *) DISPLAY=$DISP python3 -I $P/tools/xkeys.py $keys; sleep ${DASH_STEP:-4} ;;
  esac
  python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $D/runs/${N}_$i.png
  i=$((i+1))
done
echo "processus du test :"
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  if cat $p/environ 2>/dev/null | tr '\0' '\n' | grep -qx "DISPLAY=$DISP"; then echo "  $pid $(tr '\0' ' ' < $p/cmdline | cut -c1-150)"; fi
done
ls -t $LOGDIR/*.log | head -2
