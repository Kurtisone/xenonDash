#!/bin/sh
# usage: run_n.sh <display> <secs> <name> <extra args...>   (env passthrough)
D=$1; T=$2; N=$3; shift 3
P=/home/kurtisone/Projects/xbox360; V=$P/dashboards/2.0.6770.0
FB=$V/runs/$N.fb; mkdir -p $FB
Xvfb :$D -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
env MESA_VK_WSI_PRESENT_MODE=immediate VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:$D DASH_AUDIO=nop LD_LIBRARY_PATH=$P/sdk/lib $V/out/build/linux-amd64-relwithdebinfo/dash --game_data_root=$V/assets "$@" > $V/runs/$N.out 2>&1 & DP=$!
i=0; while [ $i -lt $T ] && kill -0 $DP 2>/dev/null; do sleep 1; i=$((i+1)); done
python3 -I $P/tools/xwd2png.py $FB/Xvfb_screen0 $V/runs/$N.png
if kill -0 $DP 2>/dev/null; then echo "vivant après ${T}s"; kill -9 $DP; else wait $DP; echo "terminé après ${i}s code $?"; fi
kill $XP 2>/dev/null; rm -rf $FB
