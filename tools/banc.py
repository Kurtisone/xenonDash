#!/usr/bin/env python3
"""Banc visuel du Dashboard 13604 (variante -xam) sous Xvfb + lavapipe, piloté pas à pas.

Lance le Dashboard comme l'AppImage (rendu natif, XAM recompilé en mode 2, parcours profil XAM), sur un écran
Xvfb dédié, avec une COPIE du dossier de données : les profils et le journal de l'utilisateur ne sont pas touchés.
Chaque exécution a son dossier (captures, journaux, PID) ; les processus ne sont arrêtés que par leur PID.

Sous-commandes :
  start <nom> [--data D] [--build B] [--display N] [--env K=V ...] [--arg --x=y ...] [--boot S]
        démarre Xvfb puis dash, attend S secondes (60 par défaut), donne le focus à la fenêtre, capture <nom>_0.png
  keys  <nom> <touche>... [--pause S]   envoie les touches (tools/xkeys.py), attend S s (3), capture la suivante
  shot  <nom> [étiquette]               capture immédiate
  wait  <nom> <motif> [--timeout S]     attend (S s au plus, 120 par défaut) une ligne NOUVELLE du journal qui contient le motif
  cpu   <nom> [--secs S]                charge par fil sur S s (10), en % d'un cœur, fils les plus chargés d'abord
  grep  <nom> <motif>                   nombre de lignes du journal (dash.log + stdout) qui contiennent le motif
  stop  <nom>                           arrête dash puis Xvfb (par PID), indique si dash était encore vivant
  status <nom>

Touches du Dashboard (common/src/dash_input.h) : A = Return ou space, B = Escape ou BackSpace, Start = p, Back = Tab,
LB/RB = 1/3, croix = flèches, Guide = g.
"""
import argparse
import json
import os
import re
import resource
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join(ROOT, "dashboards/2.0.13604.0-xam/out/build/linux-amd64-relwithdebinfo")
DEFAULT_RUNS = os.environ.get("XD_BANC_RUNS", os.path.join(ROOT, "work/banc"))
LVP = "/usr/share/vulkan/icd.d/lvp_icd.json"


def run_dir(name):
    return os.path.join(DEFAULT_RUNS, name)


def load_state(name):
    p = os.path.join(run_dir(name), "state.json")
    if not os.path.exists(p):
        sys.exit(f"exécution inconnue : {name} ({p})")
    with open(p) as f:
        return json.load(f)


def save_state(name, st):
    with open(os.path.join(run_dir(name), "state.json"), "w") as f:
        json.dump(st, f, indent=1)


def alive(pid):
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    try:  # zombie = mort
        with open(f"/proc/{pid}/stat") as f:
            return f.read().split(")")[-1].split()[0] != "Z"
    except OSError:
        return False


def is_ours(pid, marker):
    """Vrai si le processus pid est bien celui qu'on a lancé (sa ligne de commande contient le marqueur)."""
    try:
        with open(f"/proc/{pid}/cmdline", "rb") as f:
            return marker.encode() in f.read()
    except OSError:
        return False


def capture(st, label=None):
    i = st.get("shots", 0)
    out = os.path.join(st["dir"], f"{st['name']}_{label if label else i}.png")
    fb = os.path.join(st["dir"], "fb", "Xvfb_screen0")
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools/xwd2png.py"), fb, out], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"capture impossible : {r.stderr.strip()}")
        return None
    if not label:
        st["shots"] = i + 1
    print(f"capture : {out}")
    return out


def display_env(st):
    e = dict(os.environ)
    e["DISPLAY"] = f":{st['display']}"
    return e


def focus(st):
    e = display_env(st)
    r = subprocess.run(["xdotool", "search", "--onlyvisible", "."], capture_output=True, text=True, env=e, timeout=10)
    wins = r.stdout.split()
    if not wins:
        print("focus : aucune fenêtre visible")
        return False
    subprocess.run(["xdotool", "windowfocus", wins[-1]], env=e, timeout=10)
    return True


def log_lines(st):
    n = 0
    for p in (st["log"], st["out"]):
        try:
            with open(p, "rb") as f:
                n += sum(1 for _ in f)
        except OSError:
            pass
    return n


def cmd_start(a):
    d = run_dir(a.name)
    if os.path.exists(os.path.join(d, "state.json")):
        st = json.load(open(os.path.join(d, "state.json")))
        if alive(st.get("dash_pid", -1)) or alive(st.get("xvfb_pid", -1)):
            sys.exit(f"{a.name} tourne encore : banc.py stop {a.name}")
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "fb"))
    data = os.path.abspath(a.data)
    build = os.path.abspath(a.build)
    dash = os.path.join(build, "dash")
    for p in (data, dash):
        if not os.path.exists(p):
            sys.exit(f"absent : {p}")
    if os.path.exists(f"/tmp/.X11-unix/X{a.display}") or os.path.exists(f"/tmp/.X{a.display}-lock"):
        sys.exit(f"l'écran :{a.display} est déjà pris (choisir --display)")
    xvfb = subprocess.Popen(["Xvfb", f":{a.display}", "-screen", "0", "1280x720x24", "-fbdir", os.path.join(d, "fb"), "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    for _ in range(50):
        if os.path.exists(os.path.join(d, "fb", "Xvfb_screen0")):
            break
        time.sleep(0.1)
    env = dict(os.environ)
    env.update({
        "DISPLAY": f":{a.display}",
        "VK_ICD_FILENAMES": LVP,
        "DASH_AUDIO": "nop",
        "LD_LIBRARY_PATH": os.path.join(ROOT, "sdk/lib"),
        # Comme tools/appimage/AppRun (variante XAM) :
        "DASH_XAM_MODULE": "2",
        "DASH_PROFILE_MODE": "xam",
        "XENONDASH_ROOT": ROOT,
        "XENONDASH_AUTOSCAN": "1",
        "XENONDASH_GAMES_DIR": os.path.expanduser(os.environ.get("XENONDASH_GAMES_DIR", "~/XenonDash/Games")),
        "XKEYS_HOLD": "0.12",
    })
    for kv in a.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    log = os.path.join(d, "dash.log")
    out = os.path.join(d, "out.log")
    argv = [dash, f"--game_data_root={data}", f"--log_file={log}", "--log_flush_interval=1"] + (a.arg or [])

    def child_setup():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    with open(out, "wb") as fo:
        p = subprocess.Popen(argv, cwd=data, env=env, stdout=fo, stderr=subprocess.STDOUT, preexec_fn=child_setup, start_new_session=True)
    st = {"name": a.name, "dir": d, "display": a.display, "dash_pid": p.pid, "xvfb_pid": xvfb.pid, "log": log, "out": out,
          "data": data, "build": build, "env": a.env or [], "args": a.arg or [], "t0": time.time(), "shots": 0}
    save_state(a.name, st)
    print(f"dash pid {p.pid}, Xvfb pid {xvfb.pid}, écran :{a.display}, journal {log}")
    t_end = time.time() + a.boot
    while time.time() < t_end:
        if not alive(p.pid):
            print(f"dash a quitté pendant le démarrage (code {p.poll()})")
            break
        time.sleep(1)
    if alive(p.pid):
        focus(st)
        time.sleep(3)
    capture(st)
    save_state(a.name, st)


def cmd_keys(a):
    st = load_state(a.name)
    if not alive(st["dash_pid"]):
        print("attention : dash ne tourne plus")
    env = display_env(st)  # xkeys.py donne lui-même le focus à la fenêtre du haut
    env["XKEYS_HOLD"] = env.get("XKEYS_HOLD", "0.12")
    subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools/xkeys.py")] + a.keys, env=env, timeout=120)
    time.sleep(a.pause)
    capture(st)
    save_state(a.name, st)


def cmd_shot(a):
    st = load_state(a.name)
    capture(st, a.label)
    save_state(a.name, st)


def cmd_wait(a):
    st = load_state(a.name)
    rx = re.compile(a.pattern)
    seen = {}
    for p in (st["log"], st["out"]):
        try:
            seen[p] = os.path.getsize(p)
        except OSError:
            seen[p] = 0
    t_end = time.time() + a.timeout
    while time.time() < t_end:
        for p in (st["log"], st["out"]):
            try:
                with open(p, "rb") as f:
                    f.seek(seen[p])
                    chunk = f.read()
                    seen[p] += len(chunk)
            except OSError:
                continue
            for line in chunk.decode("utf-8", "replace").splitlines():
                if rx.search(line):
                    print(f"trouvé après {a.timeout - (t_end - time.time()):.0f} s : {line[:200]}")
                    return
        if not alive(st["dash_pid"]):
            print("dash a quitté")
            return
        time.sleep(1)
    print(f"motif absent après {a.timeout} s")


def thread_ticks(pid):
    res = {}
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                s = f.read()
            comm = s[s.index("(") + 1:s.rindex(")")]
            fields = s[s.rindex(")") + 2:].split()
            res[tid] = (comm, int(fields[11]) + int(fields[12]))  # utime + stime
        except (OSError, ValueError):
            pass
    return res


def cmd_cpu(a):
    st = load_state(a.name)
    pid = st["dash_pid"]
    if not alive(pid):
        sys.exit("dash ne tourne plus")
    hz = os.sysconf("SC_CLK_TCK")
    t0 = thread_ticks(pid)
    time.sleep(a.secs)
    t1 = thread_ticks(pid)
    rows = []
    for tid, (comm, ticks) in t1.items():
        d = ticks - t0.get(tid, (comm, ticks))[1]
        rows.append((100.0 * d / hz / a.secs, tid, comm))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print(f"charge totale {total:.0f} % d'un cœur sur {a.secs} s ({len(rows)} fils) ; les plus chargés :")
    for pct, tid, comm in rows[:a.top]:
        print(f"  {pct:6.1f} %  {tid:>8}  {comm}")


def cmd_grep(a):
    st = load_state(a.name)
    n = 0
    rx = re.compile(a.pattern)
    for p in (st["log"], st["out"]):
        try:
            with open(p, "rb") as f:
                for line in f:
                    if rx.search(line.decode("utf-8", "replace")):
                        n += 1
                        if n <= a.show:
                            print(line.decode("utf-8", "replace").rstrip()[:220])
        except OSError:
            pass
    print(f"{n} ligne(s) pour « {a.pattern} »")


def stop_pid(pid, marker, sig=signal.SIGKILL):
    if alive(pid) and is_ours(pid, marker):
        os.kill(pid, sig)
        for _ in range(50):
            if not alive(pid):
                return True
            time.sleep(0.1)
    return not alive(pid)


def cmd_stop(a):
    st = load_state(a.name)
    was = alive(st["dash_pid"])
    stop_pid(st["dash_pid"], st["log"])
    stop_pid(st["xvfb_pid"], f":{st['display']}", signal.SIGTERM)
    print(f"dash {'vivant' if was else 'déjà arrêté'} au moment de l'arrêt ; {log_lines(st)} lignes de journal ; dossier {st['dir']}")


def cmd_status(a):
    st = load_state(a.name)
    print(f"dash {st['dash_pid']} {'vivant' if alive(st['dash_pid']) else 'arrêté'} ; Xvfb {st['xvfb_pid']} "
          f"{'vivant' if alive(st['xvfb_pid']) else 'arrêté'} ; {time.time() - st['t0']:.0f} s ; {st.get('shots', 0)} captures")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("start")
    p.add_argument("name")
    p.add_argument("--data", required=True)
    p.add_argument("--build", default=DEFAULT_BUILD)
    p.add_argument("--display", type=int, default=96)
    p.add_argument("--env", action="append")
    p.add_argument("--arg", action="append")
    p.add_argument("--boot", type=int, default=60)
    p.set_defaults(fn=cmd_start)
    p = sp.add_parser("keys")
    p.add_argument("name")
    p.add_argument("keys", nargs="+")
    p.add_argument("--pause", type=float, default=3.0)
    p.set_defaults(fn=cmd_keys)
    p = sp.add_parser("shot")
    p.add_argument("name")
    p.add_argument("label", nargs="?")
    p.set_defaults(fn=cmd_shot)
    p = sp.add_parser("wait")
    p.add_argument("name")
    p.add_argument("pattern")
    p.add_argument("--timeout", type=int, default=120)
    p.set_defaults(fn=cmd_wait)
    p = sp.add_parser("cpu")
    p.add_argument("name")
    p.add_argument("--secs", type=float, default=10.0)
    p.add_argument("--top", type=int, default=8)
    p.set_defaults(fn=cmd_cpu)
    p = sp.add_parser("grep")
    p.add_argument("name")
    p.add_argument("pattern")
    p.add_argument("--show", type=int, default=5)
    p.set_defaults(fn=cmd_grep)
    p = sp.add_parser("stop")
    p.add_argument("name")
    p.set_defaults(fn=cmd_stop)
    p = sp.add_parser("status")
    p.add_argument("name")
    p.set_defaults(fn=cmd_status)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
