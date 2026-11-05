"""Adapte un shrdres.xzp de XAM récent (2.0.17559, XZP v3) au Dashboard 2.0.6770.

Le XUI du Dashboard 6770 attend des formats plus anciens :
  * paquet XZP v1 (noms de fichiers en UTF-16 big-endian ; le v3 les a en ASCII) ;
  * tables de chaînes XUIS v1 (0x0102, chaînes UTF-16 préfixées par leur longueur) ;
    le XAM récent les fournit en XUIS v2 (0x0202, chaînes UTF-8 terminées par un zéro) ;
  * quelques noms différents (casse : ico_32x_Zone_RR.png / ico_32x_zone_RR.png, ou fichier
    renommé : GScore.png / GScore_white.png) : ajoutés comme alias, sans copier les données.

usage: xzp_alias.py <entrée.xzp> <sortie.xzp> [nouveau=existant ...]
Sans alias explicite, applique la liste DASH_6770 ci-dessous.
"""
import struct
import sys

DASH_6770 = {
    "ico_32x_Zone_Family.png": "ico_32x_zone_Family.png",
    "ico_32x_Zone_Pro.png": "ico_32x_zone_Pro.png",
    "ico_32x_Zone_RR.png": "ico_32x_zone_RR.png",
    "ico_32x_Zone_Underground.png": "ico_32x_zone_Underground.png",
    "ico_64x_Friend.png": "ico_64x_friend.png",
    "GScore.png": "GScore_white.png",
    "ico_64x_DefaultPicture.png": "64_fffe07d10002000000010000.png",
}


def read_xzp(blob):
    assert blob[:4] == b"XUIZ"
    ver, total, flags, table_size = struct.unpack(">IIII", blob[4:20])
    count, = struct.unpack(">H", blob[20:22])
    entries, p = [], 22
    for _ in range(count):
        size, off, nlen = struct.unpack(">IIB", blob[p:p + 9]); p += 9
        if ver >= 3:
            name = blob[p:p + nlen].decode("latin1"); p += nlen
        else:
            name = blob[p:p + nlen * 2].decode("utf-16-be"); p += nlen * 2
        entries.append((name, off, size))
    return flags, entries, blob[22 + table_size:]


def xus_v2_to_v1(data):
    """XUIS v2 (UTF-8, terminées par 0) -> XUIS v1 (UTF-16BE, préfixées par la longueur)."""
    if data[:4] != b"XUIS" or data[4:6] != b"\x02\x02":
        return data
    count, = struct.unpack(">H", data[10:12])
    strings = data[12:].split(b"\0")[:count]
    body = b""
    for s in strings:
        u = s.decode("utf-8").encode("utf-16-be")
        body += struct.pack(">H", len(u) // 2) + u
    return b"XUIS" + b"\x01\x02" + struct.pack(">IH", 12 + len(body), count) + body


def main():
    src, dst = sys.argv[1], sys.argv[2]
    aliases = dict(a.split("=", 1) for a in sys.argv[3:]) or DASH_6770
    flags, entries, data = read_xzp(open(src, "rb").read())

    # Données réécrites (XUIS converties), une seule copie par fichier source.
    new_data, new_off, converted = b"", {}, 0
    out = []
    for name, off, size in entries:
        if off not in new_off:
            chunk = data[off:off + size]
            if name.lower().endswith(".xus"):
                conv = xus_v2_to_v1(chunk)
                converted += conv is not chunk
                chunk = conv
            new_off[off] = (len(new_data), len(chunk))
            new_data += chunk
        out.append((name, *new_off[off]))

    by_name = {n: (o, s) for n, o, s in out}
    for new, old in aliases.items():
        if new in by_name:
            continue
        if old not in by_name:
            print(f"ignoré : {old} absent", file=sys.stderr)
            continue
        out.append((new, *by_name[old]))
        print(f"alias : {new} -> {old}")

    table = b"".join(struct.pack(">IIB", s, o, len(n)) + n.encode("utf-16-be") for n, o, s in out)
    header = b"XUIZ" + struct.pack(">IIII", 1, 22 + len(table) + len(new_data), flags, len(table))
    header += struct.pack(">H", len(out))
    open(dst, "wb").write(header + table + new_data)
    print(f"{len(out)} entrées, {converted} tables XUIS converties en v1")


if __name__ == "__main__":
    main()
