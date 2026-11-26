#!/usr/bin/env python3
"""Suite de non-régression du banc (Dashboard Kinect 2.0.13604, variante -xam) : un parcours fixe, des captures comparées
à des références gardées hors du dépôt, des assertions sur le journal et la charge par fil, un verdict OK ou KO.

Une seule exécution de dash (tools/banc.py : Xvfb + lavapipe, rendu natif) sur une copie fraîche d'un instantané des
données. Les touches de menu partent une par une avec une pause : envoyées en rafale, une partie se perd pendant les
transitions (Escape x4 n'avait reculé que de deux niveaux).

Points de contrôle (capture comparée à la référence) :
  xmb            démarrage -> canal Welcome
  myxbox         Up -> canal My Xbox (Open Tray, Sign In, Game Library...)
  gamelib        Right Right Return -> Game Library
  gamelib_back   Escape -> retour sur My Xbox
  settings       Right x6, Return, Return -> Console Settings (réglage courant 1280 x 720, Widescreen, VGA)
  resolutions    Return Return -> Screen Resolution (640 x 480 à 1680 x 1050)
  guide          Escape x4, g -> Guide (Sign In (1 Profile), Create Profile...)
  signin         Down Return -> Sign In (profil de l'instantané, HDD)
  signed_in      Return -> Guide du profil connecté
Assertions : dash vivant à la fin ; aucun « *** STOP » (KeBugCheck, fatal) ; aucune ligne « [fibre] » ; présence de
« [vd] découverte de l'affichage » ; Guide fermé et 15 s de repos, aucun fil (hors llvmpipe) au-dessus de 60 % d'un
cœur sur 10 s (régression des fibres de XAM : un fil à 99 %).

Sous-commandes :
  init  [--data D] [--games G]  instantané des données de départ : copie de D (dossier de l'AppImage par défaut) sans
                                dash.log, dans <refs>/donnees ; liens vers les jeux présents dans G (~/XenonDash/Games)
                                dans <refs>/jeux (la Game Library de la suite ne change pas quand un jeu est ajouté) et
                                bibliothèque scannée sur ces liens dans <refs>/bibliotheque (copiée à chaque exécution)
  run   [--name N] [--build B] [--display N] [--boot S] [--env K=V ...] [--only a,b]
                                exécute la suite ; code de sortie 0 = OK, 1 = KO, 2 = erreur. --env passe une variable à
                                dash (ex. DASH_XAM_FIBER_FIX=0 pour vérifier que la suite voit la régression des fibres)
  refs  <exécution> [--calib <exécution2>]   les captures de <exécution> deviennent les références (les regarder
                                avant !) ; avec --calib, tolérances par point d'après l'écart entre deux exécutions
                                correctes (3 fois l'écart mesuré, avec un plancher)

Références et instantané : <copie principale>/work/banc/refs/2.0.13604.0-xam (ou XD_BANC_REFS), hors du dépôt : ils
contiennent des images et des données du Dashboard. Exécutions : dossier de tools/banc.py (XD_BANC_RUNS).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import banc  # noqa: E402
import framecmp  # noqa: E402

VARIANT = "2.0.13604.0-xam"
DEFAULT_DATA = os.path.expanduser(f"~/.local/share/xenondash/{VARIANT}")

# Parcours : (touches envoyées ensemble, pause après, point de contrôle ou None). Les flèches du ruban de tuiles
# supportent la rafale ; Return, Escape et g partent seuls.
STEPS = [
    ([], 0, "xmb"),
    (["Up"], 3, "myxbox"),
    (["Right", "Right"], 3, None),
    (["Return"], 8, "gamelib"),
    (["Escape"], 5, "gamelib_back"),
    (["Right"] * 6, 3, None),  # Game Library -> System Settings (sans l'emplacement Mediaroom, masqué depuis le 2026-11-26)
    (["Return"], 5, None),
    (["Return"], 5, "settings"),
    (["Return"], 5, None),
    (["Return"], 5, "resolutions"),
    (["Escape"], 3, None),
    (["Escape"], 3, None),
    (["Escape"], 3, None),
    (["Escape"], 4, None),
    (["g"], 6, "guide"),
    (["Down"], 2, None),
    (["Return"], 8, "signin"),
    (["Return"], 12, "signed_in"),
    # Fermeture du Guide puis repos : la charge se mesure ensuite, hors du travail passager de la connexion (fil principal
    # à 91 % juste après « signed_in »).
    (["Escape"], 15, None),
]
CHECKPOINTS = [s[2] for s in STEPS if s[2]]

# Tolérances par défaut (sans calibration) : écart moyen global (0-255), part des pixels à écart > 32, écart moyen de
# la pire zone (grille 8x6). Le rendu (lavapipe) est presque déterministe : entre deux exécutions, seuls diffèrent
# l'horloge du Guide et le logo Xbox animé (tous deux masqués) et la pulsation de l'élément choisi (écart faible, aucun
# pixel > 32). Le pointeur de la souris n'est plus dessiné (Xvfb -nocursor, tools/banc.py) : il apparaissait ou non au
# centre de l'écran selon l'instant. Planchers serrés pour qu'un texte changé (3 glyphes : ~110 pixels > 32, zone ~1,2)
# soit vu.
DEFAULT_TOL = {"ecart_moyen": 1.0, "part_gt32": 0.0005, "zone_max": 3.0}
FLOOR_TOL = {"ecart_moyen": 0.3, "part_gt32": 0.0001, "zone_max": 1.0}
# Zones ignorées (x, y, largeur, hauteur) : l'horloge du Guide (« 5:18 PM », pixels 845-868 x 176-193 changeants) et le
# logo Xbox animé en bas à droite (pixels 1127-1168 x 610-651 changeants, écart jusqu'à 30), présent sur tous les écrans.
CLOCK = (815, 172, 100, 26)
LOGO = (1118, 600, 60, 60)
MASKS = {cp: [LOGO] for cp in ("xmb", "myxbox", "gamelib", "gamelib_back", "settings", "resolutions")}
MASKS.update({cp: [LOGO, CLOCK] for cp in ("guide", "signin", "signed_in")})
DEFAULT_GAMES = os.path.expanduser(os.environ.get("XENONDASH_GAMES_DIR", "~/XenonDash/Games"))

LOG_FORBIDDEN = [(r"\*\*\* STOP", "arrêt KeBugCheck"), (r"\[fibre\]", "avertissement des fibres de XAM")]
LOG_REQUIRED = [(r"\[vd\] découverte de l'affichage", "découverte de l'affichage (liste des résolutions)")]
# Lignes [error] admises (expressions régulières). Toute autre ligne [error] du journal fait échouer la suite : une erreur
# est un défaut à corriger, ou un cas normal à journaliser plus bas, pas un bruit de fond. Liste vide depuis le 2026-11-26
# (150 lignes avant : périphériques absents sondés par XAM, modules optionnels absents, service Live hors ligne, appel
# indirect nul de l'Async Loader).
ERROR_ALLOWED = []
CPU_SECS = 10.0
CPU_MAX = 60.0


def main_checkout():
    root = banc.ROOT
    try:
        common = subprocess.run(["git", "-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, check=True).stdout.strip()
        return os.path.dirname(common)
    except (OSError, subprocess.CalledProcessError):
        return root


def refs_dir():
    return os.environ.get("XD_BANC_REFS") or os.path.join(main_checkout(), "work/banc/refs", VARIANT)


def load_tol():
    p = os.path.join(refs_dir(), "tolerances.json")
    if os.path.exists(p):
        with open(p) as f:
            return json.load(f)
    return {}


def cmd_init(a):
    src = os.path.abspath(a.data)
    dst = os.path.join(refs_dir(), "donnees")
    if not os.path.isdir(src):
        sys.exit(f"absent : {src}")
    if os.path.exists(dst):
        shutil.rmtree(dst)
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns("dash.log", "dash.log.*"), symlinks=True)
    print(f"instantané : {src} -> {dst}")
    # Jeux : liens vers les entrées présentes aujourd'hui (pas de copie : plusieurs Go). Un jeu ajouté plus tard ne
    # change pas la Game Library de la suite.
    games = os.path.abspath(a.games)
    gdst = os.path.join(refs_dir(), "jeux")
    if os.path.exists(gdst):
        shutil.rmtree(gdst)
    os.makedirs(gdst)
    if os.path.isdir(games):
        for e in sorted(os.listdir(games)):
            os.symlink(os.path.join(games, e), os.path.join(gdst, e))
    print(f"jeux : {len(os.listdir(gdst))} entrée(s) de {games} -> {gdst}")
    # Bibliothèque scannée sur ces liens : banc.py en donne une copie à chaque exécution, dont le scan du démarrage reste
    # incrémental (mêmes chemins) ; celle de l'utilisateur n'est jamais écrite.
    lib = os.path.join(refs_dir(), "bibliotheque")
    if os.path.exists(lib):
        shutil.rmtree(lib)
    subprocess.run([sys.executable, "-I", os.path.join(banc.ROOT, "tools/game_library.py"), "scan", "--out", lib],
                   env=dict(os.environ, XENONDASH_GAMES_DIR=gdst), check=True, stdout=subprocess.DEVNULL, timeout=600)
    print(f"bibliothèque : {lib}")


def masked(img, rects):
    if not rects:
        return img
    w, h, d = img
    b = bytearray(d)
    for x, y, rw, rh in rects:
        for yy in range(y, min(h, y + rh)):
            b[(yy * w + x) * 3:(yy * w + min(w, x + rw)) * 3] = bytes((min(w, x + rw) - x) * 3)
    return w, h, bytes(b)


def compare_masked(cp, ref, cur):
    m = MASKS.get(cp)
    return framecmp.compare(masked(framecmp.load(ref), m), masked(framecmp.load(cur), m))


def send(st, keys):
    env = banc.display_env(st)
    env["XKEYS_HOLD"] = env.get("XKEYS_HOLD", "0.12")
    subprocess.run([sys.executable, "-I", os.path.join(banc.ROOT, "tools/xkeys.py")] + keys, env=env, timeout=120,
                   stdout=subprocess.DEVNULL)


def scan_log(st):
    text = ""
    for p in (st["log"], st["out"]):
        try:
            with open(p, "rb") as f:
                text += f.read().decode("utf-8", "replace")
        except OSError:
            pass
    return text


def thread_loads(pid):
    """Charge par fil (% d'un cœur sur CPU_SECS), fils llvmpipe exclus, les plus chargés d'abord."""
    hz = os.sysconf("SC_CLK_TCK")
    t0 = banc.thread_ticks(pid)
    time.sleep(CPU_SECS)
    t1 = banc.thread_ticks(pid)
    rows = []
    for tid, (comm, ticks) in t1.items():
        if comm.startswith("llvmpipe"):
            continue
        rows.append((round(100.0 * (ticks - t0.get(tid, (comm, ticks))[1]) / hz / CPU_SECS, 1), comm))
    return sorted(rows, reverse=True) or [(0.0, "")]


def judge(cp, ref, cur, tol, outdir, name):
    """Compare une capture à sa référence (zones de MASKS ignorées) ; écrit l'image des écarts si le point échoue."""
    r, diff = compare_masked(cp, ref, cur)
    zmax = max(z["ecart_moyen"] for z in r["zones"])
    t = dict(DEFAULT_TOL)
    t.update(tol.get(cp, {}))
    fails = []
    if r["ecart_moyen"] > t["ecart_moyen"]:
        fails.append(f"écart moyen {r['ecart_moyen']:.2f} > {t['ecart_moyen']:.2f}")
    if r["part_pixels_gt32"] > t["part_gt32"]:
        fails.append(f"pixels > 32 : {100 * r['part_pixels_gt32']:.2f} % > {100 * t['part_gt32']:.2f} %")
    if zmax > t["zone_max"]:
        fails.append(f"pire zone {zmax:.1f} > {t['zone_max']:.1f}")
    if fails:
        w, h, d = diff
        dp = os.path.join(outdir, f"{name}_{cp}_ecarts.png")
        framecmp.write_png(dp, w, h, d)
        framecmp.montage([ref, cur, dp], os.path.join(outdir, f"{name}_{cp}_ref-capture-ecarts.png"), scale=2, cols=3)
    return {"point": cp, "ecart_moyen": r["ecart_moyen"], "part_gt32": r["part_pixels_gt32"], "zone_max": round(zmax, 2),
            "echecs": fails}


def cmd_run(a):
    rd = refs_dir()
    snap = os.path.join(rd, "donnees")
    if not os.path.isdir(snap):
        print(f"pas d'instantané des données ({snap}) : banc_suite.py init")
        return 2
    only = set(a.only.split(",")) if a.only else None
    data = os.path.join(banc.DEFAULT_RUNS, f"{a.name}.donnees")
    if os.path.exists(data):
        shutil.rmtree(data)
    os.makedirs(banc.DEFAULT_RUNS, exist_ok=True)
    shutil.copytree(snap, data, symlinks=True)
    env = list(a.env)
    if os.path.isdir(os.path.join(rd, "jeux")):
        env.insert(0, f"XENONDASH_GAMES_DIR={os.path.join(rd, 'jeux')}")
    lib = os.path.join(rd, "bibliotheque")
    t0 = time.time()
    banc.cmd_start(argparse.Namespace(name=a.name, data=data, build=a.build, display=a.display, env=env, arg=[],
                                      boot=a.boot, library=lib if os.path.isdir(lib) else None))
    st = banc.load_state(a.name)
    shots = {}
    for keys, pause, cp in STEPS:
        if not banc.alive(st["dash_pid"]):
            break
        if keys:
            send(st, keys)
            time.sleep(pause)
        if cp:
            out = banc.capture(st, cp)
            if out:
                shots[cp] = out
    alive_end = banc.alive(st["dash_pid"])
    loads = thread_loads(st["dash_pid"]) if alive_end else [(0.0, "")]
    load = loads[0]
    banc.save_state(a.name, st)
    banc.cmd_stop(argparse.Namespace(name=a.name))
    shutil.rmtree(data, ignore_errors=True)

    problems = []
    if not alive_end:
        problems.append("dash ne tourne plus à la fin du parcours")
    text = scan_log(st)
    for rx, what in LOG_FORBIDDEN:
        hits = [ln for ln in text.splitlines() if re.search(rx, ln)]
        if hits:
            problems.append(f"journal : {len(hits)} ligne(s) « {what} », ex. {hits[0][:160]}")
    for rx, what in LOG_REQUIRED:
        if not re.search(rx, text):
            problems.append(f"journal : absent : {what}")
    if load[0] > CPU_MAX:
        problems.append(f"charge : fil {load[1]} à {load[0]:.0f} % d'un cœur (> {CPU_MAX:.0f} %)")
    errors = [ln for ln in text.splitlines() if "[error]" in ln]
    n_err = len(errors)
    unexpected = [ln for ln in errors if not any(re.search(rx, ln) for rx in ERROR_ALLOWED)]
    if unexpected:
        problems.append(f"journal : {len(unexpected)} ligne(s) [error] hors liste, ex. {unexpected[0][:160]}")

    tol = load_tol()
    rows = []
    for cp in CHECKPOINTS:
        if only and cp not in only:
            continue
        ref = os.path.join(rd, f"{cp}.png")
        if cp not in shots:
            rows.append({"point": cp, "echecs": ["pas de capture"]})
        elif not os.path.exists(ref):
            rows.append({"point": cp, "echecs": ["pas de référence (banc_suite.py refs)"]})
        else:
            rows.append(judge(cp, ref, shots[cp], tol, st["dir"], a.name))

    print(f"\nSuite {a.name} : {time.time() - t0:.0f} s, dossier {st['dir']}")
    for r in rows:
        mark = "KO" if r["echecs"] else "ok"
        metrics = (f"écart {r['ecart_moyen']:5.2f}  >32 {100 * r['part_gt32']:5.2f} %  zone {r['zone_max']:5.1f}"
                   if "ecart_moyen" in r else "")
        print(f"  {mark}  {r['point']:<13} {metrics}  {'; '.join(r['echecs'])}")
    top = ", ".join(f"{c} {p:.0f} %" for p, c in loads[:3])
    print(f"  charge sur {CPU_SECS:.0f} s, fils les plus chargés (hors llvmpipe) : {top} ; lignes [error] : {n_err}")
    for p in problems:
        print(f"  KO  {p}")
    ko = bool(problems) or any(r["echecs"] for r in rows)
    verdict = {"nom": a.name, "verdict": "KO" if ko else "OK", "points": rows, "problemes": problems,
               "charge": loads[:8], "lignes_error": n_err, "duree_s": round(time.time() - t0)}
    with open(os.path.join(st["dir"], "verdict.json"), "w") as f:
        json.dump(verdict, f, indent=1, ensure_ascii=False)
    print(f"VERDICT : {verdict['verdict']}")
    return 1 if ko else 0


def cmd_refs(a):
    rd = refs_dir()
    os.makedirs(rd, exist_ok=True)
    src = banc.run_dir(a.run)
    missing = [cp for cp in CHECKPOINTS if not os.path.exists(os.path.join(src, f"{a.run}_{cp}.png"))]
    if missing:
        sys.exit(f"captures absentes dans {src} : {', '.join(missing)}")
    for cp in CHECKPOINTS:
        shutil.copyfile(os.path.join(src, f"{a.run}_{cp}.png"), os.path.join(rd, f"{cp}.png"))
    print(f"{len(CHECKPOINTS)} références : {rd}")
    if not a.calib:
        return 0
    other = banc.run_dir(a.calib)
    tol = {}
    for cp in CHECKPOINTS:
        r, _ = compare_masked(cp, os.path.join(src, f"{a.run}_{cp}.png"), os.path.join(other, f"{a.calib}_{cp}.png"))
        zmax = max(z["ecart_moyen"] for z in r["zones"])
        tol[cp] = {"ecart_moyen": round(max(FLOOR_TOL["ecart_moyen"], 3 * r["ecart_moyen"]), 2),
                   "part_gt32": round(max(FLOOR_TOL["part_gt32"], 3 * r["part_pixels_gt32"]), 4),
                   "zone_max": round(max(FLOOR_TOL["zone_max"], 3 * zmax), 1)}
        print(f"  {cp:<13} écart entre exécutions {r['ecart_moyen']:5.2f}, >32 {100 * r['part_pixels_gt32']:5.2f} %, "
              f"pire zone {zmax:5.1f} -> tolérance {tol[cp]}")
    with open(os.path.join(rd, "tolerances.json"), "w") as f:
        json.dump(tol, f, indent=1)
    print(f"tolérances : {os.path.join(rd, 'tolerances.json')}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("init")
    p.add_argument("--data", default=DEFAULT_DATA)
    p.add_argument("--games", default=DEFAULT_GAMES)
    p.set_defaults(fn=cmd_init)
    p = sp.add_parser("run")
    p.add_argument("--name", default="suite")
    p.add_argument("--build", default=banc.DEFAULT_BUILD)
    p.add_argument("--display", type=int, default=95)
    p.add_argument("--boot", type=int, default=75)
    p.add_argument("--env", action="append", default=[])
    p.add_argument("--only")
    p.set_defaults(fn=cmd_run)
    p = sp.add_parser("refs")
    p.add_argument("run")
    p.add_argument("--calib")
    p.set_defaults(fn=cmd_refs)
    a = ap.parse_args()
    sys.exit(a.fn(a) or 0)


if __name__ == "__main__":
    main()
