"""Crée un profil de démonstration (fichiers GPD au format XDBF) : deux jeux fictifs,
leurs succès (certains obtenus) et leurs images, pour tester les écrans du Dashboard
sans profil de console. Un vrai profil s'utilise de la même façon : copier ses GPD
(FFFE07D1.gpd + <TitleID>.gpd) dans dash/assets/profiles/<XUID>/.

usage: make_test_gpd.py <dossier_profil>
"""
import os
import struct
import sys
import time
import zlib

NS_ACHIEVEMENT, NS_IMAGE, NS_TITLE = 1, 2, 4
DASH_TITLE = 0xFFFE07D1
ACHIEVED = 0x00020000  # obtenu (hors ligne)
SHOW_UNACHIEVED = 0x8


def filetime(days_ago):
    return int((time.time() - days_ago * 86400 + 11644473600) * 10_000_000)


def png(rgb_fn, size=64):
    raw = b""
    for y in range(size):
        raw += b"\0" + b"".join(bytes(rgb_fn(x / size, y / size)) for x in range(size))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def icon(hue, motif):
    def f(u, v):
        d = ((u - .5) ** 2 + (v - .5) ** 2) ** .5
        on = {"disque": d < .32, "carre": abs(u - .5) < .25 and abs(v - .5) < .25,
              "losange": abs(u - .5) + abs(v - .5) < .35, "anneau": .2 < d < .34}[motif]
        base = [int(c * (0.55 + 0.45 * (1 - v))) for c in hue]
        return [255, 255, 255] if on else base
    return png(f)


def xdbf(entries):
    """entries : liste de (espace, id, données)."""
    table, data = b"", b""
    for ns, eid, blob in entries:
        table += struct.pack(">HQII", ns, eid, len(data), len(blob))
        data += blob
    header = b"XDBF" + struct.pack(">IIIII", 0x10000, len(entries), len(entries), 1, 1)
    free = struct.pack(">II", len(data), 0xFFFFFFFF - len(data))
    return header + table + free + data


def wstr(s):
    return s.encode("utf-16-be") + b"\0\0"


def achievement(aid, image_id, gamerscore, earned_days, name, unlocked, locked):
    flags = SHOW_UNACHIEVED | 1 | (ACHIEVED if earned_days is not None else 0)
    t = filetime(earned_days) if earned_days is not None else 0
    return struct.pack(">IIIiIQ", 0x1C, aid, image_id, gamerscore, flags, t) + \
        wstr(name) + wstr(unlocked) + wstr(locked)


GAMES = [
    (0x44454D01, "Démo Arcade", (40, 120, 200), [
        ("Premier pas", "Terminer le didacticiel.", "Terminez le didacticiel.", 10, 30),
        ("Série de trois", "Gagner trois parties d'affilée.", "Gagnez trois parties d'affilée.", 20, 12),
        ("Collectionneur", "Trouver tous les bonus d'un niveau.", "Trouvez tous les bonus.", 30, None),
        ("Sans faute", "Finir un niveau sans être touché.", "Finissez un niveau intact.", 40, 3),
        ("Maître", "Atteindre le rang maximal.", "Atteignez le rang maximal.", 100, None),
    ]),
    (0x44454D02, "Démo Course", (200, 70, 40), [
        ("Sur la grille", "Terminer une première course.", "Terminez une course.", 15, 2),
        ("Podium", "Finir dans les trois premiers.", "Finissez sur le podium.", 25, None),
        ("Tour parfait", "Boucler un tour sans sortie de piste.", "Bouclez un tour parfait.", 35, None),
    ]),
]
MOTIFS = ["disque", "carre", "losange", "anneau"]


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    dash_entries = []
    for i, (tid, name, hue, achs) in enumerate(GAMES):
        entries, earned, gs_total, gs_earned = [], 0, 0, 0
        entries.append((NS_IMAGE, 0x8000, icon(hue, "anneau")))
        for n, (aname, unl, lck, gs, days) in enumerate(achs, 1):
            entries.append((NS_ACHIEVEMENT, n, achievement(n, n, gs, days, aname, unl, lck)))
            entries.append((NS_IMAGE, n, icon(hue, MOTIFS[n % len(MOTIFS)])))
            gs_total += gs
            if days is not None:
                earned += 1
                gs_earned += gs
        open(os.path.join(out, f"{tid:08X}.gpd"), "wb").write(xdbf(entries))
        title = struct.pack(">IIIIIH6sIQ", tid, len(achs), earned, gs_total, gs_earned, 0,
                            b"\0" * 6, 0, filetime(i + 1)) + wstr(name)
        dash_entries.append((NS_TITLE, tid, title))
        dash_entries.append((NS_IMAGE, tid, icon(hue, "anneau")))
        print(f"{tid:08X} {name} : {earned}/{len(achs)} succès, {gs_earned}/{gs_total} G")
    open(os.path.join(out, "FFFE07D1.gpd"), "wb").write(xdbf(dash_entries))


if __name__ == "__main__":
    main()
