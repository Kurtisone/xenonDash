#!/usr/bin/env python3
"""Comparaison image par image de deux captures 1280x720 (natif vs oracle xenos) : PSNR, écart moyen, par zone.

usage :
  framecmp.py A B [--grid 8x6] [--png-diff diff.png] [--json]      compare deux images (PNG, PPM ou XWD d'Xvfb)
  framecmp.py --convert A out.png                                  convertit PPM/XWD en PNG
  framecmp.py --montage out.png A B C ...                          planche contact (réduction 1/3, 3 colonnes)
  framecmp.py --noise A1 A2 [B]                                    plancher de bruit : écart entre deux exécutions de l'oracle
                                                                   (et écart de B à A1 pour la comparaison)

Sans numpy ni PIL (bibliothèque standard seulement). Métriques : écart absolu moyen (0-255, moyenne RGB), PSNR global, et
grille de zones (par défaut 8x6) avec l'écart moyen par zone, la zone la pire, et la part de pixels à écart > 32.
Le fond du Dashboard est animé : la référence honnête est l'écart entre deux exécutions de l'oracle (--noise).
"""
import json
import struct
import sys
import zlib


def read_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'pas un PNG'
    pos, idat, w = 8, b'', None
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8])
        c = d[pos + 8:pos + 8 + n]
        if t == b'IHDR':
            w, h, bd, ct = struct.unpack('>IIBB', c[:10])
            assert bd == 8 and ct in (2, 6), 'PNG 8 bits RGB/RGBA seulement'
            bpp = 3 if ct == 2 else 4
        elif t == b'IDAT':
            idat += c
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        if bpp == 4:
            for i in range(0, stride, 4):
                out += line[i:i + 3]
        else:
            out += line
    return w, h, bytes(out)


def read_ppm(path):
    d = open(path, 'rb').read()
    parts = d.split(None, 4)
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[len(d) - w * h * 3:]


def read_xwd(path):
    d = open(path, 'rb').read()
    hd = struct.unpack('>25I', d[:100])
    hsize, w, h, bpl, ncol, bpp = hd[0], hd[4], hd[5], hd[12], hd[19], hd[11]
    off = hsize + ncol * 12
    out = bytearray()
    for y in range(h):
        line = d[off + y * bpl: off + y * bpl + w * (bpp // 8)]
        for x in range(w):
            out += bytes((line[x * 4 + 2], line[x * 4 + 1], line[x * 4]))
    return w, h, bytes(out)


def load(path):
    d = open(path, 'rb').read(8)
    if d[:8] == b'\x89PNG\r\n\x1a\n':
        return read_png(path)
    if d[:2] == b'P6':
        return read_ppm(path)
    return read_xwd(path)


def write_png(path, w, h, rgb):
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))


def montage(paths, out, scale=3, cols=3):
    """Planche contact : images réduites par `scale` (moyenne par blocs), `cols` colonnes."""
    ims = [load(p) for p in paths]
    w, h = ims[0][0] // scale, ims[0][1] // scale
    rows = (len(ims) + cols - 1) // cols
    W, H = w * cols, h * rows
    buf = bytearray(W * H * 3)
    for n, (iw, ih, d) in enumerate(ims):
        ox, oy = (n % cols) * w, (n // cols) * h
        for y in range(h):
            for x in range(w):
                i = ((y * scale) * iw + x * scale) * 3
                o = ((oy + y) * W + ox + x) * 3
                buf[o:o + 3] = d[i:i + 3]
    write_png(out, W, H, bytes(buf))


def compare(a, b, gx=8, gy=6):
    (w, h, A), (w2, h2, B) = a, b
    assert (w, h) == (w2, h2), 'dimensions différentes'
    zs = [[0, 0, 0] for _ in range(gx * gy)]  # somme des écarts, somme des carrés, pixels > 32
    zn = [0] * (gx * gy)
    tot = sq = big = 0
    diff = bytearray(w * h * 3)
    for y in range(h):
        zy = y * gy // h
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            d0, d1, d2 = abs(A[i] - B[i]), abs(A[i + 1] - B[i + 1]), abs(A[i + 2] - B[i + 2])
            m = (d0 + d1 + d2) / 3.0
            s = d0 * d0 + d1 * d1 + d2 * d2
            z = zy * gx + x * gx // w
            zs[z][0] += m
            zs[z][1] += s
            zn[z] += 1
            if m > 32:
                zs[z][2] += 1
                big += 1
            tot += m
            sq += s
            v = min(255, int(m * 4))
            diff[i] = diff[i + 1] = diff[i + 2] = v
    n = w * h
    mse = sq / (3.0 * n)
    import math
    psnr = 99.0 if mse == 0 else 10 * math.log10(255.0 * 255.0 / mse)
    zones = [{'zone': (k % gx, k // gx), 'ecart_moyen': round(zs[k][0] / zn[k], 2), 'psnr': round(99.0 if zs[k][1] == 0 else 10 * math.log10(255.0 * 255.0 * 3 * zn[k] / zs[k][1]), 2),
              'part_gt32': round(zs[k][2] / zn[k], 4)} for k in range(gx * gy)]
    return {'ecart_moyen': round(tot / n, 3), 'psnr_db': round(psnr, 2), 'part_pixels_gt32': round(big / n, 4), 'zones': zones}, (w, h, bytes(diff))


def show(name, r, gx, gy):
    print('%s : écart moyen %.3f/255, PSNR %.2f dB, pixels à écart>32 : %.2f %%' % (name, r['ecart_moyen'], r['psnr_db'], 100 * r['part_pixels_gt32']))
    print('  écart moyen par zone (%dx%d, ligne = haut vers bas) :' % (gx, gy))
    for j in range(gy):
        print('   ' + ' '.join('%6.1f' % r['zones'][j * gx + i]['ecart_moyen'] for i in range(gx)))
    worst = max(r['zones'], key=lambda z: z['ecart_moyen'])
    print('  pire zone : %s écart %.1f' % (worst['zone'], worst['ecart_moyen']))


def main(av):
    gx, gy, png, js = 8, 6, None, False
    args = []
    i = 0
    while i < len(av):
        if av[i] == '--grid':
            gx, gy = map(int, av[i + 1].split('x')); i += 2
        elif av[i] == '--png-diff':
            png = av[i + 1]; i += 2
        elif av[i] == '--json':
            js = True; i += 1
        else:
            args.append(av[i]); i += 1
    if args and args[0] == '--montage':
        montage(args[2:], args[1]); return 0
    if args and args[0] == '--convert':
        w, h, rgb = load(args[1]); write_png(args[2], w, h, rgb); return 0
    noise = bool(args and args[0] == '--noise')
    if noise:
        args = args[1:]
    if len(args) < 2:
        print(__doc__); return 2
    a, b = load(args[0]), load(args[1])
    r, d = compare(a, b, gx, gy)
    out = {'ab': r}
    if png:
        write_png(png, *d)
    if noise and len(args) > 2:
        c = load(args[2])
        r2, _ = compare(a, c, gx, gy)
        out['a_vs_c'] = r2
    if js:
        print(json.dumps(out))
    else:
        show('A vs B' if not noise else 'bruit (oracle vs oracle)', r, gx, gy)
        if 'a_vs_c' in out:
            show('A vs C (natif vs oracle)', out['a_vs_c'], gx, gy)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
