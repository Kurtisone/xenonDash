"""Crée le GPD d'un jeu à partir de ses données de titre (SPA, ressource <TitleID> de son XEX)
et l'ajoute à la liste des jeux joués du profil (FFFE07D1.gpd).

Les succès (noms, descriptions, Gamerscore, images) et l'icône sont ceux du jeu ; la progression
est fictive : --earned 1,2,5 marque ces succès comme obtenus (dates échelonnées).

usage: spa_to_gpd.py <spa.bin> <dossier_profil> [--earned id,id,...] [--lang 1]
ex.  : xex_resource.py default.xex skate.pe 45410813 work/skate.spa
       spa_to_gpd.py work/skate.spa dash/assets/profiles/B13EBABEBABEBABE --earned 1,2,3
"""
import argparse
import os
import struct
import time

NS_META, NS_IMAGE, NS_STRINGS = 1, 2, 3                    # SPA
GPD_ACHIEVEMENT, GPD_IMAGE, GPD_TITLE = 1, 2, 4            # GPD
ACHIEVED, SHOW_UNACHIEVED = 0x00020000, 0x8
TITLE_IMAGE = 0x8000


def read_xdbf(blob):
    assert blob[:4] == b"XDBF", blob[:4]
    _, cap, count, free_cap, _ = struct.unpack(">IIIII", blob[4:24])
    start = 24 + cap * 18 + free_cap * 8
    out = {}
    for i in range(count):
        ns, eid, off, size = struct.unpack(">HQII", blob[24 + i * 18:42 + i * 18])
        out[(ns, eid)] = blob[start + off:start + off + size]
    return out


def write_xdbf(entries):
    table, data = b"", b""
    for (ns, eid), blob in sorted(entries.items()):
        table += struct.pack(">HQII", ns, eid, len(data), len(blob))
        data += blob
    header = b"XDBF" + struct.pack(">IIIII", 0x10000, len(entries), len(entries), 1, 1)
    return header + table + struct.pack(">II", len(data), 0xFFFFFFFF - len(data)) + data


def fourcc(s):
    return int.from_bytes(s.encode(), "big")


def strings(spa, lang):
    blob = spa.get((NS_STRINGS, lang)) or next(v for (ns, _), v in spa.items() if ns == NS_STRINGS)
    count, = struct.unpack(">H", blob[12:14])
    p, out = 14, {}
    for _ in range(count):
        sid, n = struct.unpack(">HH", blob[p:p + 4]); p += 4
        out[sid] = blob[p:p + n].decode("utf-8", "replace"); p += n
    return out


def wstr(s):
    return s.encode("utf-16-be") + b"\0\0"


def filetime(days_ago):
    return int((time.time() - days_ago * 86400 + 11644473600) * 10_000_000)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("spa"); ap.add_argument("profile")
    ap.add_argument("--earned", default=""); ap.add_argument("--lang", type=int, default=1)
    a = ap.parse_args()
    earned = {int(x) for x in a.earned.split(",") if x}

    spa = read_xdbf(open(a.spa, "rb").read())
    s = strings(spa, a.lang)
    xach = spa[(NS_META, fourcc("XACH"))]
    count, = struct.unpack(">H", xach[12:14])
    tid = int(os.path.basename(a.spa).split(".")[0], 16) if all(
        c in "0123456789abcdefABCDEF" for c in os.path.basename(a.spa).split(".")[0]) else None
    xthd = spa.get((NS_META, fourcc("XTHD")))
    if xthd:  # en-tête de titre : magic, version, taille, TitleID
        tid = struct.unpack(">I", xthd[12:16])[0]
    assert tid, "TitleID introuvable (XTHD absent) : nommer le fichier <TitleID>.spa"
    title_name = s.get(TITLE_IMAGE, f"{tid:08X}")

    gpd = {}
    if (NS_IMAGE, TITLE_IMAGE) in spa:
        gpd[(GPD_IMAGE, TITLE_IMAGE)] = spa[(NS_IMAGE, TITLE_IMAGE)]
    gs_total = gs_earned = n_earned = 0
    for i in range(count):
        aid, label, desc, unach, image, gs, _, flags = struct.unpack(">HHHHIHHI", xach[14 + i * 0x24:14 + i * 0x24 + 20])
        got = aid in earned
        f = (flags & 0x7) | SHOW_UNACHIEVED | (ACHIEVED if got else 0)
        t = filetime(len(earned) - sorted(earned).index(aid)) if got else 0
        gpd[(GPD_ACHIEVEMENT, aid)] = struct.pack(">IIIiIQ", 0x1C, aid, image, gs, f, t) + \
            wstr(s.get(label, "")) + wstr(s.get(desc, "")) + wstr(s.get(unach, ""))
        if (NS_IMAGE, image) in spa:
            gpd[(GPD_IMAGE, image)] = spa[(NS_IMAGE, image)]
        gs_total += gs
        if got:
            gs_earned += gs; n_earned += 1
    open(os.path.join(a.profile, f"{tid:08X}.gpd"), "wb").write(write_xdbf(gpd))

    dash_path = os.path.join(a.profile, "FFFE07D1.gpd")
    dash = read_xdbf(open(dash_path, "rb").read()) if os.path.exists(dash_path) else {}
    dash[(GPD_TITLE, tid)] = struct.pack(">IIIIIH6sIQ", tid, count, n_earned, gs_total, gs_earned, 0,
                                         b"\0" * 6, 0, filetime(0)) + wstr(title_name)
    if (NS_IMAGE, TITLE_IMAGE) in spa:
        dash[(GPD_IMAGE, tid)] = spa[(NS_IMAGE, TITLE_IMAGE)]
    open(dash_path, "wb").write(write_xdbf(dash))
    print(f"{tid:08X} {title_name} : {count} succès, {n_earned} obtenus, {gs_earned}/{gs_total} G")


if __name__ == "__main__":
    main()
