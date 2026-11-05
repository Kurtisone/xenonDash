"""Liste les imports (bibliothèque, ordinal) d'un XEX.
Entrées : le .xex (en-tête en clair) et l'image produite par xexinfo, où XenonUtils a
byte-swappé les enregistrements de variables d'import (type 0) en little-endian."""
import sys, struct
xex = open(sys.argv[1],'rb').read(); pe = open(sys.argv[2],'rb').read(); base = int(sys.argv[3],16)
nhdr = struct.unpack('>I', xex[0x14:0x18])[0]
opt = dict(struct.unpack('>II', xex[0x18+i*8:0x20+i*8]) for i in range(nhdr))
off = opt[0x000103FF]
_, strsz, nlib = struct.unpack('>III', xex[off:off+12])
names = [n.decode() for n in xex[off+12:off+12+strsz].split(b'\0') if n]
p = off+12+strsz
for _ in range(nlib):
    size, = struct.unpack('>I', xex[p:p+4]); nameidx, nimp = struct.unpack('>HH', xex[p+0x24:p+0x28])
    for j in range(nimp):
        a, = struct.unpack('>I', xex[p+0x28+j*4:p+0x2C+j*4])
        rec, = struct.unpack('<I', pe[a-base:a-base+4])
        if rec >> 24 == 0: print(f"{names[nameidx]}\t{rec & 0xFFFF}")
    p += size
