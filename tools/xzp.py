"""Liste / extrait les fichiers d'un paquet XUI (XUIZ v1 et v3, .xzp) ou des ressources XZP d'un XEX.
usage: xzp.py <fichier.xzp> [dossier_sortie]
       xzp.py --pe <image.pe> <base_hex> <adresse_hex> <taille_hex> [dossier_sortie]"""
import os, struct, sys

def parse(blob):
    assert blob[:4] == b'XUIZ', blob[:4]
    ver, size, flags, table_size = struct.unpack('>IIII', blob[4:20])
    count, = struct.unpack('>H', blob[20:22])
    p, files = 22, []
    for _ in range(count):
        fsize, foff, nlen = struct.unpack('>IIB', blob[p:p + 9]); p += 9
        if ver >= 3:  # v3 : noms en ASCII
            name = blob[p:p + nlen].decode('latin1'); p += nlen
        else:         # v1 : noms en UTF-16 big-endian
            name = blob[p:p + nlen * 2].decode('utf-16-be'); p += nlen * 2
        files.append((name, foff, fsize))
    data_start = 22 + table_size
    return ver, [(n, data_start + o, s) for n, o, s in files]

def main():
    if sys.argv[1] == '--pe':
        d = open(sys.argv[2], 'rb').read(); base = int(sys.argv[3], 16)
        a, s = int(sys.argv[4], 16), int(sys.argv[5], 16)
        blob = d[a - base:a - base + s]; out = sys.argv[6] if len(sys.argv) > 6 else None
    else:
        blob = open(sys.argv[1], 'rb').read(); out = sys.argv[2] if len(sys.argv) > 2 else None
    ver, files = parse(blob)
    for name, off, size in files:
        print(f'{size:>8}  {name}')
        if out:
            dst = os.path.join(out, name.replace('\\', '/')); os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            open(dst, 'wb').write(blob[off:off + size])

main()
