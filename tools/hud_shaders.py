#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Shaders du HUD de XAM 13604 (rendu natif, palier N2) : conteneurs de l'ancien format (D3D9 2.0.2099 lie dans xam.xex).

XAM embarque une XDK D3D bien plus ancienne que celle du Dashboard : ses conteneurs de shaders portent la magie 0x102A0E0x et un
en-tete de 6 mots (magie, taille virtuelle, taille physique, table de definitions, table de constantes, structure du shader), la
structure du shader ayant deux mots de plus que celle des conteneurs 0x102A11xx que lit XenosRecomp. Cet outil les convertit en conteneurs
du format de XenosRecomp (sans table de definitions : les constantes `def` sont copiees par D3D dans la memoire du peripherique
au SetPixelShader/SetVertexShader, donc lues dans l'ombre) puis les traduit comme tools/dash_shaders.py.

Cle d'un shader : XXH3-64 du microcode ; pour un vertex shader, les instructions vfetch listees dans la structure du shader sont mises
a zero avant le hachage (D3D les remplit a l'execution ; les autres octets sont ceux du conteneur). La facade
(native_gpu/src/hud_front.cpp) calcule la meme cle sur la memoire de l'objet D3D.

  hud_shaders.py extract <image.pe> <dossier>            conteneurs 0x102A0E0x -> <dossier>/<adresse>_<VS|PS>.bin (format XenosRecomp)
  hud_shaders.py translate <dossier_bin> <sortie> [racine XenosRecomp]   -> <sortie>/<cle>.spv et <cle>.cont (conteneur converti)

Rien de ce que cet outil produit n'est versionne (derives de microcode Microsoft).
"""
import os
import re
import shutil
import struct
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import dash_shaders as ds  # noqa: E402  (XXH3, XenosRecomp, traduction par lots)


def be(data, offset):
    return struct.unpack(">I", data[offset:offset + 4])[0]


xxh3 = ds.xxh3


def convert(old):
    """Conteneur 0x102A0E0x (virtuel + physique) -> conteneur au format de XenosRecomp (virtuel + physique)."""
    flags, vsize, psize, def_off, ctab_off, shader_off = struct.unpack(">6I", old[:24])
    is_vs = flags & 1
    flags = 0x102A1100 | is_vs  # magie du format de XenosRecomp
    # Table de constantes : de ctab_off jusqu'a la structure du shader (la table de definitions la precede quand elle existe).
    ctab = bytearray(old[ctab_off:shader_off] if 0 < ctab_off < shader_off else b"")
    ctab += b"\0" * (-len(ctab) % 4)
    if ctab and not is_vs:
        # Les constantes booleennes d'un pixel shader sont numerotees 128 + n dans le microcode (b128 : XenosRecomp les retrouve par
        # ce numero) ; l'ancienne table donne n. Le decalage est retire du texte HLSL a la traduction (voir translate).
        count, info = be(ctab, 4 + 12), be(ctab, 4 + 16)
        for i in range(count):
            ci = 4 + info + 20 * i
            if struct.unpack(">H", ctab[ci + 4:ci + 6])[0] == 0:  # Bool
                ctab[ci + 6:ci + 8] = struct.pack(">H", struct.unpack(">H", ctab[ci + 6:ci + 8])[0] + 128)
    ctab = bytes(ctab)
    nwords = (vsize - shader_off) // 4
    o = struct.unpack(">%dI" % nwords, old[shader_off:vsize])
    # Ancienne structure : {field8, fieldC, field10, 0, interpolatorInfo, ?, field18, (VS : nb d'elements, field20, 0 | PS : sorties), ...}
    base = [0, psize, o[0], o[1], o[2], o[4]]
    if is_vs:
        shader = base + [o[6], o[7], o[8]] + list(o[10:])
    else:
        shader = base + [o[6], o[7]] + list(o[8:])
    header_size = 0x24
    new_ctab_off = header_size if ctab else 0
    new_shader_off = new_ctab_off + len(ctab)
    new_vsize = new_shader_off + 4 * len(shader)
    header = struct.pack(">9I", flags, new_vsize, psize, 0, new_ctab_off, 0, new_shader_off, 0, 0)
    return header + ctab + struct.pack(">%dI" % len(shader), *shader) + old[vsize:vsize + psize]


def microcode_key(container):
    """Cle de microcode (voir l'en-tete) d'un conteneur converti ; None si ce n'est pas un conteneur."""
    if len(container) < 0x30 or be(container, 0) & 0xFFFFFFFE != 0x102A1100:
        return None
    flags, vsize = be(container, 0), be(container, 4)
    shader = be(container, 0x18)
    offset, size = be(container, shader), be(container, shader + 4)
    ucode = bytearray(container[vsize + offset:vsize + offset + size])
    if flags & 1:  # vertex shader : instructions vfetch a zero
        field18, count = be(container, shader + 0x18), be(container, shader + 0x1C)
        for i in range(count):
            address = be(container, shader + 0x24 + 4 * (field18 + i)) & 0xFFF
            ucode[address * 12:address * 12 + 12] = bytes(12)
    return xxh3(bytes(ucode))


def extract(image, out):
    import d3d_match as m
    img = m.Image(image)
    d = img.data
    os.makedirs(out, exist_ok=True)
    n = 0
    for o in range(0, len(d) - 0x30, 4):
        if d[o:o + 3] != b"\x10\x2a\x0e" or d[o + 3] & 0xFE != 0:
            continue
        vsize, psize = struct.unpack(">II", d[o + 4:o + 12])
        if not 0x30 <= vsize < 0x10000 or not 0 < psize < 0x80000 or o + vsize + psize > len(d):
            continue
        shader_off = be(d, o + 0x14)
        if not 0x18 <= shader_off < vsize:
            continue
        kind = "VS" if d[o + 3] & 1 else "PS"
        with open(os.path.join(out, f"{img.base + o:08X}_{kind}.bin"), "wb") as f:
            f.write(convert(d[o:o + vsize + psize]))
        n += 1
    print(f"{n} conteneurs -> {out}")


def fix_bools(text):
    """Booleens de pixel shader : le microcode lit b128 + n, la table de constantes donne n (decalage retire du HLSL)."""
    return re.sub(r"#define (\w+) \(1 << (\d+)\)",
                  lambda m: f"#define {m.group(1)} (1 << {int(m.group(2)) - 128 if int(m.group(2)) >= 144 else int(m.group(2))})", text)


def translate(bindir, out, root):
    """Conteneurs convertis (<bindir>/*.bin) -> <out>/<cle>.spv et <cle>.cont (lus par hud_front.cpp)."""
    tmp = tempfile.mkdtemp(prefix="hudsh-")
    try:
        os.makedirs(os.path.join(tmp, "ui"))
        for name in os.listdir(bindir):
            if name.endswith(".bin"):
                shutil.copyfile(os.path.join(bindir, name), os.path.join(tmp, "ui", name))
        fails = ds.batch(tmp, tmp + "-out", root, text_fix=fix_bools)
        os.makedirs(out, exist_ok=True)
        done = 0
        for name in sorted(os.listdir(bindir)):
            if not name.endswith(".bin"):
                continue
            container = open(os.path.join(bindir, name), "rb").read()
            key = microcode_key(container)
            spv = os.path.join(tmp + "-out", "ui_" + name[:-4] + ".spv")
            if key is None or not os.path.isfile(spv):
                print("non traduit :", name, flush=True)
                continue
            shutil.copyfile(spv, os.path.join(out, f"{key:016X}.spv"))
            with open(os.path.join(out, f"{key:016X}.cont"), "wb") as f:
                f.write(container)
            done += 1
        print(f"{done} shaders nommes par cle -> {out} ({len(fails)} échec(s) XenosRecomp/dxc)", flush=True)
        return done, fails
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.rmtree(tmp + "-out", ignore_errors=True)


def main():
    a = sys.argv[1:]
    default_root = ds.xenos_root()
    if len(a) == 3 and a[0] == "extract":
        extract(a[1], a[2])
    elif len(a) >= 3 and a[0] == "translate":
        translate(a[1], a[2], a[3] if len(a) > 3 else default_root)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
