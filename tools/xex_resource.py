"""Extrait une ressource nommée (en-tête XEX 0x2FF) de l'image décompressée d'un XEX.
L'image vient de tools/xexinfo (xexinfo <fichier.xex> <image.pe>).

usage: xex_resource.py <fichier.xex> <image.pe> [<nom> <sortie>]   (sans nom : liste)
ex.  : xex_resource.py '$flash_xam.xex' work/xam17559.pe shrdres work/shrdres_17559.xzp
"""
import struct
import sys

xex = open(sys.argv[1], "rb").read()
pe = open(sys.argv[2], "rb").read()
n, = struct.unpack(">I", xex[0x14:0x18])
opt = dict(struct.unpack(">II", xex[0x18 + i * 8:0x20 + i * 8]) for i in range(n))
base = opt[0x10201]
v = opt[0x2FF]
size, = struct.unpack(">I", xex[v:v + 4])
for j in range((size - 4) // 16):
    e = xex[v + 4 + j * 16:v + 20 + j * 16]
    name = e[:8].rstrip(b"\0").decode("latin1")
    addr, length = struct.unpack(">II", e[8:16])
    if len(sys.argv) < 5:
        print(f"{name:10} {addr:#010x} {length:#x}")
    elif name == sys.argv[3]:
        open(sys.argv[4], "wb").write(pe[addr - base:addr - base + length])
        print(f"{name} -> {sys.argv[4]} ({length} octets)")
        break
else:
    if len(sys.argv) >= 5:
        sys.exit(f"ressource {sys.argv[3]} introuvable")
