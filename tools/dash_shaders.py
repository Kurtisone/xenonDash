#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shaders du rendu natif des Dashboards (native_gpu/, profil 13604) : nommage par cle de microcode et boucle « jouer -> reconstruire ».

La facade (front_profile.cpp) cherche le SPIR-V d'un shader dans <game_data_root>/shaders/dash (ou DASH_NGPU_SHADERS) sous le nom
<cle>.spv, cle = XXH3-64 du microcode (la cle de cache de XenosRecomp avec XENOS_RECOMP_MICROCODE_KEY). Rien de ce que cet outil
produit n'est versionne (derives de microcode Microsoft).

Outils externes : xxhsum (XXH3 ; $XD_XXHSUM, sinon xxhsum du PATH) ; XenosRecomp (racine : $XD_XENOSRECOMP, sinon
work/XenosRecomp-unified, comme tools/recipe_build.py) avec son dxc-linux.

  dash_shaders.py extract <image.pe> <dossier>        conteneurs 0x102A11xx de l'image -> <dossier>/<adresse>_<VS|PS>.bin
  dash_shaders.py translate <dossier_bin> <sortie> [racine XenosRecomp]   XenosRecomp + dxc puis <cle>.spv (progression sur stdout)
  dash_shaders.py keys <dossier_bin> <dossier_spv_batch> <sortie>         renomme ui_<adresse>_<VS|PS>.spv en <cle>.spv
  dash_shaders.py missing <dossier_manques> <sortie> [racine XenosRecomp]  traduit les conteneurs ecrits par la facade
                                                       (DASH_NGPU_MISSING=<dossier>, fichiers <cle>_<VS|PS>.bin) et les ajoute a <sortie>
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def xxhsum_bin():
    """xxhsum : $XD_XXHSUM, sinon le PATH (l'AppImage embarque le sien et définit XD_XXHSUM)."""
    b = os.environ.get("XD_XXHSUM") or shutil.which("xxhsum")
    if not b:
        raise RuntimeError("xxhsum introuvable (paquet xxhash, ou XD_XXHSUM=<chemin>)")
    return b


def xenos_root():
    """Racine de XenosRecomp : dérivée de XD_XENOSRECOMP (le binaire, comme tools/recipe_build.py), sinon work/XenosRecomp-unified."""
    x = os.environ.get("XD_XENOSRECOMP")
    if x:
        return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(x))))
    return os.path.join(ROOT, "work", "XenosRecomp-unified")


def xenos_missing(root):
    """Fichiers de XenosRecomp absents sous `root` (liste vide si tout est là)."""
    need = ["build/XenosRecomp/XenosRecomp", "thirdparty/dxc-bin/bin/x64/dxc-linux", "XenosRecomp/shader_common.h"]
    return [n for n in need if not os.path.exists(os.path.join(root, n))]


def xxh3(data):
    with tempfile.NamedTemporaryFile() as f:
        f.write(data)
        f.flush()
        out = subprocess.run([xxhsum_bin(), "-H3", f.name], capture_output=True, text=True, check=True).stdout
    return int(out.split("_")[1].split()[0], 16)


def microcode_key(container):
    """XXH3-64 du microcode d'un conteneur (partie virtuelle puis physique) ; None si ce n'est pas un conteneur."""
    if len(container) < 0x30 or struct.unpack(">I", container[:4])[0] & 0xFFFFFFFE != 0x102A1100:
        return None
    vsize = struct.unpack(">I", container[4:8])[0]
    shader = struct.unpack(">I", container[0x18:0x1C])[0]
    offset, size = struct.unpack(">II", container[shader:shader + 8])
    return xxh3(container[vsize + offset:vsize + offset + size])


def extract(image, out):
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import d3d_match as m
    img = m.Image(image)
    d = img.data
    os.makedirs(out, exist_ok=True)
    n = 0
    for o in range(0, len(d) - 0x30, 4):
        if d[o:o + 3] != b"\x10\x2a\x11" or d[o + 3] & 0xFE != 0:
            continue
        magic = d[o + 3]
        vsize, psize = struct.unpack(">II", d[o + 4:o + 12])
        if not 0x30 <= vsize < 0x10000 or not 0 < psize < 0x80000 or o + vsize + psize > len(d):
            continue
        shader = struct.unpack(">I", d[o + 0x18:o + 0x1C])[0]
        if shader >= vsize:
            continue
        kind = "VS" if magic & 1 else "PS"
        with open(os.path.join(out, f"{img.base + o:08X}_{kind}.bin"), "wb") as f:
            f.write(d[o:o + vsize + psize])
        n += 1
    print(f"{n} conteneurs -> {out}", flush=True)
    return n


def batch(src, out, root, text_fix=None, log=None):
    """XenosRecomp puis dxc sur chaque <src>/ui/*.bin -> <out>/ui_<nom>.spv (comme tools/xenos_batch.py, avec progression).
    `text_fix(hlsl) -> hlsl` : retouche facultative du HLSL produit. Renvoie la liste des échecs (nom, étape, cause)."""
    X = f"{root}/build/XenosRecomp/XenosRecomp"
    DXC = f"{root}/thirdparty/dxc-bin/bin/x64/dxc-linux"
    INC = f"{root}/XenosRecomp/shader_common.h"
    libs = [p for p in (f"{root}/build/XenosRecomp", f"{root}/thirdparty/dxc-bin/lib/x64") if os.path.isdir(p)]
    env = dict(os.environ, LD_LIBRARY_PATH=":".join(libs + [os.environ.get("LD_LIBRARY_PATH", "")]).rstrip(":"))
    os.makedirs(out, exist_ok=True)
    files = sorted(f for f in os.listdir(os.path.join(src, "ui")) if f.endswith(".bin"))
    fails = []
    for i, f in enumerate(files, 1):
        name = f[:-4]
        h, o = f"{out}/ui_{name}.hlsl", f"{out}/ui_{name}.spv"
        try:
            r = subprocess.run([X, os.path.join(src, "ui", f), h, INC], capture_output=True, text=True, timeout=60)
            if r.returncode != 0 or not os.path.exists(h):
                fails.append((name, "recomp", (r.stderr.strip().splitlines() or [f"code {r.returncode}"])[-1][:160]))
            else:
                if text_fix:
                    with open(h) as fh:
                        text = text_fix(fh.read())
                    with open(h, "w") as fh:
                        fh.write(text)
                t = "ps_6_0" if name.endswith("PS") else "vs_6_0"
                args = [DXC, "-T", t, "-HV", "2021", "-all-resources-bound", "-spirv", "-fvk-use-dx-layout", "-Qstrip_debug"]
                if t == "vs_6_0":
                    args.append("-fvk-invert-y")
                r = subprocess.run(args + [h, "-Fo", o], capture_output=True, text=True, env=env, timeout=300)
                if r.returncode != 0 or not os.path.exists(o):
                    errs = [l for l in r.stderr.splitlines() if "error" in l]
                    fails.append((name, "dxc", (errs[0] if errs else r.stderr.strip())[:160]))
        except subprocess.TimeoutExpired:
            fails.append((name, "timeout", ""))
        if i % 10 == 0 or i == len(files):
            (log or (lambda m: print(f"[shaders] {m}", flush=True)))(f"traduction {i}/{len(files)} ({len(fails)} échec(s))")
    return fails


def keys(bindir, spvdir, out):
    """Renomme les <adresse>_<VS|PS>.bin traduits en <cle>.spv. Renvoie (nombre, noms non traduits)."""
    os.makedirs(out, exist_ok=True)
    done, untranslated = 0, []
    for name in sorted(os.listdir(bindir)):
        if not name.endswith(".bin"):
            continue
        spv = os.path.join(spvdir, "ui_" + name[:-4] + ".spv")
        if not os.path.isfile(spv):
            untranslated.append(name[:-4])
            continue
        key = microcode_key(open(os.path.join(bindir, name), "rb").read())
        if key is not None:
            shutil.copyfile(spv, os.path.join(out, f"{key:016X}.spv"))
            done += 1
        else:
            untranslated.append(name[:-4])
    return done, untranslated


def translate(bindir, out, root, text_fix=None, log=print):
    """Traduit tous les .bin de `bindir` vers <out>/<cle>.spv. Renvoie (nombre, échecs)."""
    tmp = tempfile.mkdtemp(prefix="dashsh-")
    try:
        os.makedirs(os.path.join(tmp, "ui"))
        for name in os.listdir(bindir):
            if name.endswith(".bin"):
                shutil.copyfile(os.path.join(bindir, name), os.path.join(tmp, "ui", name))
        fails = batch(tmp, tmp + "-out", root, text_fix, log)
        done, untranslated = keys(os.path.join(tmp, "ui"), tmp + "-out", out)
        print(f"{done} shaders nommes par cle -> {out} ({len(untranslated)} non traduit(s))", flush=True)
        deja = {n for n, _, _ in fails}
        return done, fails + [(n, "absent", "") for n in untranslated if n not in deja]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(tmp + "-out", ignore_errors=True)


def missing(mdir, out, root):
    """<cle>_<VS|PS>.bin ecrits par la facade -> <cle>.spv dans `out`."""
    tmp = tempfile.mkdtemp(prefix="dashmiss-")
    try:
        os.makedirs(os.path.join(tmp, "ui"))
        for name in os.listdir(mdir):
            m = re.match(r"([0-9A-F]{16})_(VS|PS)\.bin$", name)
            if m:
                shutil.copyfile(os.path.join(mdir, name), os.path.join(tmp, "ui", f"{m.group(1)}_{m.group(2)}.bin"))
        batch(tmp, tmp + "-out", root)
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(os.path.join(tmp, "ui")):
            spv = os.path.join(tmp + "-out", "ui_" + name[:-4] + ".spv")
            if os.path.isfile(spv):
                shutil.copyfile(spv, os.path.join(out, name.split("_")[0] + ".spv"))
                print("ajoute", name.split("_")[0], flush=True)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(tmp + "-out", ignore_errors=True)


def main():
    a = sys.argv[1:]
    if len(a) == 3 and a[0] == "extract":
        extract(a[1], a[2])
    elif len(a) >= 3 and a[0] == "translate":
        translate(a[1], a[2], a[3] if len(a) > 3 else xenos_root())
    elif len(a) == 4 and a[0] == "keys":
        print(keys(a[1], a[2], a[3]))
    elif len(a) >= 3 and a[0] == "missing":
        missing(a[1], a[2], a[3] if len(a) > 3 else xenos_root())
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
