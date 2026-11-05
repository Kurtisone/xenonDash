"""Extrait le système de fichiers flash ("FS root") d'un dump NAND Xbox 360.

usage: nandfs.py <nand.bin> [dossier_sortie]      (sans dossier : liste seulement)

Formats gérés :
  * dump brut AVEC données de contrôle (spare) : pages de 0x200 + 0x10
    (16/64 Mo "small block" Xenon..Falcon, 256/512 Mo "big block" Jasper)
  * dump SANS spare (0x200 par page) : repli heuristique, LBA = bloc physique.
  * eMMC Corona 4 Go : non géré pour l'instant (pas de spare, autre organisation).

Organisation (Free60 + outils de la scène) :
  spare[0xC] & 0x3F  = type de bloc FS ; 0x30 = FS root ("mobiles" 0x31..0x3F)
  numéro de bloc logique (LBA) : small = ((s[1]&0xF)<<8)|s[0] ; big = ((s[2]&0xF)<<8)|s[1]
  séquence (on prend le FS root le plus récent) : small = s[2],s[3],s[4],s[6] ; big = s[5],s[4],s[3]
  FS root (0x4000 octets) : 0x0000-0x1FFF table de chaînage u16 BE (0x1FFF = fin),
                            0x2000-0x3FFF 256 entrées de 0x20 octets :
                              nom[0x16] | bloc de départ u16 BE | taille u32 BE | horodatage u32 BE
Les unités du FS font 0x4000 octets ; en big block un bloc d'effacement (0x20000) en contient 8.

NON TESTÉ sur un vrai dump au moment de l'écriture : vérifier la sortie (noms, tailles, en-têtes
XEX2 des .xex) avant de s'y fier.
"""
import os
import struct
import sys

PAGE, SPARE, FS_UNIT = 0x200, 0x10, 0x4000


def load(path):
    raw = open(path, 'rb').read()
    if len(raw) % (PAGE + SPARE) == 0 and len(raw) % 0x1000000 != 0:
        n = len(raw) // (PAGE + SPARE)
        data = bytearray(); spares = []
        for i in range(n):
            p = raw[i * (PAGE + SPARE):(i + 1) * (PAGE + SPARE)]
            data += p[:PAGE]; spares.append(p[PAGE:])
        return bytes(data), spares
    return raw, None


def detect_block(data_len, spares):
    # big block si > 64 Mo de données, sinon small block
    big = data_len > 64 * 1024 * 1024
    return big, (0x20000 if big else 0x4000)


def spare_info(s, big):
    if big:
        lba = ((s[2] & 0xF) << 8) | s[1]; seq = s[5] | s[4] << 8 | s[3] << 16; bad = s[0] != 0xFF
    else:
        lba = ((s[1] & 0xF) << 8) | s[0]; seq = s[2] | s[3] << 8 | s[4] << 16 | s[6] << 24; bad = s[5] != 0xFF
    return lba, seq, s[0xC] & 0x3F, bad


def parse_root(blk):
    table = struct.unpack('>4096H', blk[:0x2000])
    entries = []
    for i in range(256):
        e = blk[0x2000 + i * 0x20:0x2000 + (i + 1) * 0x20]
        if e[0] in (0x00, 0x05, 0xFF):
            continue
        name = e[:0x16].split(b'\0')[0]
        if not name or any(c < 0x20 or c > 0x7E for c in name):
            continue
        start, size, ts = struct.unpack('>HII', e[0x16:0x20])
        entries.append((name.decode('ascii'), start, size, ts))
    return table, entries


def main():
    data, spares = load(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else None
    big, erase = detect_block(len(data), spares)
    nblocks = len(data) // erase
    print(f"données {len(data):#x} octets, {'big' if big else 'small'} block ({erase:#x}), "
          f"{'avec' if spares else 'SANS'} spare")

    lba_map = {}
    roots = []
    if spares:
        ppb = erase // PAGE
        for b in range(nblocks):
            lba, seq, btype, bad = spare_info(spares[b * ppb], big)
            if bad:
                continue
            lba_map.setdefault(lba, b)
            if btype == 0x30:
                roots.append((seq, b))
    else:
        # repli : chercher un bloc dont la zone d'entrées contient des noms plausibles
        for off in range(0, len(data) - FS_UNIT, FS_UNIT):
            if b'xam.xex\0' in data[off + 0x2000:off + 0x4000]:
                roots.append((0, off // erase))
    if not roots:
        sys.exit("aucun FS root trouvé")
    seq, rb = max(roots)
    print(f"FS root : bloc physique {rb:#x} (séquence {seq:#x}, {len(roots)} candidat(s))")
    root_off = rb * erase
    table, entries = parse_root(data[root_off:root_off + FS_UNIT])

    def unit_offset(u):
        per = erase // FS_UNIT
        phys = lba_map.get(u // per, u // per) if spares else u // per
        return phys * erase + (u % per) * FS_UNIT

    for name, start, size, ts in entries:
        print(f"{size:>10}  {start:#06x}  {name}")
        if not out:
            continue
        buf = bytearray(); u = start; guard = 0
        while len(buf) < size and u < 0x1FFF and guard < 0x2000:
            off = unit_offset(u); buf += data[off:off + FS_UNIT]; u = table[u] if u < len(table) else 0x1FFF; guard += 1
        os.makedirs(out, exist_ok=True)
        open(os.path.join(out, name), 'wb').write(bytes(buf[:size]))
        if len(buf) < size:
            print(f"    ! chaîne tronquée ({len(buf)}/{size})")


if __name__ == '__main__':
    main()
