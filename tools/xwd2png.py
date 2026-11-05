"""Convertit le framebuffer XWD d'Xvfb (-fbdir) en PNG. usage: xwd2png.py <Xvfb_screen0> <out.png>"""
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
h = struct.unpack('>25I', d[:100])
hsize, w, ht, bpl, ncolors = h[0], h[4], h[5], h[12], h[19]
bpp = h[11]
off = hsize + ncolors * 12
rows = []
for y in range(ht):
    line = d[off + y*bpl: off + y*bpl + w*(bpp//8)]
    px = bytearray()
    for x in range(w):
        b, g, r = line[x*4], line[x*4+1], line[x*4+2]  # 32 bpp, little-endian BGRX
        px += bytes((r, g, b))
    rows.append(b'\0' + bytes(px))
def chunk(t, c): return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, ht, 8, 2, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
open(sys.argv[2], 'wb').write(png)
