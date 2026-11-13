#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Desassemble une fonction (ou une plage) d'une image PE decodee de XEX (PowerPC 64, llvm-mc), avec annotations.

Annote : appels (bl) vers les fonctions de .pdata (sub_XXXXXXXX) ou les imports du noyau, constantes chargees par lis/addi,
chaines ASCII/UTF-16BE pointees. Sert a valider a la main les fonctions D3D retrouvees par d3d_match.py (docs/ngpu_core.md).

usage : ppc_dis.py <image.pe> <debut_hex> [fin_hex] [--xex fichier.xex] [--init dash_init.cpp]
        --init : table d'imports du code genere (generated/<projet>/<projet>_init.cpp) pour nommer les appels du noyau
        sans fin : la fonction de .pdata contenant `debut` (ou 0x100 octets si aucune)
"""
import bisect
import os
import re
import struct
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import d3d_match as m  # noqa: E402


def mc(chunk):
    r = subprocess.run(["llvm-mc", "--disassemble", "-triple=powerpc64-unknown-linux-gnu", "-mcpu=pwr7"],
                       input=" ".join(f"0x{b:02x}" for b in chunk), capture_output=True, text=True)
    return [re.sub(r"\s+", " ", ln.strip()) for ln in r.stdout.splitlines() if ln.startswith("\t") and not ln.strip().startswith(".")]


def cstr(img, a):
    o = a - img.base
    if o < 0 or o >= len(img.data) - 80:
        return ""
    s = img.data[o:o + 80]
    e = s.find(b"\0")
    if e >= 4 and all(32 <= c < 127 for c in s[:e]):
        return '"' + s[:e].decode() + '"'
    w = []
    for i in range(0, 78, 2):
        c = (s[i] << 8) | s[i + 1]
        if c == 0:
            break
        if not 32 <= c < 127:
            return ""
        w.append(chr(c))
    return 'L"' + "".join(w) + '"' if len(w) >= 3 else ""


def main():
    args = [a for a in sys.argv[1:]]
    xex = None
    names = {}
    if "--init" in args:
        i = args.index("--init")
        for ln in open(args[i + 1]):
            mm = re.search(r"\{ *0x([0-9A-Fa-f]{8}), *__imp__(\w+)", ln)
            if mm:
                names[int(mm.group(1), 16)] = mm.group(2)
        del args[i:i + 2]
    if "--xex" in args:
        i = args.index("--xex")
        xex = args[i + 1]
        del args[i:i + 2]
    if len(args) < 2:
        sys.exit(__doc__)
    img = m.Image(args[0], xex)
    start = int(args[1], 16)
    if len(args) > 2:
        end = int(args[2], 16)
    else:
        f = img.func_of(start)
        end = f[0] + f[1] if f else start + 0x100
        if f:
            start = f[0]
    n = (end - start) // 4
    words = img.words(start, n)
    asm = {}

    def fill(off, cnt):
        out = mc(b"".join(struct.pack(">I", w) for w in words[off:off + cnt]))
        if len(out) == cnt:
            for k, x in enumerate(out):
                asm[off + k] = x
        elif cnt == 1:
            asm[off] = "<?>"
        else:
            h = cnt // 2
            fill(off, h)
            fill(off + h, cnt - h)

    for o in range(0, n, 256):
        fill(o, min(256, n - o))
    starts = set(img.fstarts)
    hi = {}
    for k, w in enumerate(words):
        a = start + 4 * k
        op = w >> 26
        rt, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        line, note = asm[k], ""
        if a in starts:
            hi = {}
            print()
        if op == 18:
            t = m.branch_target(a, w)
            nm = names.get(t) or img.thunk_name(t) or (f"thunk{img.thunks[t][1]:#x}" if t in img.thunks else None)
            line = ("bl " if w & 1 else "b ") + (nm or ("sub_%08X" % t if t in starts else "loc_%08X" % t))
        elif op == 16:
            bd = w & 0xFFFC
            bd = bd - 0x10000 if bd & 0x8000 else bd
            line = line.rsplit(" ", 1)[0] + f" loc_{a + bd:08X}"
        elif op == 15:
            if ra == 0:
                hi[rt] = imm << 16
            else:
                hi.pop(rt, None)
        elif op == 24:
            if rt in hi:
                v = hi[rt] | imm
                note = f" ; {v:#x} {cstr(img, v)}"
        elif op in (14, 32, 34, 36, 38, 40, 42, 44, 46, 48, 50):
            if ra in hi:
                simm = imm - 0x10000 if imm & 0x8000 else imm
                v = (hi[ra] + simm) & 0xFFFFFFFF
                note = f" ; {v:#x} {cstr(img, v)}"
            if op in (14, 32, 34, 40, 42) and rt != ra:
                hi.pop(rt, None)
        print(f"{a:08X}  {line}{note}")


main()
