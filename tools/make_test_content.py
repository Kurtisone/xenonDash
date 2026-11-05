#!/usr/bin/env python3
"""Génère des paquets STFS de test (type `CON `) : en-tête complet (type, TitleID, nom, description,
miniature PNG) et un volume STFS minimal (une table de fichiers + un fichier), lisible par
`tools/stfs.py`. Pas de signature RSA (impossible sans clé de console).

Crée pour Skate (45410813) dans <profil> : deux sauvegardes (type 1) et un téléchargement (type 2),
dans <profil>/Content/<TitleID>/<Type>/.

usage: make_test_content.py <dossier_profil>
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_test_gpd import icon  # noqa: E402

SKATE = 0x45410813
HEADER_SIZE = 0x971A
BLOCK = 0x1000


def wfield(s, size=0x80):
    return s.encode("utf-16-be")[:size - 2].ljust(size, b"\0")


def build_package(content_type, title_id, name, desc, thumb, payload, filename="data.bin",
                  title_name="Skate", publisher="Electronic Arts", profile_id=0):
    h = bytearray(0xA000)
    h[0:4] = b"CON "
    struct.pack_into(">I", h, 0x340, HEADER_SIZE)
    struct.pack_into(">I", h, 0x344, content_type)
    struct.pack_into(">I", h, 0x348, 1)            # version des métadonnées
    struct.pack_into(">I", h, 0x360, title_id)
    h[0x364] = 2                                    # plate-forme Xbox 360
    h[0x365] = 1                                    # type d'objet : dossier
    h[0x366] = 1                                    # numéro de disque
    struct.pack_into(">Q", h, 0x371, profile_id)    # propriétaire (0 = contenu partagé)
    for i in range(18):                             # même texte pour les 18 langues
        h[0x411 + i * 0x80:0x411 + (i + 1) * 0x80] = wfield(name)
        h[0xD11 + i * 0x80:0xD11 + (i + 1) * 0x80] = wfield(desc)
    h[0x1611:0x1611 + 0x80] = wfield(publisher)
    h[0x1691:0x1691 + 0x80] = wfield(title_name)
    struct.pack_into(">I", h, 0x1712, len(thumb))
    struct.pack_into(">I", h, 0x1716, len(thumb))
    h[0x171A:0x171A + len(thumb)] = thumb
    h[0x571A:0x571A + len(thumb)] = thumb

    # Volume STFS : bloc 0 = table de fichiers, bloc 1 = données du fichier.
    blocks = []
    ft = bytearray(BLOCK)
    nm = filename.encode()
    ft[0:len(nm)] = nm
    ft[0x28] = len(nm) | 0x40                       # longueur + bloc consécutifs
    nblk = max(1, (len(payload) + BLOCK - 1) // BLOCK)
    ft[0x29:0x2C] = nblk.to_bytes(3, "little")      # blocs alloués
    ft[0x2C:0x2F] = nblk.to_bytes(3, "little")
    ft[0x2F:0x32] = (1).to_bytes(3, "little")       # premier bloc
    struct.pack_into(">h", ft, 0x32, -1)            # dossier parent : racine
    struct.pack_into(">I", ft, 0x34, len(payload))
    blocks.append(bytes(ft))
    padded = payload.ljust(nblk * BLOCK, b"\0")
    for i in range(nblk):
        blocks.append(padded[i * BLOCK:(i + 1) * BLOCK])
    # Table de hachage de niveau 0 (SHA-1 de chaque bloc + 4 octets d'état).
    ht = bytearray(BLOCK)
    for i, b in enumerate(blocks):
        ht[i * 24:i * 24 + 20] = hashlib.sha1(b).digest()
        ht[i * 24 + 20] = 0x80
    vol = bytearray(h)
    vol += ht
    for b in blocks:
        vol += b
    # Descripteur de volume (0x379) : taille 0x24, séparation des blocs, table de fichiers.
    vol[0x379] = 0x24
    vol[0x37B] = 1
    struct.pack_into("<H", vol, 0x37C, 1)           # nombre de blocs de la table
    vol[0x37E:0x381] = (0).to_bytes(3, "little")    # numéro du bloc de la table
    vol[0x381:0x395] = hashlib.sha1(bytes(ht)).digest()
    struct.pack_into(">II", vol, 0x395, len(blocks), 0)
    struct.pack_into(">Q", vol, 0x34C, len(payload))
    vol[0x32C:0x340] = hashlib.sha1(bytes(vol[0x344:HEADER_SIZE])).digest()
    return bytes(vol)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    profile = sys.argv[1]
    items = [
        (0x00000001, SKATE, "save_career", "Skate - Carrière",
         "Carrière : San Vanelona, 38 % terminé.", ((60, 120, 200), "disque"), b"SAVE-CAREER" * 300),
        (0x00000001, SKATE, "save_freeskate", "Skate - Session libre",
         "Spots débloqués et défis personnels.", ((200, 120, 60), "carre"), b"SAVE-FREE" * 200),
        (0x00000002, SKATE, "dlc_pack1", "Skate - Pack de planches",
         "Dix planches supplémentaires.", ((60, 170, 90), "losange"), b"DLC-PACK1" * 1000),
    ]
    for ctype, tid, fn, name, desc, (hue, motif), payload in items:
        d = os.path.join(profile, "Content", f"{tid:08X}", f"{ctype:08X}")
        os.makedirs(d, exist_ok=True)
        # Noms de fichier de type console : 16 chiffres hexadécimaux (hash).
        real = hashlib.sha1(fn.encode()).hexdigest()[:16].upper()
        thumb = png_bytes(hue, motif)
        with open(os.path.join(d, real), "wb") as f:
            f.write(build_package(ctype, tid, name, desc, thumb, payload,
                                      profile_id=0 if ctype == 2 else int(os.path.basename(profile.rstrip("/")), 16)))
        print(os.path.join(d, real), name)


def png_bytes(hue, motif):
    return icon(hue, motif)  # PNG 64x64 (make_test_gpd)


if __name__ == "__main__":
    main()
