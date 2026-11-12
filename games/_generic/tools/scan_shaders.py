# SPDX-License-Identifier: GPL-3.0-or-later
"""Recolte generique des conteneurs de microcode Xenos (magie 0x102A11xx) : balayage binaire de tous les fichiers du jeu
(archives non compressees comprises), de l'image decodee du XEX et du dossier des shaders manques (journal d'execution).

  scan_shaders.py <dossier du jeu | -> <image decodee> <sortie> [<dossier shaders manques>]

Un conteneur est accepte quand sa taille (virtuelle + physique) tient dans le fichier et que l'un des deux champs suivants de
l'en-tete vaut 0x24 (taille de l'en-tete), comme les conteneurs de la XDK. Les archives compressees (RefPack, zlib...) ne sont pas
decompressees : leurs shaders arrivent par la boucle « jouer -> reconstruire » (la facade ecrit le microcode manquant).
"""
import hashlib
import mmap
import os
import struct
import sys

MAGIC = b"\x10\x2a\x11"


def containers(buf):
    i = 0
    n = len(buf)
    while True:
        i = buf.find(MAGIC, i)
        if i < 0 or i + 0x28 > n:
            return
        flags, vsize, psize = struct.unpack_from(">III", buf, i)
        size = vsize + psize
        if ((flags & 0xFFFFFF00) == 0x102A1100 and flags & 0xFE == 0 and 0 < size <= n - i and size < 0x400000
                and 0x24 in struct.unpack_from(">II", buf, i + 12)):
            yield i, bytes(buf[i:i + size])
            i += size
        else:
            i += 1


def main():
    game, image, out = sys.argv[1:4]
    missing = sys.argv[4] if len(sys.argv) > 4 else None
    os.makedirs(out, exist_ok=True)
    seen = set()
    stats = {"files": 0, "bytes": 0, "image": 0, "game": 0, "missing": 0}

    def emit(data, tag):
        d = hashlib.sha1(data).hexdigest()
        if d in seen:
            return
        seen.add(d)
        kind = "ps" if (data[3] & 1) == 0 else "vs"
        with open(os.path.join(out, f"{tag}_{d[:16]}_{kind}.bin"), "wb") as f:
            f.write(data)
        stats[tag] += 1

    # image decodee du XEX
    with open(image, "rb") as f:
        for _, data in containers(f.read()):
            emit(data, "image")
    # fichiers du jeu
    if game and game != "-":
        for dp, _, files in os.walk(game):
            for name in sorted(files):
                p = os.path.join(dp, name)
                try:
                    size = os.path.getsize(p)
                    if size < 0x30:
                        continue
                    with open(p, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
                        stats["files"] += 1
                        stats["bytes"] += size
                        for _, data in containers(m):
                            emit(data, "game")
                except (OSError, ValueError):
                    continue
    # shaders manques (microcode ecrit par la facade pendant une partie)
    if missing and os.path.isdir(missing):
        for name in sorted(os.listdir(missing)):
            if name.endswith(".bin"):
                emit(open(os.path.join(missing, name), "rb").read(), "missing")
    print(f"{len(seen)} conteneurs : {stats['image']} dans l'image, {stats['game']} dans {stats['files']} fichiers "
          f"({stats['bytes'] / 1e6:.0f} Mo), {stats['missing']} manques ajoutes")


if __name__ == "__main__":
    main()
