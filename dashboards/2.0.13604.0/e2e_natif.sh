#!/bin/sh
# Test de bout en bout de l'AppImage native 2.0.13604.0 (docs/ngpu_core.md, §8 et « Génération à l'import ») :
#   1. reconstruction de l'AppImage (tools/make_appimage.sh 2.0.13604.0) ;
#   2. HOME et TMPDIR de test sous E2E_DIR (défaut : $TMPDIR/xenondash-e2e-natif) ; aucune variable DASH_NGPU_SHADERS / DASH_HUD_SHADERS ;
#   3. premier lancement : import de firmware/2.0.13604.0 (+ modules XeBuilds, xam 17559 pour shrdres) puis Dashboard sous Xvfb + lavapipe ;
#   4. contrôles : aucune ligne « ERREUR SPIR-V » / « introuvable », aucun --gpu_plugin=xenos ; capture PNG ; durées ; tailles ; nombre de shaders.
#   5. régénération seule (--regen-shaders), chronométrée.
# usage: dashboards/2.0.13604.0/e2e_natif.sh [secondes_de_jeu] [numéro_d_affichage]   (défaut : 90 s, :92)
# Ne tue que les processus lancés par ce script (par PID). Le dossier E2E_DIR n'est jamais le vrai ~/.local/share/xenondash.
set -u
ulimit -c 0
V=2.0.13604.0; XB=xebuild-6770; T=${1:-90}; DN=${2:-92}
R=$(cd "$(dirname "$0")/../.." && pwd)
W=${E2E_DIR:-${TMPDIR:-/tmp}/xenondash-e2e-natif}
AI=$R/out/appimage/XenonDash-$V-x86_64.AppImage
H=$W/home; FB=$W/fb; P=$W/preuves
rm -rf "$H" "$FB" "$P"; mkdir -p "$H/tmp" "$FB" "$P"
LOG=$W/e2e.log
log() { echo "[e2e] $*" | tee -a "$LOG"; }
: > "$LOG"

[ -x "$R/tools/make_appimage.sh" ] || { echo "dépôt introuvable" >&2; exit 1; }
t0=$(date +%s)
"$R/tools/make_appimage.sh" $V > "$W/make_appimage.log" 2>&1 || { log "make_appimage en échec (voir $W/make_appimage.log)"; exit 1; }
log "AppImage reconstruite en $(( $(date +%s) - t0 )) s : $(ls -l "$AI" | awk '{print $5}') octets"
grep -E 'build embarqué|rendu' "$W/make_appimage.log" | tee -a "$LOG"

if [ -e /tmp/.X11-unix/X$DN ] || [ -e /tmp/.X$DN-lock ]; then log "affichage :$DN déjà utilisé : choisir un autre numéro"; exit 1; fi
export HOME=$H XDG_DATA_HOME= TMPDIR=$H/tmp APPIMAGE_EXTRACT_AND_RUN=1
export DISPLAY=:$DN MESA_VK_WSI_PRESENT_MODE=immediate VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-/usr/share/vulkan/icd.d/lvp_icd.json} DASH_AUDIO=nop
unset DASH_NGPU_SHADERS DASH_HUD_SHADERS DASH_NGPU_DISABLE DASH_NGPU_MISSING
Xvfb :$DN -screen 0 1280x720x24 -fbdir $FB >/dev/null 2>&1 & XP=$!
sleep 1

DATA=$H/.local/share/xenondash/$V
t1=$(date +%s)
"$AI" --import "$R/firmware/$V" --modules "$R/firmware/$XB" --shrdres-xam "$R/firmware/17559/\$flash_xam.xex" > "$W/run.out" 2>&1 & DP=$!
# Premier lancement : l'import se termine (ligne « import terminé » et génération des shaders), puis le Dashboard démarre
i=0; while [ $i -lt 900 ] && kill -0 $DP 2>/dev/null && ! grep -q -E 'hud : .*shaders dans|ÉCHEC hud|non générés' "$W/run.out"; do sleep 2; i=$((i+2)); done
log "import + génération des shaders : $(( $(date +%s) - t1 )) s"
i=0; while [ $i -lt "$T" ] && kill -0 $DP 2>/dev/null; do sleep 1; i=$((i+1)); done
python3 -I "$R/tools/xwd2png.py" "$FB/Xvfb_screen0" "$P/hub_$V.png" 2>>"$LOG" && log "capture : $P/hub_$V.png"
if kill -0 $DP 2>/dev/null; then
  log "Dashboard vivant après ${T}s de jeu"
  pkill -9 -P $DP 2>/dev/null; kill -9 $DP 2>/dev/null
else
  wait $DP; log "Dashboard terminé, code $?"
fi
kill $XP 2>/dev/null; wait $XP 2>/dev/null

# Journaux et contrôles
cp "$DATA/dash.log" "$P/dash.log" 2>/dev/null
grep -E '\[shaders\]' "$W/run.out" | grep -v 'traduction ' | tee -a "$LOG" >/dev/null
log "lignes « ERREUR SPIR-V » : $(grep -c 'ERREUR SPIR-V' "$W/run.out" "$DATA/dash.log" 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')"
log "lignes « introuvable » (shaders natifs) : $(cat "$W/run.out" "$DATA/dash.log" 2>/dev/null | grep -c 'introuvable')"
log "--gpu_plugin=xenos (journal, sortie) : $(cat "$W/run.out" "$DATA/dash.log" 2>/dev/null | grep -c -- '--gpu_plugin=xenos')"
log "erreurs [ngpu] : $(cat "$W/run.out" "$DATA/dash.log" 2>/dev/null | grep -c '\[ngpu\] ERREUR')"
log "shaders Dashboard : $(ls "$DATA/shaders/dash" 2>/dev/null | grep -c '\.spv$') .spv ; échecs : $(wc -l < "$DATA/shaders/dash-echecs.txt" 2>/dev/null)"
log "shaders HUD : $(ls "$DATA/shaders/hud" 2>/dev/null | grep -c '\.spv$') .spv ; échecs : $(wc -l < "$DATA/shaders/hud-echecs.txt" 2>/dev/null)"

# Régénération seule, chronométrée
t2=$(date +%s)
"$AI" --regen-shaders > "$P/regen.out" 2>&1; log "--regen-shaders : code $?, $(( $(date +%s) - t2 )) s"
log "fin : $P"
