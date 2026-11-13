#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decouverte statique des fonctions qu'aucun appel direct n'atteint (recompilateur generique, lot B2).

`rexglue codegen` connait deja toutes les fonctions de `.pdata` (verifie : toutes les entrees de .pdata de Skate et de Trials Fusion
sont dans le code genere) et celles qu'un `bl` atteint. Lui echappent les fonctions feuilles sans donnees de deroulement qui ne sont
appelees que par pointeur (vtables, tables de rappels, adresses construites par lis/addi). La table `.reloc` n'est pas exploitable
(section liberee : son contenu n'est pas dans l'image decodee).

Candidats
  data     mot aligne d'une section de donnees dont la valeur tombe dans .text ;
  code     adresse construite dans le code par `lis rX,hi` puis `addi|ori rY,rX,lo` (suivi lineaire des registres) ;
Filtre « debut plausible » (mesure sur Skate et Trials Fusion, voir docs/recompilateur_generique.md)
  - le mot a cette adresse n'est ni nul ni lui-meme une adresse de code (table de saut absolue apres un `bctr`) ;
  - ce n'est pas un thunk d'import du XEX (table d'imports) : en faire une fonction remplace l'import du noyau par un STUB
    renvoyant 0 (cause du plantage de Skate generique pur, lot B2 : KeTlsSetValue, puis recursion infinie) ;
  - hors du corps de toute fonction de .pdata (etendues exactes) : l'instruction precedente est terminale (blr, bctr, b, bourrage nul)
    ou un `bl` (fonction qui suit un appel sans retour) ;
  - dans le corps d'une fonction de .pdata : seulement si la reference est une donnee et que l'instruction precedente est `b` ou `bl`
    (reprises de gestionnaires d'exceptions, blocs partages) ; les references du code y sont des bases de `switch` : ecartees.

Usage
  func_discover.py <image.bin|default.xex> [--truth functions.toml ...] [--init <projet>_init.cpp] [--list]
    --truth : listes de reference (functions.toml d'un port) -> rappel ; --init : fonctions d'un codegen complet -> precision.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d_match

NON_DATA = (".text", ".pdata", ".reloc", ".idata", ".edata", ".XBLD")


def _class(i):
    if i == 0x4E800020:
        return "blr"
    if i == 0x4E800420:
        return "bctr"
    if i == 0:
        return "0"
    if i >> 26 == 18:
        return "bl" if i & 1 else "b"
    return "other"


def _exec_sections(img):
    """Noms des sections executables (drapeau IMAGE_SCN_MEM_EXECUTE) : exclues du balayage des donnees."""
    d = img.data
    pe = struct.unpack("<I", d[0x3C:0x40])[0]
    nsec = struct.unpack("<H", d[pe + 6:pe + 8])[0]
    optsz = struct.unpack("<H", d[pe + 20:pe + 22])[0]
    out = set()
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        if struct.unpack("<I", d[o + 36:o + 40])[0] & 0x20000020:
            out.add(d[o:o + 8].rstrip(b"\0").decode("latin1"))
    return out


def references(img):
    """({cible: adresse de la donnee}, {cible: adresse de l'instruction}) : adresses de .text prises par des donnees / par le code."""
    d, base = img.data, img.base
    lo, hi = base + img.text_lo, base + img.text_hi
    skip = set(NON_DATA) | _exec_sections(img)
    data = {}
    for name, (va, vs) in img.sections.items():
        if name in skip:
            continue
        n = max(0, min(vs, len(d) - va)) // 4
        for k, v in enumerate(struct.unpack(f">{n}I", d[va:va + 4 * n])):
            if lo <= v < hi and not v & 3:
                data.setdefault(v, base + va + 4 * k)
    code = {}
    reg = {}
    starts = set(img.fstarts)
    for k, i in enumerate(img.text_words):
        op = i >> 26
        a = lo + 4 * k
        if a in starts:
            reg = {}
        if op == 15 and not (i >> 16) & 31:                       # lis rD,hi
            reg[(i >> 21) & 31] = (i & 0xFFFF) << 16
        elif op == 14 and (i >> 16) & 31 in reg:                  # addi rD,rA,lo
            imm = i & 0xFFFF
            v = (reg[(i >> 16) & 31] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            if lo <= v < hi and not v & 3:
                code.setdefault(v, a)
            reg.pop((i >> 21) & 31, None)
        elif op == 24 and (i >> 21) & 31 in reg:                  # ori rA,rS,lo
            v = reg[(i >> 21) & 31] | (i & 0xFFFF)
            if lo <= v < hi and not v & 3:
                code.setdefault(v, a)
        elif op == 18 or i in (0x4E800020, 0x4E800420):           # appel ou fin : registres volatils perdus
            reg = {}
        elif op in (14, 15, 32, 34, 40, 42):                      # registre de destination reecrit
            reg.pop((i >> 21) & 31, None)
        elif op in (20, 21, 24, 25):
            reg.pop((i >> 16) & 31, None)
    return data, code


def discover(img):
    """{adresse: (nature, reference)} : debuts de fonction plausibles absents de .pdata. Nature : data, code, data-in."""
    lo, hi = img.base + img.text_lo, img.base + img.text_hi
    w = img.text_words
    starts = set(img.fstarts)
    data, code = references(img)
    out = {}
    for kind, refs in (("data", data), ("code", code)):
        for a, ref in refs.items():
            if a in starts or a in out or a - 4 < lo:
                continue
            if a in img.thunks:          # thunk d'import du noyau : l'appel doit rester un import (sinon STUB renvoyant 0)
                continue
            first = w[(a - lo) // 4]
            if first == 0 or (lo <= first < hi and not first & 3):
                continue
            pred = _class(w[(a - 4 - lo) // 4])
            f = img.func_of(a)
            if f is None:
                if pred != "other":
                    out[a] = (kind, ref)
            elif kind == "data" and pred in ("b", "bl"):
                out[a] = ("data-in", ref)
    return out


def read_truth(paths):
    t = set()
    for p in paths:
        for line in open(p):
            m = re.match(r'\s*"?0x([0-9A-Fa-f]{8})"?\s*=\s*\{', line)
            if m:
                t.add(int(m.group(1), 16))
    return t


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source")
    ap.add_argument("--truth", action="append", default=[])
    ap.add_argument("--init", help="<projet>_init.cpp d'un codegen complet (toutes les fonctions)")
    ap.add_argument("--list", action="store_true")
    args = ap.parse_args()
    img = d3d_match.load_image(args.source)
    found = discover(img)
    kinds = {}
    for a, (k, _) in found.items():
        kinds.setdefault(k, set()).add(a)
    print(f".pdata : {len(img.fstarts)} fonctions ; candidats hors .pdata : {len(found)} "
          + ", ".join(f"{k} {len(v)}" for k, v in sorted(kinds.items())))
    full = None
    if args.init:
        full = set(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-Fa-f]{8}), \w+ \}", open(args.init).read()))
        for k, v in sorted(kinds.items()):
            print(f"  precision {k:8s} : {len(v & full)}/{len(v)} sont des fonctions du codegen complet ({len(v - full)} hors liste)")
    if args.truth:
        t = read_truth(args.truth) - set(img.fstarts)
        print(f"  rappel : {len(t & set(found))}/{len(t)} entrees de reference hors .pdata retrouvees")
        if args.list:
            for a in sorted(t - set(found)):
                print(f"    manque 0x{a:08X}")
    elif args.list:
        for a in sorted(found):
            print(f"0x{a:08X} = {{}}  # {found[a][0]} 0x{found[a][1]:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
