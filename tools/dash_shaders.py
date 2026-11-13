#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shaders du rendu natif des Dashboards (native_gpu/, profil 13604) : nommage par cle de microcode et boucle « jouer -> reconstruire ».

La facade (front_profile.cpp) cherche le SPIR-V d'un shader dans DASH_NGPU_SHADERS sous le nom <cle>.spv, cle = XXH3-64 du microcode
(la cle de cache de XenosRecomp avec XENOS_RECOMP_MICROCODE_KEY). Rien de ce que cet outil produit n'est versionne (derives de
microcode Microsoft).

  dash_shaders.py extract <image.pe> <dossier>        conteneurs 0x102A11xx de l'image -> <dossier>/<adresse>_<VS|PS>.bin
                                                       (le meme balayage que tools/analyse_d3d.py extract, sans la classe « video »)
  dash_shaders.py translate <dossier_bin> <sortie> [racine XenosRecomp]   XenosRecomp + dxc (tools/xenos_batch.py) puis <cle>.spv
  dash_shaders.py keys <dossier_bin> <dossier_spv_xenos_batch> <sortie>   renomme ui_<adresse>_<VS|PS>.spv en <cle>.spv
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


def xxh3(data):
    with tempfile.NamedTemporaryFile() as f:
        f.write(data)
        f.flush()
        out = subprocess.run(["xxhsum", "-H3", f.name], capture_output=True, text=True, check=True).stdout
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
    print(f"{n} conteneurs -> {out}")


def run_batch(src, out, root):
    r = subprocess.run([sys.executable, "-I", os.path.join(ROOT, "tools", "xenos_batch.py"), src, out, root])
    return r.returncode


def keys(bindir, spvdir, out):
    os.makedirs(out, exist_ok=True)
    done = 0
    for name in sorted(os.listdir(bindir)):
        if not name.endswith(".bin"):
            continue
        spv = os.path.join(spvdir, "ui_" + name[:-4] + ".spv")
        if not os.path.isfile(spv):
            continue
        key = microcode_key(open(os.path.join(bindir, name), "rb").read())
        if key is not None:
            shutil.copyfile(spv, os.path.join(out, f"{key:016X}.spv"))
            done += 1
    print(f"{done} shaders nommes par cle -> {out}")


def translate(bindir, out, root):
    tmp = tempfile.mkdtemp(prefix="dashsh-")
    try:
        os.makedirs(os.path.join(tmp, "ui"))
        for name in os.listdir(bindir):
            if name.endswith(".bin"):
                shutil.copyfile(os.path.join(bindir, name), os.path.join(tmp, "ui", name))
        run_batch(tmp, tmp + "-out", root)
        keys(os.path.join(tmp, "ui"), tmp + "-out", out)
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
        run_batch(tmp, tmp + "-out", root)
        os.makedirs(out, exist_ok=True)
        for name in os.listdir(os.path.join(tmp, "ui")):
            spv = os.path.join(tmp + "-out", "ui_" + name[:-4] + ".spv")
            if os.path.isfile(spv):
                shutil.copyfile(spv, os.path.join(out, name.split("_")[0] + ".spv"))
                print("ajoute", name.split("_")[0])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(tmp + "-out", ignore_errors=True)


def main():
    a = sys.argv[1:]
    default_root = os.path.join(ROOT, "work", "XenosRecomp-unified")
    if len(a) >= 3 and a[0] == "extract":
        extract(a[1], a[2])
    elif len(a) >= 3 and a[0] == "translate":
        translate(a[1], a[2], a[3] if len(a) > 3 else default_root)
    elif len(a) == 4 and a[0] == "keys":
        keys(a[1], a[2], a[3])
    elif len(a) >= 3 and a[0] == "missing":
        missing(a[1], a[2], a[3] if len(a) > 3 else default_root)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
