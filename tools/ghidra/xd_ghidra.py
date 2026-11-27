#!/usr/bin/env python3
"""Projet Ghidra des modules du Dashboard Kinect 2.0.13604 (xam.xex, dash.xex), sans auto-analyse.

Les modules sont chargés tels qu'en mémoire (image produite par tools/xexinfo, la même que celle que ReXGlue recompile)
à leur adresse de base, en PowerPC:BE:64:A2ALT-32addr (Altivec, adresses 32 bits ; les instructions VMX128 du Xenon ne
sont pas décodées : une fonction qui en contient s'arrête à la première). L'auto-analyse de Ghidra n'est pas lancée
(lente, et ReXGlue connaît déjà les fonctions) ; les entrées appliquées viennent, par priorité croissante, de :
  1. ReXGlue : débuts de fonctions (generated/<module>/codegen.partition.json), nommés sub_XXXXXXXX ;
  2. aides de sauvegarde/restauration des registres (__savegprlr_NN, __restfpr_NN...), lues dans le code généré et
     marquées « inline » (sinon le décompilateur prend r3 pour leur valeur de retour et perd le premier argument) ;
  3. imports (thunks et variables, sortie de xexinfo), nommés sans le préfixe __imp__ ;
  4. exports de XAM (table d'exports du XEX, noms d'après les ordinaux du SDK : third_party/xenondash-sdk) ;
  5. le fichier de symboles versionné symbols/<version>/<module>.tsv (noms et commentaires de notre analyse).

Sous-commandes :
  entries   <module>                    écrit <travail>/<module>.entries.tsv (sans Ghidra)
  import    <module>                    (re)crée le programme <module>.img du projet et applique les entrées
  apply     <module>                    réapplique les entrées (après modification du fichier de symboles)
  decompile <module> <cible>... [-o D]  C décompilé, un fichier par fonction (cible : 0x819BE610, sub_819BE610 ou un nom)
Modules : xam, dash, et les modules XUI (signin, gamerprofile, createprofile, Guide_AvatarMiniCreator). Les XEX sont lus dans --data (dossier de données de l'AppImage, par défaut
~/.local/share/xenondash/2.0.13604.0-xam). Dossier de travail : XD_GHIDRA_WORK, sinon work/ghidra/2.0.13604.0 ; le
projet (xd13604.gpr) et les images restent hors du dépôt : ils contiennent du code Microsoft. Ghidra : GHIDRA_INSTALL_DIR,
sinon /opt/ghidra. Le projet ne doit pas être ouvert dans l'interface de Ghidra pendant une commande (verrou).
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
VERSION = "2.0.13604.0"
VARIANT = VERSION + "-xam"
PROJECT = "xd13604"
LANG = "PowerPC:BE:64:A2ALT-32addr"
MODULES = {"xam": "xam.xex", "dash": "dash.xex"}
# Modules XUI recompilés (chargés à la demande à 0x90100000) : XEX dans flash/, code généré dans generated/dash/<module>.
XUI_MODULES = ("signin", "gamerprofile", "createprofile", "Guide_AvatarMiniCreator")
for _m in XUI_MODULES:
    MODULES[_m] = "flash/" + _m.replace("_", ".") + ".xex"
SDK_XAM_EXPORTS = os.path.join(ROOT, "third_party/xenondash-sdk/src/kernel/xam/export_table.inc")
NAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
HELPER_RE = re.compile(r"\t// bl? 0x([0-9a-f]+)\n(?:\t[^\n]*\n)?\t(__(?:save|rest)[a-z]+_\d+)\(ctx, base\)")

# priorités des sources de noms
P_REX, P_HELPER, P_IMPORT, P_EXPORT, P_SYMBOLS = range(5)


def main_checkout():
    """Copie de travail principale du dépôt (celle d'un worktree se trouve sous .claude/worktrees/...) ; ROOT sinon."""
    try:
        r = subprocess.run(["git", "-C", ROOT, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                           capture_output=True, text=True, timeout=10)
        if r.returncode == 0 and r.stdout.strip():
            return os.path.dirname(r.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        pass
    return ROOT


def work_dir():
    # Un seul projet pour tous les worktrees, dans work/ de la copie principale : Ghidra refuse un chemin de projet
    # dont un élément commence par « . » (.claude/worktrees/...).
    d = os.environ.get("XD_GHIDRA_WORK") or os.path.join(main_checkout(), "work", "ghidra", VERSION)
    if any(part.startswith(".") for part in d.split(os.sep) if part):
        sys.exit(f"Ghidra refuse ce chemin de projet (élément commençant par « . ») : {d} ; choisir XD_GHIDRA_WORK")
    os.makedirs(d, exist_ok=True)
    return d


def symbols_path(module):
    return os.path.join(ROOT, "symbols", VERSION, module + ".tsv")


def load_symbols(path):
    """Fichier versionné : adresse<TAB>nom<TAB>commentaire ; nom vide = garder le nom automatique."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#") or line.startswith("adresse\t"):
                continue
            p = line.split("\t")
            try:
                addr = int(p[0], 16)
            except ValueError:
                sys.exit(f"{path}:{n} : adresse illisible « {p[0]} » (colonnes séparées par des tabulations)")
            name = p[1].strip() if len(p) > 1 else ""
            comment = p[2].strip() if len(p) > 2 else ""
            if name and not NAME_RE.match(name):
                sys.exit(f"{path}:{n} : nom invalide « {name} »")
            if addr in out:
                sys.exit(f"{path}:{n} : adresse 0x{addr:08X} en double")
            out[addr] = (name, comment)
    return out


def parse_xexinfo(text):
    info = {"sections": [], "syms": []}
    for line in text.split("\n"):  # pas splitlines() : un nom de section de dash.xex finit par \x0c (« .XBMOVIE\f »)
        p = line.split()
        if not p:
            continue
        if p[0] == "BASE":
            info["base"], info["size"] = int(p[1], 16), int(p[3], 16)
        elif p[0] == "SECT":
            info["sections"].append((p[1], int(p[2], 16), int(p[3], 16), int(p[4].split("=")[1])))
        elif p[0] == "SYM":
            info["syms"].append((int(p[1], 16), p[2]))
    if "base" not in info:
        sys.exit("sortie de xexinfo sans ligne BASE")
    return info


def image(module, data):
    """Image mémoire du module (xexinfo) et ses informations ; refaite si le XEX est plus récent."""
    xex = os.path.join(data, MODULES[module])
    if not os.path.isfile(xex):
        sys.exit(f"XEX absent : {xex} (--data : dossier de données de l'AppImage)")
    w = work_dir()
    img, info_path = os.path.join(w, module + ".img"), os.path.join(w, module + ".xexinfo.txt")
    if not (os.path.exists(img) and os.path.exists(info_path) and os.path.getmtime(img) >= os.path.getmtime(xex)):
        xi = os.path.join(ROOT, "tools/xexinfo/build/xexinfo")
        if not os.access(xi, os.X_OK):
            sys.exit(f"xexinfo absent : {xi} (cmake -S tools/xexinfo -B tools/xexinfo/build && cmake --build tools/xexinfo/build)")
        r = subprocess.run([xi, xex, img], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"xexinfo a échoué : {r.stderr.strip()}")
        with open(info_path, "w") as f:
            f.write(r.stdout)
    with open(info_path) as f:
        return xex, img, parse_xexinfo(f.read())


def xam_exports(xex, img, base):
    """(adresse, nom, genre SDK) des exports de XAM : table pointée par l'en-tête de sécurité du XEX (+0x160)."""
    names = {}
    with open(SDK_XAM_EXPORTS) as f:
        for m in re.finditer(r"XE_EXPORT\(\s*xam\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\w+)\s*,\s*(\w+)", f.read()):
            names[int(m.group(1), 16)] = (m.group(2), m.group(3))
    with open(xex, "rb") as f:
        x = f.read(0x10000)
    with open(img, "rb") as f:
        d = f.read()
    if x[:4] != b"XEX2":
        return []
    sec = struct.unpack(">I", x[0x10:0x14])[0]
    et = struct.unpack(">I", x[sec + 0x160:sec + 0x164])[0]
    if not et:
        return []
    o = et - base
    ibase, count, ordbase = struct.unpack(">III", d[o + 0x20:o + 0x2C])
    out = []
    for i, off in enumerate(struct.unpack(f">{count}I", d[o + 0x2C:o + 0x2C + 4 * count])):
        if off:
            name, kind = names.get(ordbase + i, (f"xam_ordinal_{ordbase + i}", "kFunction"))
            out.append((off + (ibase << 16), name, kind))
    return out


def helpers(gen_dir):
    res = {}
    for fn in sorted(os.listdir(gen_dir)):
        if re.match(r"dash_recomp\.\d+\.cpp$", fn):
            with open(os.path.join(gen_dir, fn), encoding="utf-8", errors="replace") as f:
                for m in HELPER_RE.finditer(f.read()):
                    res[int(m.group(1), 16)] = m.group(2)
    return res


def build_entries(module, data):
    xex, img, info = image(module, data)
    base = info["base"]
    gen = os.path.join(ROOT, "dashboards", VARIANT, "generated", *(("dash", module) if module in XUI_MODULES else (module,)))
    part = os.path.join(gen, "codegen.partition.json")
    if not os.path.exists(part):
        sys.exit(f"code généré absent : {part} (il est produit par la compilation de {VARIANT})")
    code = [(va, va + size) for _, va, size, flags in info["sections"] if flags & 2]
    E = {}

    def put(a, kind, name, size=0, prio=P_REX):
        cur = E.get(a)
        if cur is None:
            E[a] = {"kind": kind, "name": name, "size": size, "comment": "", "prio": prio}
        elif prio >= cur["prio"]:
            cur.update(name=name, prio=prio)
            if kind != "fn":
                cur.update(kind=kind, size=size)

    with open(part) as f:
        for k in json.load(f)["assignments"]:
            put(int(k, 16), "fn", "sub_" + k.upper())
    for a, n in helpers(gen).items():
        put(a, "inl", n, prio=P_HELPER)
    for a, n in info["syms"]:
        n = n[len("__imp__"):] if n.startswith("__imp__") else n
        if any(lo <= a < hi for lo, hi in code):
            put(a, "imp", n, 16, P_IMPORT)
        else:
            put(a, "lbl", n, prio=P_IMPORT)
    nexp = 0
    if module == "xam":
        for a, n, kind in xam_exports(xex, img, base):
            put(a, "fn" if kind == "kFunction" else "lbl", n, prio=P_EXPORT)
            nexp += 1
    syms = load_symbols(symbols_path(module))
    for a, (name, comment) in syms.items():
        cur = E.get(a)
        if cur is None:
            in_code = any(lo <= a < hi for lo, hi in code)
            if in_code:
                print(f"attention : 0x{a:08X} ({name or 'sans nom'}) n'est pas un début de fonction connu de ReXGlue")
            cur = E[a] = {"kind": "fn" if in_code else "lbl", "name": name or (f"sub_{a:08X}" if in_code else f"d_{a:08X}"),
                          "size": 0, "comment": "", "prio": P_SYMBOLS}
        if name:
            cur.update(name=name, prio=P_SYMBOLS)
        cur["comment"] = comment
    # Noms en double (Ghidra refuse deux fonctions de même nom) : le moins prioritaire prend le suffixe _<adresse>.
    by_name = {}
    for a, e in E.items():
        by_name.setdefault(e["name"], []).append(a)
    for name, addrs in by_name.items():
        if len(addrs) > 1:
            addrs.sort(key=lambda a: -E[a]["prio"])
            for a in addrs[1:]:
                E[a]["name"] = f"{name}_{a:08X}"
    out = os.path.join(work_dir(), module + ".entries.tsv")
    with open(out, "w", encoding="utf-8") as f:
        f.write(f"# entrées Ghidra de {module} ({VERSION}), produites par tools/ghidra/xd_ghidra.py : genre, adresse, nom, taille, commentaire\n")
        for a in sorted(E):
            e = E[a]
            f.write(f"{e['kind']}\t{a:08X}\t{e['name']}\t{e['size'] or ''}\t{e['comment']}\n")
    kinds = {k: sum(1 for e in E.values() if e["kind"] == k) for k in ("fn", "inl", "imp", "lbl")}
    print(f"{module} : base 0x{base:08X}, {kinds['fn']} fonctions, {kinds['inl']} aides inline, {kinds['imp']} thunks d'import, "
          f"{kinds['lbl']} étiquettes, "
          f"{nexp} exports, {len(syms)} symboles versionnés -> {out}")
    return img, info, out


def headless(args, log_name):
    exe = os.path.join(os.environ.get("GHIDRA_INSTALL_DIR", "/opt/ghidra"), "support", "analyzeHeadless")
    if not os.access(exe, os.X_OK):
        sys.exit(f"Ghidra absent : {exe} (GHIDRA_INSTALL_DIR)")
    w = work_dir()
    log = os.path.join(w, log_name)
    r = subprocess.run([exe, w, PROJECT] + args + ["-scriptPath", HERE, "-log", log + ".ghidra"],
                       capture_output=True, text=True)
    out = r.stdout + r.stderr
    with open(log, "w") as f:
        f.write(out)
    for line in out.splitlines():
        if ".java>" in line or "ERROR" in line or "Exception" in line or "locked" in line.lower():
            print(line.strip()[:300])
    if r.returncode != 0:
        sys.exit(f"analyzeHeadless : code {r.returncode} (journal {log})")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", default=os.path.expanduser(f"~/.local/share/xenondash/{VARIANT}"))
    sp = ap.add_subparsers(dest="cmd", required=True)
    for c in ("entries", "import", "apply"):
        p = sp.add_parser(c)
        p.add_argument("module", choices=sorted(MODULES))
    p = sp.add_parser("decompile")
    p.add_argument("module", choices=sorted(MODULES))
    p.add_argument("targets", nargs="+")
    p.add_argument("-o", "--out", default=None)
    a = ap.parse_args()

    if a.cmd == "decompile":
        out = os.path.abspath(a.out or os.path.join(work_dir(), "decomp", a.module))
        headless(["-process", a.module + ".img", "-noanalysis", "-readOnly", "-postScript", "XdDecompile.java", out]
                 + a.targets, f"decompile_{a.module}.log")
        return
    img, info, ent = build_entries(a.module, a.data)
    if a.cmd == "import":
        headless(["-import", img, "-overwrite", "-loader", "BinaryLoader", "-loader-baseAddr", f"0x{info['base']:08X}",
                  "-loader-blockName", a.module, "-processor", LANG, "-cspec", "default", "-noanalysis",
                  "-postScript", "XdApplySymbols.java", ent], f"import_{a.module}.log")
        print(f"projet : {os.path.join(work_dir(), PROJECT + '.gpr')} (programme {a.module}.img)")
    elif a.cmd == "apply":
        headless(["-process", a.module + ".img", "-noanalysis", "-postScript", "XdApplySymbols.java", ent],
                 f"apply_{a.module}.log")


if __name__ == "__main__":
    main()
