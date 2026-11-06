#!/bin/sh
# Test d'un AppImage : HOME temporaire (work/apphome-<v>), import depuis firmware/, Dashboard sous Xvfb + lavapipe,
# capture dans dashboards/<v>/runs/preuves/p6_<v>.png.  usage: tools/test_appimage.sh <version> <xebuild-dir> [secondes]
set -u
V=$1; XB=$2; T=${3:-60}
R=$(cd "$(dirname "$0")/.." && pwd)
AI=$R/out/appimage/XenonDash-$V-x86_64.AppImage
H=$R/work/apphome-$V; rm -rf "$H"; mkdir -p "$H" "$R/dashboards/$V/runs/preuves"
mkdir -p $H/tmp; export TMPDIR=$H/tmp HOME=$H XDG_DATA_HOME= APPIMAGE_EXTRACT_AND_RUN=1
unset XDG_DATA_HOME
FB=$H/fb; mkdir -p $FB
DN=90; while [ -e /tmp/.X11-unix/X$DN ] || [ -e /tmp/.X$DN-lock ]; do DN=$((DN+1)); done
Xvfb :$DN -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1
export DISPLAY=:$DN MESA_VK_WSI_PRESENT_MODE=immediate VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} DASH_AUDIO=nop
"$AI" --import "$R/firmware/$V" --modules "$R/firmware/$XB" --shrdres-xam "$R/firmware/17559/\$flash_xam.xex" > $H/run.out 2>&1 & DP=$!
i=0; while [ $i -lt $T ] && kill -0 $DP 2>/dev/null; do sleep 1; i=$((i+1)); done
python3 -I $R/tools/xwd2png.py $FB/Xvfb_screen0 $R/dashboards/$V/runs/preuves/p6_$V.png
if kill -0 $DP 2>/dev/null; then echo "dash vivant après ${T}s"; pkill -9 -P $DP; kill -9 $DP; else wait $DP; echo "dash terminé code $?"; fi
kill $XP 2>/dev/null
tail -5 $H/run.out; grep -rc -E "FATAL|\[critical\]" --include=*.log $H | head -3
