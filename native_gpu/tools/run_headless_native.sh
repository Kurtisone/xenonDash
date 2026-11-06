#!/bin/sh
# Lance le Dashboard 6770 compilé avec DASH_NATIVE_GPU=ON sous Xvfb + lavapipe, capture l'écran au bout de N secondes.
# usage: run_headless_native.sh <affichage_Xvfb> <secondes> <nom_du_run> [arguments du Dashboard...]
# Variables : DASH_NGPU_SHADERS (défaut <racine>/work/shaders_out), DASH_NGPU_DISABLE=1 (+ --gpu_plugin=xenos) pour la
# référence d'émulation, DASH_NGPU_FPSLOG=1 (cadence), DASH_NGPU_VERBOSE/FRAMELOG/DRAWLOG/TEXLOG/EVLOG (journaux),
# DASH_D3D_TRACE=<fichier> (traçage des appels D3D, voir native_gpu/src/d3d_trace.cpp).
D=$1; T=$2; N=$3; shift 3
P=$(cd "$(dirname "$0")/../.." && pwd); V=$P/dashboards/2.0.6770.0
FB=$V/runs/$N.fb; mkdir -p "$FB"
Xvfb :$D -screen 0 1280x720x24 -fbdir "$FB" >/dev/null 2>&1 & XP=$!
sleep 1
env MESA_VK_WSI_PRESENT_MODE=immediate VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json DISPLAY=:$D DASH_AUDIO=nop \
    DASH_NGPU_SHADERS=${DASH_NGPU_SHADERS:-$P/work/shaders_out} LD_LIBRARY_PATH=$P/sdk/lib \
    "$V/out/build/linux-amd64-relwithdebinfo/dash" --game_data_root="$V/assets" "$@" > "$V/runs/$N.out" 2>&1 & DP=$!
i=0; while [ $i -lt "$T" ] && kill -0 $DP 2>/dev/null; do sleep 1; i=$((i+1)); done
python3 -I "$P/tools/xwd2png.py" "$FB/Xvfb_screen0" "$V/runs/$N.png"
if kill -0 $DP 2>/dev/null; then echo "vivant après ${T}s"; kill -9 $DP; else wait $DP; echo "terminé après ${i}s code $?"; fi
kill $XP 2>/dev/null; rm -rf "$FB"
