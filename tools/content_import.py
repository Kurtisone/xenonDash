#!/usr/bin/env python3
"""Lecture de l'en-tête d'un paquet STFS (CON / LIVE / PIRS) et import dans un profil.

Sur la console, le contenu est dans Content/<XUID ou 0>/<TitleID>/<ContentType>/<fichier>.
Le Dashboard natif lit le même arbre dans dashboards/<version>/assets/profiles/<XUID>/Content/.

Offsets de l'en-tête (vérifiés sur les paquets de test et sur la documentation Free60) :
  0x000  magique 'CON ' / 'LIVE' / 'PIRS'
  0x340  taille de l'en-tête (BE32)        0x344  type de contenu (BE32)
  0x34C  taille du contenu (BE64)          0x360  TitleID (BE32)
  0x411  nom affiché, 18 langues x 0x80 octets UTF-16BE
  0xD11  description, 18 langues x 0x80 octets UTF-16BE
  0x1611 éditeur (0x80)                    0x1691 nom du titre (0x80)
  0x1711 indicateurs de transfert          0x1712 taille de la miniature (BE32)
  0x1716 taille de la miniature du titre   0x171A miniature PNG (0x4000 max)
  0x571A miniature du titre (0x4000 max)

usage:
  content_import.py info <paquet>...
  content_import.py import <profil> <paquet>... [--common] [--title ID] [--type 0xT]
      copie le paquet vers <profil>/Content/<TitleID>/<Type>/<nom>   (--common : profil 0000000000000000)
  content_import.py list <profil>
"""
import argparse
import os
import shutil
import struct
import sys

MAGICS = (b"CON ", b"LIVE", b"PIRS")
TYPES = {
    0x00000001: "sauvegarde", 0x00000002: "contenu téléchargé", 0x00000003: "éditeur",
    0x00001000: "titre Xbox 360", 0x00004000: "jeu installé", 0x00007000: "Games on Demand",
    0x00009000: "objet d'avatar", 0x00010000: "profil", 0x00020000: "image de joueur",
    0x00030000: "thème", 0x00040000: "fichier de cache", 0x00050000: "stockage",
    0x00060000: "sauvegarde Xbox", 0x00070000: "téléchargement Xbox", 0x00080000: "démo",
    0x00090000: "vidéo", 0x000A0000: "titre", 0x000B0000: "installeur", 0x000C0000: "bande-annonce",
    0x000D0000: "jeu Arcade", 0x000E0000: "XNA",
}
LANG_COUNT, LANG_SIZE = 18, 0x80


def _wstr(b):
    return b.decode("utf-16-be", "replace").split("\0")[0]


def read_header(path):
    """Renvoie un dict : magic, type, title_id, size, name(s), desc, thumb (octets PNG)."""
    with open(path, "rb") as f:
        h = f.read(0x971A + 0x4000 if os.path.getsize(path) > 0x971A else os.path.getsize(path))
    if h[:4] not in MAGICS:
        raise ValueError(f"{path} : pas un paquet STFS (magique {h[:4]!r})")
    if len(h) < 0x171A:
        raise ValueError(f"{path} : en-tête tronqué")
    hdr_size, ctype = struct.unpack(">II", h[0x340:0x348])
    size = struct.unpack(">Q", h[0x34C:0x354])[0]
    title_id = struct.unpack(">I", h[0x360:0x364])[0]
    names = [_wstr(h[0x411 + i * LANG_SIZE:0x411 + (i + 1) * LANG_SIZE]) for i in range(LANG_COUNT)]
    descs = [_wstr(h[0xD11 + i * LANG_SIZE:0xD11 + (i + 1) * LANG_SIZE]) for i in range(LANG_COUNT)]
    tsize = struct.unpack(">I", h[0x1712:0x1716])[0]
    thumb = h[0x171A:0x171A + min(tsize, 0x4000)] if tsize else b""
    return dict(magic=h[:4].decode(), header_size=hdr_size, type=ctype, title_id=title_id, size=size,
                names=names, name=next((n for n in names if n), ""),
                desc=next((d for d in descs if d), ""), descs=descs,
                publisher=_wstr(h[0x1611:0x1611 + LANG_SIZE]), title_name=_wstr(h[0x1691:0x1691 + LANG_SIZE]),
                thumb=thumb, file_size=os.path.getsize(path))


def describe(path, hd):
    t = TYPES.get(hd["type"], "inconnu")
    return (f"{path}\n  {hd['magic']}  type 0x{hd['type']:08X} ({t})  TitleID {hd['title_id']:08X}"
            f"\n  nom : {hd['name']!r}\n  description : {hd['desc']!r}"
            f"\n  miniature : {len(hd['thumb'])} octets  fichier : {hd['file_size']} octets")


def import_package(profile, path, common=False, title=None, ctype=None):
    hd = read_header(path)
    tid = hd["title_id"] if title is None else title
    ty = hd["type"] if ctype is None else ctype
    owner = "0000000000000000" if common else None
    root = os.path.join(profile, "Content") if owner is None else os.path.join(
        os.path.dirname(profile.rstrip("/")), owner, "Content")
    dest_dir = os.path.join(root, f"{tid:08X}", f"{ty:08X}")
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, os.path.basename(path))
    shutil.copyfile(path, dest)
    print(f"{path} -> {dest}")
    return dest


def list_profile(profile):
    root = os.path.join(profile, "Content")
    if not os.path.isdir(root):
        print("aucun contenu")
        return
    for tid in sorted(os.listdir(root)):
        for ty in sorted(os.listdir(os.path.join(root, tid))):
            d = os.path.join(root, tid, ty)
            for fn in sorted(os.listdir(d)):
                try:
                    hd = read_header(os.path.join(d, fn))
                    print(f"{tid}/{ty}/{fn}  {hd['name']!r}  {hd['file_size']} octets")
                except ValueError as e:
                    print(f"{tid}/{ty}/{fn}  ILLISIBLE : {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("info"); p.add_argument("paquets", nargs="+")
    p = sub.add_parser("import")
    p.add_argument("profil"); p.add_argument("paquets", nargs="+")
    p.add_argument("--common", action="store_true", help="contenu partagé (XUID 0000000000000000)")
    p.add_argument("--title", type=lambda s: int(s, 16), help="forcer le TitleID (hexadécimal)")
    p.add_argument("--type", type=lambda s: int(s, 0), help="forcer le type de contenu")
    p = sub.add_parser("list"); p.add_argument("profil")
    a = ap.parse_args()
    if a.cmd == "info":
        for f in a.paquets:
            print(describe(f, read_header(f)))
    elif a.cmd == "import":
        for f in a.paquets:
            import_package(a.profil, f, a.common, a.title, a.type)
    else:
        list_profile(a.profil)


if __name__ == "__main__":
    main()
