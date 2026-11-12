#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bibliothèque de jeux de XenonDash : inventaire du dossier de jeux de l'utilisateur.

  game_library.py init [--dir D]                  crée le dossier de jeux (et LISEZMOI.txt) s'il manque
  game_library.py scan [--dir D] [--out O] [--force] [--json]
                                                  identifie les jeux, écrit games.auto.toml, icons/, library.json
  game_library.py list [--out O] [--json]         affiche le dernier inventaire

Dossier de jeux : $XENONDASH_GAMES_DIR, sinon ~/XenonDash/Games. Sortie : ${XDG_DATA_HOME:-~/.local/share}/xenondash/library/.
Aucun fichier de jeu n'est copié ni modifié : l'inventaire référence les sources par chemin absolu.
Formats : voir tools/xcontainer.py et docs/bibliotheque.md.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import xcontainer as xc  # noqa: E402

SCHEMA = 1
MAX_DEPTH = 8
ICON_LIMIT = 16 * 1024
README = """XenonDash : dossier de jeux

Deposez ici vos jeux Xbox 360, tels quels, dans n'importe quel sous-dossier :
  - un dossier de jeu extrait (contenant default.xex) ;
  - une image disque ISO (XDVDFS, XGD1/XGD2/XGD3) ;
  - un paquet STFS (LIVE / PIRS / CON : jeux Xbox Live Arcade, demos...) ;
  - un jeu Games on Demand (le fichier d'en-tete et son dossier <nom>.data) ;
  - un fichier default.xex seul ;
  - une arborescence de disque dur (<TitleID>/<type>/<paquet>).

Le format est reconnu par le contenu des fichiers, pas par leur extension. Les liens symboliques sont suivis.
Aucun fichier n'est copie ni modifie. Le Dashboard NXE (Games Library > My Games) voit les jeux au lancement
(l'inventaire est refait automatiquement ; seuls les fichiers nouveaux ou changes sont relus).

Les sauvegardes, contenus telechargeables (DLC), mises a jour de titre ou du systeme ne sont pas des jeux : ils
sont ignores et signales dans le rapport de l'inventaire.
"""

PREFERENCE = {"dir": 0, "xex": 1, "xdvdfs": 2, "god": 2, "stfs": 3}


def games_dir(arg=None):
    return os.path.abspath(os.path.expanduser(
        arg or os.environ.get("XENONDASH_GAMES_DIR") or "~/XenonDash/Games"))


def library_dir(arg=None):
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.abspath(os.path.expanduser(arg or os.path.join(base, "xenondash", "library")))


def init(d=None):
    d = games_dir(d)
    os.makedirs(d, exist_ok=True)
    readme = os.path.join(d, "LISEZMOI.txt")
    created = not os.path.exists(readme)
    if created:
        with open(readme, "w", encoding="utf-8") as f:
            f.write(README)
    return {"games_dir": d, "readme": readme, "readme_created": created}


# ---------------------------------------------------------------------------------------------
# Découverte
# ---------------------------------------------------------------------------------------------
def discover(root):
    """[(chemin, 'dir'|'file')] : dossiers de jeu (default.xex) et fichiers candidats."""
    out, seen = [], set()

    def walk(d, depth):
        rp = os.path.realpath(d)
        if rp in seen:
            return
        seen.add(rp)
        try:
            names = sorted(os.listdir(d))
        except OSError:
            return
        for n in names:
            p = os.path.join(d, n)
            if n.startswith("."):
                continue
            try:
                isdir = os.path.isdir(p)
            except OSError:
                continue
            if isdir:
                if xc._has_default_xex(p):
                    out.append((p, "dir"))
                elif n.lower().endswith(".data") and os.path.isfile(p[:-5]):
                    continue                       # données d'un jeu Games on Demand : lues via l'en-tête
                elif depth < MAX_DEPTH:
                    walk(p, depth + 1)
            elif n != "LISEZMOI.txt":
                out.append((p, "file"))

    if xc._has_default_xex(root):
        out.append((root, "dir"))
    else:
        walk(root, 0)
    return out


def stat_sig(paths):
    sig = []
    for p in paths:
        try:
            st = os.stat(p)
            sig.append([p, st.st_size, st.st_mtime_ns])
        except OSError:
            sig.append([p, -1, -1])
    return sig


def sig_valid(sig):
    return bool(sig) and stat_sig([s[0] for s in sig]) == sig


# ---------------------------------------------------------------------------------------------
# Identification d'une source
# ---------------------------------------------------------------------------------------------
def version_key(v):
    try:
        return tuple(int(x) for x in v.split("."))
    except ValueError:
        return (0,)


def game_type(content_type):
    return {0x80000: "demo", 0xE0000: "xna"}.get(content_type, "arcade")


def identify_source(path, kind):
    """Renvoie (enregistrement, icône | None). Enregistrement : kind = game | not_game | unknown | error."""
    rec = {"source": path}
    try:
        fmt = "dir" if kind == "dir" else xc.detect_file(path)
    except OSError as e:
        return {**rec, "kind": "error", "error": str(e)}, None
    if fmt is None:
        return {**rec, "kind": "unknown"}, None
    if fmt == "god-sans-data":
        return {**rec, "kind": "error", "format": "god", "error": "en-tête Games on Demand sans dossier .data"}, None
    rec["format"] = fmt
    c = None
    try:
        c = xc.open_container(path)
        meta = c.metadata()
        ctype = meta.get("content_type")
        if ctype is not None:
            rec["content_type"] = f"0x{ctype:X}"
            rec["content_type_name"] = meta.get("content_type_name")
        if ctype is not None and ctype not in xc.GAME_TYPES:
            return {**rec, "kind": "not_game", "title_id": meta.get("title_id"),
                    "reason": f"contenu de type {meta.get('content_type_name')} (pas un jeu)"}, None
        x = c.xex_info()
        if x is None:
            names = {f["name"].lower() for f in c.list()} if c.format != "dir" else set()
            reason = "pas de default.xex"
            if "default.xbe" in names:
                reason = "jeu Xbox d'origine (default.xbe), non pris en charge"
            elif "default.xexp" in names:
                reason = "mise à jour de titre (default.xexp)"
            return {**rec, "kind": "not_game", "title_id": meta.get("title_id"), "reason": reason}, None
        if not x["is_title"] or x["title_id"] in ("00000000",) or x["title_id"].startswith("FFFE"):
            return {**rec, "kind": "not_game", "title_id": x["title_id"],
                    "reason": "module système ou bibliothèque (pas un titre)"}, None
        name, icon, origin = c.title_data()
        rec.update({
            "kind": "game", "title_id": x["title_id"], "name": name or f"Titre {x['title_id']}",
            "name_origin": origin or "TitleID", "media_id": x["media_id"], "version": x["version"],
            "base_version": x["base_version"], "disc_number": x["disc_number"],
            "type": game_type(ctype), "icon_bytes": len(icon) if icon else 0,
        })
        rec["sig"] = stat_sig(c.signature_files())
        return rec, icon
    except Exception as e:      # un fichier corrompu ne fait jamais échouer le scan
        return {**rec, "kind": "error", "error": f"{type(e).__name__}: {e}"}, None
    finally:
        if c:
            c.close()


def src_hash(path):
    return hashlib.sha1(path.encode()).hexdigest()[:16]


def atomic_write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
    os.replace(tmp, path)


# ---------------------------------------------------------------------------------------------
# Inventaire
# ---------------------------------------------------------------------------------------------
def load_library(out=None):
    try:
        with open(os.path.join(library_dir(out), "library.json"), encoding="utf-8") as f:
            lib = json.load(f)
        return lib if lib.get("schema") == SCHEMA else None
    except (OSError, ValueError):
        return None


def scan(gdir=None, out=None, force=False):
    gdir, out = games_dir(gdir), library_dir(out)
    t0 = time.time()
    os.makedirs(os.path.join(out, "icons", ".cache"), exist_ok=True)
    prev = {} if force else (load_library(out) or {}).get("sources", {})
    cache_icons = os.path.join(out, "icons", ".cache")
    sources, reused = {}, 0
    for path, kind in discover(gdir) if os.path.isdir(gdir) else []:
        old = prev.get(path)
        icon_file = os.path.join(cache_icons, src_hash(path) + ".png")
        if old and old.get("kind_hint") == kind and sig_valid(old.get("sig")) and (
                old["kind"] != "game" or not old.get("icon_bytes") or os.path.isfile(icon_file)):
            sources[path] = old
            reused += 1
            continue
        rec, icon = identify_source(path, kind)
        rec["kind_hint"] = kind
        if "sig" not in rec:
            rec["sig"] = stat_sig([path] if kind == "file" else [os.path.join(path, "default.xex")])
        if icon:
            atomic_write(icon_file, icon)
        elif os.path.isfile(icon_file):
            os.remove(icon_file)
        sources[path] = rec

    # Un jeu = un TitleID : la meilleure source (version la plus haute, puis format) devient la principale.
    by_title = {}
    for path, r in sources.items():
        if r["kind"] == "game":
            by_title.setdefault(r["title_id"], []).append(r)
    games = {}
    os.makedirs(os.path.join(out, "icons"), exist_ok=True)
    for tid, lst in sorted(by_title.items()):
        lst.sort(key=lambda r: (tuple(-v for v in version_key(r["version"])), PREFERENCE.get(r["format"], 9),
                                r["source"]))
        best = lst[0]
        g = {k: best[k] for k in ("title_id", "name", "name_origin", "format", "source", "type", "media_id",
                                  "version", "base_version", "disc_number", "icon_bytes")}
        if best.get("content_type"):
            g["content_type"] = best["content_type"]
        icon_src = os.path.join(cache_icons, src_hash(best["source"]) + ".png")
        icon_dst = os.path.join(out, "icons", f"{tid}.png")
        if best["icon_bytes"] and os.path.isfile(icon_src):
            shutil.copyfile(icon_src, icon_dst)
            g["icon"] = icon_dst
            g["icon_oversize"] = best["icon_bytes"] > ICON_LIMIT
        else:
            g["icon"] = None
            if os.path.isfile(icon_dst):
                os.remove(icon_dst)
        g["alternates"] = [{k: r.get(k) for k in ("source", "format", "version", "media_id", "disc_number")}
                           for r in lst[1:]]
        games[tid] = g
    # icônes de jeux disparus
    for f in os.listdir(os.path.join(out, "icons")):
        if f.endswith(".png") and f[:-4] not in games:
            os.remove(os.path.join(out, "icons", f))
    live = {src_hash(p) + ".png" for p, r in sources.items() if r["kind"] == "game"}
    for f in os.listdir(cache_icons):
        if f not in live:
            os.remove(os.path.join(cache_icons, f))

    lib = {
        "schema": SCHEMA, "scanned_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "scan_seconds": round(time.time() - t0, 3),
        "games_dir": gdir, "out_dir": out, "sources_reused": reused, "sources_total": len(sources),
        "games": games,
        "not_games": [{"source": r["source"], "format": r.get("format"), "title_id": r.get("title_id"),
                       "reason": r["reason"]} for r in sources.values() if r["kind"] == "not_game"],
        "errors": [{"source": r["source"], "format": r.get("format"), "error": r["error"]}
                   for r in sources.values() if r["kind"] == "error"],
        "unrecognized": [r["source"] for r in sources.values() if r["kind"] == "unknown"],
        "sources": sources,
    }
    atomic_write(os.path.join(out, "library.json"), json.dumps(lib, indent=1, ensure_ascii=False))
    atomic_write(os.path.join(out, "games.auto.toml"), render_toml(games))
    return lib


def tq(s):
    """Chaîne TOML (chaîne de base) : json.dumps produit des échappements valides en TOML."""
    return json.dumps(s, ensure_ascii=False)


def render_toml(games):
    lines = ["# Généré par tools/game_library.py (inventaire du dossier de jeux) : ne pas modifier, il est refait à chaque scan.",
             "# Même format que games.toml ; le Dashboard le fusionne avec games.toml (celui-ci l'emporte).",
             "library = true", ""]
    for tid, g in sorted(games.items()):
        lines.append(f"[game.{tid}]")
        lines.append(f"name = {tq(g['name'])}")
        lines.append(f"recipe = {tq(g['source'])}")
        lines.append(f"type = {tq(g['type'])}")
        if g.get("icon"):
            lines.append(f"icon = {tq(g['icon'])}")
        lines.append(f"format = {tq(g['format'])}")
        lines.append(f"media_id = {tq(g['media_id'])}")
        lines.append(f"version = {tq(g['version'])}")
        lines.append("")
    return "\n".join(lines)


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------
def print_report(lib):
    g = lib["games"]
    print(f"{len(g)} jeu(x) dans {lib['games_dir']} ({lib['sources_total']} sources, {lib['sources_reused']} en cache, "
          f"{lib['scan_seconds']} s)")
    print_table(g)
    for label, items, fmt in (("ignoré ", lib["not_games"], lambda n: f"{n['source']} : {n['reason']}"),
                              ("erreur ", lib["errors"], lambda e: f"{e['source']} : {e['error']}"),
                              ("inconnu", lib["unrecognized"], lambda u: u)):
        for it in items[:10]:
            print(f"  {label} {fmt(it)}")
        if len(items) > 10:
            print(f"  {label} ... et {len(items) - 10} autres (library.json)")
    for t, x in g.items():
        if x.get("icon_oversize"):
            print(f"  icône > 16 Ko ({x['icon_bytes']} octets, conservée telle quelle) : {t}")
    print(f"écrit : {os.path.join(lib['out_dir'], 'games.auto.toml')}")


def print_table(games):
    for tid, g in sorted(games.items()):
        print(f"  {tid}  {g['name'][:32]:32}  {g['format']:6}  {g['version']:10}  {g['source']}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Bibliothèque de jeux XenonDash")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("init"); s.add_argument("--dir"); s.add_argument("--json", action="store_true")
    s = sub.add_parser("scan"); s.add_argument("--dir"); s.add_argument("--out"); s.add_argument("--force", action="store_true")
    s.add_argument("--json", action="store_true")
    s = sub.add_parser("list"); s.add_argument("--out"); s.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    if a.cmd == "init":
        r = init(a.dir)
        print(json.dumps(r, ensure_ascii=False) if a.json else f"dossier de jeux : {r['games_dir']}")
    elif a.cmd == "scan":
        lib = scan(a.dir, a.out, a.force)
        if a.json:
            print(json.dumps(lib, indent=1, ensure_ascii=False))
        else:
            print_report(lib)
    else:
        lib = load_library(a.out)
        if not lib:
            print("aucun inventaire : lancer `game_library.py scan`", file=sys.stderr)
            return 1
        if a.json:
            print(json.dumps(lib, indent=1, ensure_ascii=False))
        else:
            print(f"{len(lib['games'])} jeu(x), scan du {lib['scanned_at']}")
            print_table(lib["games"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
