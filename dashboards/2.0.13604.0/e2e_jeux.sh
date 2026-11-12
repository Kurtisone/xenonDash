#!/bin/sh
# Test de bout en bout du Dashboard NXE 13604 en AppImage : dossier de jeux -> Game Library -> A -> recompilation avec
# progression -> jeu natif -> retour au Dashboard (docs/nxe_jeux.md). Xvfb + lavapipe, HOME temporaire, import du firmware.
#
# usage : E2E_GAME=<dossier ou ISO du jeu> [E2E_OUT=<captures>] [E2E_HOME=<HOME de test>] [E2E_DISPLAY=94] \
#         [E2E_CACHE_FROM=<cache xenondash/games à copier : test rapide sans recompilation>] [E2E_CORES=0-5] \
#         dashboards/2.0.13604.0/e2e_jeux.sh [xebuild-6770]
# Le HOME de test doit être sur un disque (la construction de Skate demande 40 Go libres annoncés, ~0,6 Go réels : XD_MIN_DISK_GB
# peut être abaissé). Ne touche ni ~/XenonDash ni ~/.local/share/xenondash.
set -u
XB=${1:-xebuild-6770}
R=$(cd "$(dirname "$0")/../.." && pwd)
V=2.0.13604.0
AI=$R/out/appimage/XenonDash-$V-x86_64.AppImage
GAME=${E2E_GAME:?E2E_GAME=<dossier du jeu>}
OUT=${E2E_OUT:-$R/work/e2e-$V}; mkdir -p "$OUT"
H=${E2E_HOME:-$R/work/e2e-home}; rm -rf "$H"; mkdir -p "$H/tmp" "$H/XenonDash/Games"
DN=${E2E_DISPLAY:-94}
CORES=${E2E_CORES:-0-5}
ln -s "$GAME" "$H/XenonDash/Games/$(basename "$GAME")"
if [ -n "${E2E_CACHE_FROM:-}" ]; then
  mkdir -p "$H/.local/share/xenondash"; cp -a "$E2E_CACHE_FROM" "$H/.local/share/xenondash/games"
fi
FB=$OUT/fb; mkdir -p "$FB"
shot() { python3 -I "$R/tools/xwd2png.py" "$FB/Xvfb_screen0" "$OUT/$1.png" >/dev/null 2>&1 && echo "capture $OUT/$1.png"; }
key() { DISPLAY=:$DN XKEYS_HOLD=${2:-0.15} python3 -I "$R/tools/xkeys.py" "$1" >/dev/null 2>&1; }
log() { echo "[e2e $(date +%H:%M:%S)] $*"; }
DATA=$H/.local/share/xenondash/$V
LOG=$DATA/dash.log

Xvfb :$DN -screen 0 1280x720x24 -fbdir "$FB" >/dev/null 2>&1 & XP=$!
sleep 1
export TMPDIR=$H/tmp HOME=$H XDG_DATA_HOME=$H/.local/share APPIMAGE_EXTRACT_AND_RUN=1 \
       DISPLAY=:$DN MESA_VK_WSI_PRESENT_MODE=immediate DASH_AUDIO=nop \
       VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json}
unset XENONDASH_GAMES_DIR XDG_CONFIG_HOME
log "lancement de l'AppImage (import du firmware au premier lancement)"
taskset -c "$CORES" "$AI" --import "$R/firmware/$V" --modules "$R/firmware/$XB" --shrdres-xam "$R/firmware/17559/\$flash_xam.xex" \
  > "$OUT/appimage.out" 2>&1 & AP=$!
wait_log() { # motif, délai max (s)
  t=0; until grep -a -q -E "$1" "$LOG" 2>/dev/null || [ $t -ge "$2" ]; do sleep 2; t=$((t+2)); kill -0 $AP 2>/dev/null || break; done
  grep -a -q -E "$1" "$LOG" 2>/dev/null
}
wait_log "bibliothèque : scan" 240 && log "scan de la bibliothèque : $(grep -a 'bibliothèque : scan' "$LOG" | tail -1 | cut -c60-200)" || log "pas de scan"
sleep 40; shot 01_dashboard
# Welcome -> My Xbox -> Game Library (le rendu sous lavapipe est lent : ~10 s par transition)
key Shift_L; sleep 2; key Up; sleep 14; key Up; sleep 14; key Right 0.08; sleep 16; shot 02_my_xbox
key Return 0.08; sleep 50; shot 03_game_library
key Return 0.08; sleep 20; shot 04_page_jeu
key Return 0.08; log "A sur Play Game"
sleep 5; shot 05_apres_play
t=0
while ! grep -a -q "LANCEMENT de" "$LOG" 2>/dev/null && [ $t -lt 3600 ]; do
  sleep 60; t=$((t+60))
  shot "06_progression_$(printf %03d $((t/60)))min"
  log "$(grep -a 'construction de' "$LOG" | tail -1 | cut -c40-230)"
done
log "lancement du jeu"; grep -a "LANCEMENT de" "$LOG" | tail -1 | cut -c30-200
sleep 45; shot 07_jeu
GP=$(for p in $(pgrep -x skate1); do ls -l /proc/$p/cwd 2>/dev/null | grep -q "$H" && echo $p; done | head -1)
log "jeu pid=${GP:-aucun}"
[ -n "$GP" ] && kill "$GP"
sleep 60; shot 08_retour_dashboard
log "retour : $(grep -a 'retour au Dashboard' "$LOG" | tail -1 | cut -c30-160)"
for p in $(pgrep -x dash); do ls -l /proc/$p/cwd 2>/dev/null | grep -q "$H" && kill "$p"; done
kill $XP 2>/dev/null
echo "FATAL/critical : $(grep -a -c -E "FATAL|\[critical\]" "$LOG")"
