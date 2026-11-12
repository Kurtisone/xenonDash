#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lecteur unifié des formats de jeu Xbox 360 (les images de l'utilisateur, jamais copiées ni modifiées).

Formats reconnus par contenu (magie), pas par extension :
  dir     dossier extrait (contient default.xex)
  xex     fichier XEX2 nu
  xdvdfs  image disque XDVDFS / GDFX (XGD1, XGD2, XGD3, ou image sans partition vidéo)
  stfs    paquet STFS LIVE / PIRS / CON (XBLA, démos, mises à jour...)
  god     Games on Demand / SVOD : en-tête LIVE/PIRS + dossier <nom>.data/Data0000...
  tree    arborescence « disque dur » <TitleID>/<type>/<paquet> (ou tout dossier de paquets)

API :  detect(path) -> format | None ;  open(path) -> Container
       Container : list(), read(nom), extract(dest, progress=None), metadata(), find_xex()
CLI :  xcontainer.py info <chemin>            JSON : format, métadonnées, identité du XEX, nom et icône
       xcontainer.py list <chemin>            fichiers du conteneur
       xcontainer.py cat <chemin> <fichier>   écrit le fichier sur stdout
       xcontainer.py extract <chemin> <dest>  extraction complète (progression sur stderr, idempotente : .extracted.ok)
`extract` affiche sur stdout le dossier qui contient default.xex (dest, ou le dossier source pour `dir` et `xex`,
qui n'ont rien à extraire).

Le nom et l'icône viennent de la ressource XDBF du XEX (ressource nommée par le TitleID) : cela demande l'image
décodée, obtenue par `rexglue dump-xex` (SDK) ou `xexinfo` ; à défaut, l'en-tête STFS (nom, miniature de titre).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

_fopen = open   # `open` devient l'ouverture de conteneur (API) plus bas
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECTOR = 2048
XDVDFS_MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Décalages de la partition de jeu : image nue, XGD2, XGD3, XGD1 ; puis disques Xbox d'origine.
ISO_OFFSETS = (0x00000000, 0x0FD90000, 0x02080000, 0x18300000, 0x0000FB20, 0x00020600)
STFS_MAGICS = (b"LIVE", b"PIRS", b"CON ")

CONTENT_TYPES = {
    0x1: "sauvegarde", 0x2: "contenu du Marketplace (DLC)", 0x3: "contenu d'éditeur", 0x1000: "titre Xbox 360",
    0x2000: "tampon IPTV", 0x4000: "jeu installé", 0x5000: "jeu Xbox d'origine", 0x6000: "titre social",
    0x7000: "Games on Demand", 0x9000: "objet d'avatar", 0x10000: "profil", 0x20000: "image de joueur",
    0x30000: "thème", 0x40000: "fichier de cache", 0x50000: "téléchargement", 0x60000: "sauvegarde Xbox",
    0x70000: "téléchargement Xbox", 0x80000: "démo", 0x90000: "vidéo", 0xA0000: "jeu (disque)",
    0xB0000: "mise à jour (installeur)", 0xC0000: "bande-annonce", 0xD0000: "Xbox Live Arcade", 0xE0000: "XNA",
    0xF0000: "magasin de licences", 0x100000: "film", 0x200000: "télévision", 0x2000000: "jeu communautaire",
}
# Types dont le paquet peut contenir un jeu exécutable (à confirmer par la présence de default.xex).
GAME_TYPES = {0x1000, 0x4000, 0x7000, 0x80000, 0xA0000, 0xD0000, 0xE0000, 0x2000000}


class FormatError(Exception):
    pass


def u16(b, o=0): return struct.unpack_from(">H", b, o)[0]
def u32(b, o=0): return struct.unpack_from(">I", b, o)[0]
def u64(b, o=0): return struct.unpack_from(">Q", b, o)[0]
def u24le(b, o=0): return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


def fmt_version(v):
    return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"


# ---------------------------------------------------------------------------------------------
# Flux : lecture ciblée à un décalage (jamais de lecture complète d'une image)
# ---------------------------------------------------------------------------------------------
class FileStream:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.fstat(self.fd).st_size

    def read_at(self, off, n):
        out = []
        while n > 0:
            b = os.pread(self.fd, n, off)
            if not b:
                break
            out.append(b)
            off += len(b)
            n -= len(b)
        return b"".join(out)

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None


class GodStream:
    """Flux XDVDFS reconstruit à partir des fichiers Data0000... d'un SVOD : chaque fichier est une « partie »
    = 1 bloc de hachage maître + 203 sous-parties de (1 bloc de hachage + 204 blocs de données) ; blocs de 4096 octets.
    Les blocs de hachage sont sautés."""
    BLOCK = 0x1000
    PER_SUB = 204
    SUBS = 203
    PER_PART = PER_SUB * SUBS          # 41412 blocs de données (0x14388 secteurs de 2048)

    def __init__(self, data_dir):
        self.parts = []
        i = 0
        while True:
            p = os.path.join(data_dir, f"Data{i:04d}")
            if not os.path.isfile(p):
                break
            self.parts.append(FileStream(p))
            i += 1
        if not self.parts:
            raise FormatError(f"aucun fichier Data0000 dans {data_dir}")
        self.size = len(self.parts) * self.PER_PART * self.BLOCK   # majorant

    def read_at(self, off, n):
        out = []
        while n > 0:
            d, inner = divmod(off, self.BLOCK)
            part, within = divmod(d, self.PER_PART)
            if part >= len(self.parts):
                break
            sub, blk = divmod(within, self.PER_SUB)
            phys = (1 + sub * (self.PER_SUB + 1) + 1 + blk) * self.BLOCK + inner
            take = min(n, self.BLOCK - inner)
            b = self.parts[part].read_at(phys, take)
            if not b:
                break
            out.append(b)
            off += len(b)
            n -= len(b)
            if len(b) < take:
                break
        return b"".join(out)

    def close(self):
        for p in self.parts:
            p.close()


# ---------------------------------------------------------------------------------------------
# XEX2 : en-tête en clair
# ---------------------------------------------------------------------------------------------
def parse_xex_header(head):
    """Identité lue dans l'en-tête XEX2 (sans déchiffrer) : TitleID, MediaID, versions, disque, ressources."""
    if head[:4] != b"XEX2":
        raise FormatError("pas un XEX2")
    flags, data_off = u32(head, 4), u32(head, 8)
    count = u32(head, 0x14)
    if count > 512:
        raise FormatError("en-tête XEX2 invalide")
    opt = {}
    for i in range(count):
        k, v = struct.unpack_from(">II", head, 0x18 + 8 * i)
        opt[k] = v
    if 0x40006 not in opt or opt[0x40006] + 0x18 > len(head):
        raise FormatError("en-tête d'exécution absent du XEX")
    e = opt[0x40006]
    media_id, version, base_version, title_id = struct.unpack_from(">IIII", head, e)
    platform, exec_type, disc, disc_count = head[e + 16:e + 20]
    info = {
        "module_flags": flags, "title_id": f"{title_id:08X}", "media_id": f"{media_id:08X}",
        "version": fmt_version(version), "base_version": fmt_version(base_version),
        "disc_number": disc, "disc_count": disc_count, "header_size": data_off,
        "is_title": bool(flags & 0x1) and not (flags & 0x78),   # titre, ni DLL ni correctif
        "image_base": opt.get(0x10201),
    }
    res = []
    if 0x2FF in opt and opt[0x2FF] + 4 <= len(head):
        v = opt[0x2FF]
        size = u32(head, v)
        for j in range(max(0, min((size - 4) // 16, 64))):
            ent = head[v + 4 + j * 16:v + 20 + j * 16]
            if len(ent) < 16:
                break
            res.append({"name": ent[:8].rstrip(b"\0").decode("latin1"), "addr": u32(ent, 8), "size": u32(ent, 12)})
    info["resources"] = res
    return info


def read_xex_head(read_fn):
    """Lit l'en-tête d'un XEX via read_fn(offset, n)."""
    first = read_fn(0, 0x1000)
    if first[:4] != b"XEX2":
        raise FormatError("pas un XEX2")
    size = min(max(u32(first, 8), 0x1000), 4 << 20)
    return first if size <= len(first) else read_fn(0, size)


def parse_xdbf(blob):
    if blob[:4] != b"XDBF":
        raise FormatError("XDBF attendu")
    _, cap, count, free_cap, _ = struct.unpack_from(">IIIII", blob, 4)
    start = 24 + cap * 18 + free_cap * 8
    out = {}
    for i in range(count):
        ns, eid, off, size = struct.unpack_from(">HQII", blob, 24 + i * 18)
        out[(ns, eid)] = blob[start + off:start + off + size]
    return out


def xdbf_title(blob):
    """(nom, icône PNG | None, langue) de la ressource de titre : chaîne 0x8000 de la langue par défaut, image 0x8000."""
    x = parse_xdbf(blob)
    lang = 1
    xstc = x.get((1, 0x58535443))
    if xstc and len(xstc) >= 16:
        lang = u32(xstc, 12) or 1
    strs = x.get((3, lang)) or x.get((3, 1)) or next((v for (ns, _), v in sorted(x.items()) if ns == 3), None)
    name = None
    if strs and len(strs) >= 14:
        n, p = u16(strs, 12), 14
        for _ in range(n):
            if p + 4 > len(strs):
                break
            sid, ln = struct.unpack_from(">HH", strs, p)
            p += 4
            if sid == 0x8000:
                name = strs[p:p + ln].decode("utf-8", "replace").strip("\0 ")
            p += ln
    icon = x.get((2, 0x8000))
    if icon and icon[:4] != b"\x89PNG":
        icon = None
    return name or None, icon, lang


def find_decoder():
    """Outil qui décode l'image d'un XEX : ('rexglue'|'xexinfo', chemin, env) ou None."""
    sdk = os.environ.get("XD_SDK", os.path.join(ROOT, "sdk"))
    cands = [("rexglue", os.environ.get("XD_REXGLUE") or os.path.join(sdk, "bin", "rexglue")),
             ("xexinfo", os.environ.get("XD_XEXINFO") or os.path.join(ROOT, "tools", "xexinfo", "build", "xexinfo"))]
    for kind, p in cands:
        if p and os.path.isfile(p) and os.access(p, os.X_OK):
            env = dict(os.environ)
            lib = os.path.join(sdk, "lib")
            if kind == "rexglue":
                env["LD_LIBRARY_PATH"] = lib + (":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else "")
            return kind, p, env
    for kind in ("rexglue", "xexinfo"):
        p = shutil.which(kind)
        if p:
            return kind, p, dict(os.environ)
    return None


def decode_xex_image(xex_path, workdir):
    """Image décodée du XEX -> (octets, adresse de base) ; None si aucun décodeur."""
    dec = find_decoder()
    if not dec:
        return None
    kind, tool, env = dec
    if kind == "rexglue":
        out = os.path.join(workdir, "img")
        r = subprocess.run([tool, "dump-xex", xex_path, out], env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, timeout=300)
        bins = sorted(f for f in (os.listdir(out) if os.path.isdir(out) else []) if f.endswith(".bin"))
        if r.returncode != 0 or not bins:
            return None
        m = re.search(r"_([0-9a-fA-F]{8})_[0-9a-fA-F]{8}\.bin$", bins[0])
        with _fopen(os.path.join(out, bins[0]), "rb") as f:
            return f.read(), (int(m.group(1), 16) if m else None)
    out = os.path.join(workdir, "image.pe")
    r = subprocess.run([tool, xex_path, out], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
    if r.returncode != 0 or not os.path.isfile(out):
        return None
    with _fopen(out, "rb") as f:
        return f.read(), None


def xex_title_data(xex_path, xex_info):
    """Nom et icône de la ressource XDBF du XEX (par décodage de l'image) ; (None, None) si impossible."""
    res = xex_info.get("resources") or []
    r = next((x for x in res if x["name"].upper() == xex_info["title_id"]), None) or (res[0] if res else None)
    if not r:
        return None, None
    tmp = tempfile.mkdtemp(prefix="xcont-")
    try:
        dec = decode_xex_image(xex_path, tmp)
        if not dec:
            return None, None
        img, base = dec
        base = xex_info.get("image_base") or base
        if base is None:
            return None, None
        off = r["addr"] - base
        if off < 0 or off + r["size"] > len(img):
            return None, None
        name, icon, _ = xdbf_title(img[off:off + r["size"]])
        return name, icon
    except (FormatError, struct.error, subprocess.SubprocessError, OSError):
        return None, None
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------------------------
# XDVDFS
# ---------------------------------------------------------------------------------------------
def probe_xdvdfs(stream, extra_offsets=()):
    """(base, secteur racine, taille racine) si un volume XDVDFS est trouvé."""
    for base in tuple(extra_offsets) + ISO_OFFSETS:
        b = stream.read_at(base + 32 * SECTOR, 28)
        if b[:20] == XDVDFS_MAGIC:
            root_sector, root_size = struct.unpack("<II", b[20:28])
            if root_size and root_size < (1 << 28):
                return base, root_sector, root_size
    return None


class Xdvdfs:
    def __init__(self, stream, extra_offsets=()):
        self.stream = stream
        p = probe_xdvdfs(stream, extra_offsets)
        if not p:
            raise FormatError("volume XDVDFS introuvable")
        self.base, self.root_sector, self.root_size = p
        self._files = None

    def _walk(self):
        files, dirs_seen = {}, set()
        pending = [(self.root_sector, self.root_size, "")]
        while pending:
            sector, size, prefix = pending.pop()
            if (sector, prefix) in dirs_seen or size > (1 << 26):
                continue
            dirs_seen.add((sector, prefix))
            buf = self.stream.read_at(self.base + sector * SECTOR, size)
            seen_nodes, stack = set(), [0]
            while stack:
                node = stack.pop()
                if node in seen_nodes or node + 14 > len(buf):
                    continue
                seen_nodes.add(node)
                left, right, sec, length, attr, nlen = struct.unpack_from("<HHIIBB", buf, node)
                if left == 0xFFFF and right == 0xFFFF and sec == 0xFFFFFFFF:
                    continue
                name = buf[node + 14:node + 14 + nlen].decode("latin1")
                if left and left != 0xFFFF:
                    stack.append(left * 4)
                if right and right != 0xFFFF:
                    stack.append(right * 4)
                if not name or "/" in name or name in (".", ".."):
                    continue
                path = prefix + name
                if attr & 0x10:
                    if length:
                        pending.append((sec, length, path + "/"))
                else:
                    files[path] = (sec, length)
        return files

    def files(self):
        if self._files is None:
            self._files = self._walk()
        return self._files

    def read_range(self, name, off, n):
        sec, length = self.files()[name]
        n = max(0, min(n, length - off))
        return self.stream.read_at(self.base + sec * SECTOR + off, n)


# ---------------------------------------------------------------------------------------------
# STFS (LIVE / PIRS / CON) et en-tête commun (SVOD)
# ---------------------------------------------------------------------------------------------
def _wstr(b):
    return b.decode("utf-16-be", "replace").split("\0")[0].strip()


def parse_package_header(head):
    """Métadonnées de l'en-tête commun STFS/SVOD (lues sur les 0x971A premiers octets)."""
    if head[:4] not in STFS_MAGICS:
        raise FormatError("pas un paquet STFS")
    hsize = u32(head, 0x340)
    m = {
        "magic": head[:4].decode("latin1").strip(),
        "header_size": hsize,
        "content_type": u32(head, 0x344),
        "media_id": f"{u32(head, 0x354):08X}",
        "version": fmt_version(u32(head, 0x358)),
        "base_version": fmt_version(u32(head, 0x35C)),
        "title_id": f"{u32(head, 0x360):08X}",
        "disc_number": head[0x366], "disc_count": head[0x367],
        "descriptor_type": head[0x3A9],     # 0 = STFS, 1 = SVOD
    }
    m["content_type_name"] = CONTENT_TYPES.get(m["content_type"], f"0x{m['content_type']:X}")
    names = [_wstr(head[0x411 + i * 0x80:0x411 + (i + 1) * 0x80]) for i in range(9)]
    m["display_name"] = next((n for n in names if n), "")
    m["publisher"] = _wstr(head[0x1611:0x1691])
    m["title_name"] = _wstr(head[0x1691:0x1711])
    if len(head) >= 0x171A:
        tsz, ttsz = u32(head, 0x1712), u32(head, 0x1716)
        if 0 < tsz <= 0x4000 and head[0x171A:0x171A + 4] == b"\x89PNG":
            m["_thumbnail"] = head[0x171A:0x171A + tsz]
        if 0 < ttsz <= 0x4000 and head[0x571A:0x571A + 4] == b"\x89PNG":
            m["_title_thumbnail"] = head[0x571A:0x571A + ttsz]
    return m


class StfsFS:
    """Table de fichiers d'un paquet STFS. Décalages de blocs : une table de hachage L0 tous les 170 blocs, L1 tous
    les 28900, L2 tous les 4913000 ; chaque table occupe 1 bloc (paquet « read-only » LIVE/PIRS, bit 0 de l'octet
    0x37B) ou 2 blocs (CON, lecture-écriture)."""
    BLK = 0x1000

    def __init__(self, stream, head):
        self.stream = stream
        self.magic = head[:4]
        hsize = u32(head, 0x340)
        flags = head[0x37B]
        con = self.magic == b"CON "
        # Même règle que tools/stfs.py (qui a servi sur des CON) : en-tête de 0xB000 -> tables doubles.
        if con and ((hsize + 0xFFF) & 0xB000) == 0xB000:
            shift = 1
        else:
            shift = 0 if flags & 1 else 1
        self.h = 1 << shift
        self.data_start = (hsize + 0xFFF) & ~0xFFF
        self.ft_count = struct.unpack_from("<H", head, 0x37C)[0]
        self.ft_block = u24le(head, 0x37E)
        self.total_blocks = u32(head, 0x395)
        self._hashes = {}
        self.entries = self._read_table()

    def true_block(self, i):
        h = self.h
        r = (i // 0xAA + 1) * h + i
        if i >= 0xAA:
            r += ((i + 0x70E4) // 0x70E4) * h
        if i >= 0x70E4:
            r += ((i + 0x4AF768) // 0x4AF768) * h
        return r

    def block_off(self, i):
        return self.data_start + self.true_block(i) * self.BLK

    def _read_table(self):
        ents = []
        for k in range(self.ft_count):
            blk = self.stream.read_at(self.block_off(self.ft_block + k), self.BLK)
            if len(blk) < self.BLK:
                raise FormatError("table de fichiers STFS tronquée")
            for j in range(0, self.BLK, 0x40):
                e = blk[j:j + 0x40]
                fl = e[0x28]
                if fl & 0x3F == 0:
                    return self._finish(ents)
                ents.append({
                    "name": e[:fl & 0x3F].decode("latin1"), "flags": fl, "blocks": u24le(e, 0x29),
                    "start": u24le(e, 0x2F), "parent": struct.unpack_from(">h", e, 0x32)[0], "size": u32(e, 0x34)})
        return self._finish(ents)

    @staticmethod
    def _finish(ents):
        for e in ents:
            parts, x, guard = [e["name"]], e, 0
            while x["parent"] != -1 and guard < 64:
                if not 0 <= x["parent"] < len(ents):
                    raise FormatError("table de fichiers STFS incohérente")
                x = ents[x["parent"]]
                parts.insert(0, x["name"])
                guard += 1
            e["path"] = "/".join(parts)
            e["dir"] = bool(e["flags"] & 0x80)
        return ents

    def files(self):
        return {e["path"]: e for e in self.entries if not e["dir"]}

    def _next_block(self, i):
        g = i // 170
        tab = self._hashes.get(g)
        if tab is None:
            first = (g * 170)
            tab = self.stream.read_at(self.data_start + (self.true_block(first) - self.h) * self.BLK, 170 * 24)
            self._hashes[g] = tab
        o = (i % 170) * 24 + 0x15
        return (tab[o] << 16) | (tab[o + 1] << 8) | tab[o + 2]

    def block_chain(self, e):
        n = (e["size"] + self.BLK - 1) // self.BLK
        if e["flags"] & 0x40 or n <= 1:
            return range(e["start"], e["start"] + n)
        out, b = [], e["start"]
        for _ in range(n):
            out.append(b)
            b = self._next_block(b)
        return out

    def read_range(self, e, off, n):
        n = max(0, min(n, e["size"] - off))
        out, pos = [], off
        chain = self.block_chain(e)
        while n > 0:
            bi, inner = divmod(pos, self.BLK)
            take = min(n, self.BLK - inner)
            # regroupe les blocs physiquement contigus
            run, nxt = take, bi
            while run < n and bi_next_ok(self, chain, nxt) and run < (8 << 20):
                nxt += 1
                run += min(self.BLK, n - run)
            take = min(n, run)
            out.append(self.stream.read_at(self.block_off(chain[bi]) + inner, take))
            pos += take
            n -= take
        return b"".join(out)

    def verify_block(self, i):
        """Compare le SHA-1 du bloc de données i à sa table de hachage L0."""
        tab = self.stream.read_at(self.data_start + (self.true_block((i // 170) * 170) - self.h) * self.BLK, 170 * 24)
        o = (i % 170) * 24
        return hashlib.sha1(self.stream.read_at(self.block_off(i), self.BLK)).digest() == tab[o:o + 20]


def bi_next_ok(fs, chain, k):
    """Le bloc k+1 de la chaîne suit physiquement le bloc k."""
    return k + 1 < len(chain) and fs.block_off(chain[k + 1]) == fs.block_off(chain[k]) + fs.BLK


# ---------------------------------------------------------------------------------------------
# Conteneurs
# ---------------------------------------------------------------------------------------------
class Container:
    format = None

    def __init__(self, path):
        self.path = os.path.abspath(path)
        self._meta = {}

    def list(self):
        raise NotImplementedError

    def read_range(self, name, off, n):
        raise NotImplementedError

    def size_of(self, name):
        for f in self.list():
            if f["name"] == name:
                return f["size"]
        raise KeyError(name)

    def resolve(self, name):
        """Nom exact (sans tenir compte de la casse, séparateurs / ou \\)."""
        want = name.replace("\\", "/").strip("/").lower()
        for f in self.list():
            if f["name"].lower() == want:
                return f["name"]
        raise KeyError(name)

    def read(self, name, max_bytes=None):
        name = self.resolve(name)
        size = self.size_of(name)
        return self.read_range(name, 0, size if max_bytes is None else min(size, max_bytes))

    def iter_file(self, name, chunk=4 << 20):
        size, off = self.size_of(name), 0
        while off < size:
            b = self.read_range(name, off, chunk)
            if not b:
                raise FormatError(f"lecture tronquée de {name}")
            yield b
            off += len(b)

    def find_xex(self):
        try:
            return self.resolve("default.xex")
        except KeyError:
            return None

    def xex_head(self):
        n = self.find_xex()
        if not n:
            return None
        return read_xex_head(lambda o, s: self.read_range(n, o, s))

    def xex_info(self):
        h = self.xex_head()
        return parse_xex_header(h) if h else None

    def metadata(self):
        return self._meta

    def signature_files(self):
        """Fichiers dont (taille, mtime) signent la source pour le cache de l'inventaire."""
        return [self.path]

    def title_fallback(self):
        """(nom, icône) de repli quand le XEX ne donne rien."""
        return None, None

    def extract_xex_to(self, dest):
        """Écrit default.xex dans dest (pour décoder sa ressource) ; renvoie le chemin."""
        n = self.find_xex()
        p = os.path.join(dest, "default.xex")
        with _fopen(p, "wb") as o:
            for b in self.iter_file(n):
                o.write(b)
        return p

    def title_data(self):
        """(nom, icône PNG, origine) : ressource du XEX, sinon repli de l'en-tête."""
        info = self.xex_info()
        if info:
            tmp = tempfile.mkdtemp(prefix="xcont-x-")
            try:
                name, icon = xex_title_data(self.extract_xex_to(tmp), info)
            finally:
                shutil.rmtree(tmp, ignore_errors=True)
            if name:
                fb_name, fb_icon = self.title_fallback()
                return name, icon or fb_icon, "xex"
        fb_name, fb_icon = self.title_fallback()
        if fb_name:
            return fb_name, fb_icon, "en-tête"
        return None, None, None

    def extract(self, dest, progress=None):
        """Extraction complète dans dest ; idempotente (.extracted.ok). Renvoie le dossier contenant default.xex."""
        sig = self._extract_sig()
        marker = os.path.join(dest, ".extracted.ok")
        try:
            with _fopen(marker) as f:
                if json.load(f).get("source") == sig:
                    return dest
        except (OSError, ValueError):
            pass
        files = self.list()
        total = sum(f["size"] for f in files)
        done = 0
        os.makedirs(dest, exist_ok=True)
        for f in files:
            rel = f["name"]
            parts = rel.split("/")
            if rel.startswith("/") or ".." in parts or "" in parts:
                raise FormatError(f"nom de fichier refusé : {rel!r}")
            dst = os.path.join(dest, *parts)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.isfile(dst) and os.path.getsize(dst) == f["size"] and not os.path.exists(dst + ".part"):
                done += f["size"]
                if progress:
                    progress(done, total, rel)
                continue
            with _fopen(dst + ".part", "wb") as o:
                for b in self.iter_file(rel):
                    o.write(b)
                    done += len(b)
                    if progress:
                        progress(done, total, rel)
            os.replace(dst + ".part", dst)
        with _fopen(marker, "w") as m:
            json.dump({"source": sig, "files": len(files), "bytes": total, "date": time.strftime("%Y-%m-%dT%H:%M:%S")}, m)
        return dest

    def _extract_sig(self):
        out = []
        for p in self.signature_files():
            try:
                st = os.stat(p)
                out.append([p, st.st_size, st.st_mtime_ns])
            except OSError:
                out.append([p, 0, 0])
        return out

    def close(self):
        pass


class DirContainer(Container):
    format = "dir"

    def __init__(self, path):
        super().__init__(path)
        self.root = self.path
        self._list = None

    def list(self):
        if self._list is None:
            out = []
            for d, dirs, files in os.walk(self.root, followlinks=True):
                dirs.sort()
                for f in sorted(files):
                    p = os.path.join(d, f)
                    try:
                        out.append({"name": os.path.relpath(p, self.root).replace(os.sep, "/"), "size": os.path.getsize(p)})
                    except OSError:
                        pass
            self._list = out
        return self._list

    def read_range(self, name, off, n):
        with _fopen(os.path.join(self.root, *name.split("/")), "rb") as f:
            f.seek(off)
            return f.read(n)

    def find_xex(self):
        try:
            for f in os.listdir(self.root):
                if f.lower() == "default.xex":
                    return f
        except OSError:
            pass
        return None

    def signature_files(self):
        x = self.find_xex()
        return [os.path.join(self.root, x)] if x else [self.root]

    def extract(self, dest, progress=None):
        return self.root      # rien à extraire : le dossier est déjà utilisable


class XexContainer(Container):
    format = "xex"

    def list(self):
        return [{"name": os.path.basename(self.path), "size": os.path.getsize(self.path)}]

    def read_range(self, name, off, n):
        with _fopen(self.path, "rb") as f:
            f.seek(off)
            return f.read(n)

    def find_xex(self):
        return os.path.basename(self.path)

    def extract_xex_to(self, dest):
        return self.path

    def extract(self, dest, progress=None):
        return os.path.dirname(self.path)


class StreamContainer(Container):
    """Base des conteneurs à flux (ISO, STFS, GOD)."""

    def close(self):
        if getattr(self, "stream", None):
            self.stream.close()


class XdvdfsContainer(StreamContainer):
    format = "xdvdfs"

    def __init__(self, path, stream=None, base_candidates=()):
        super().__init__(path)
        self.stream = stream or FileStream(self.path)
        self.fs = Xdvdfs(self.stream, base_candidates)
        self._meta = {"partition_offset": f"0x{self.fs.base:X}", "layout": self._layout(self.fs.base)}

    @staticmethod
    def _layout(base):
        return {0: "image nue", 0x0FD90000: "XGD2", 0x02080000: "XGD3", 0x18300000: "XGD1"}.get(base, "Xbox d'origine")

    def list(self):
        return [{"name": n, "size": s} for n, (_, s) in sorted(self.fs.files().items())]

    def read_range(self, name, off, n):
        return self.fs.read_range(name, off, n)


class StfsContainer(StreamContainer):
    format = "stfs"

    def __init__(self, path):
        super().__init__(path)
        self.stream = FileStream(self.path)
        head = self.stream.read_at(0, 0x971A)
        self._meta = parse_package_header(head)
        if self._meta["descriptor_type"] != 0:
            raise FormatError("descripteur de volume non STFS")
        if len(head) < 0x3A0:
            raise FormatError("en-tête STFS tronqué")
        self.fs = StfsFS(self.stream, head)
        self._files = self.fs.files()
        self._meta["files"] = len(self._files)

    def list(self):
        return [{"name": n, "size": e["size"]} for n, e in sorted(self._files.items())]

    def read_range(self, name, off, n):
        return self.fs.read_range(self._files[name], off, n)

    def title_fallback(self):
        m = self._meta
        return (m.get("title_name") or m.get("display_name") or None), (m.get("_title_thumbnail") or m.get("_thumbnail"))

    def metadata(self):
        return {k: v for k, v in self._meta.items() if not k.startswith("_")} | {
            "thumbnail_bytes": len(self._meta.get("_thumbnail", b"")),
            "title_thumbnail_bytes": len(self._meta.get("_title_thumbnail", b""))}


def god_data_dir(header_path):
    d = header_path + ".data"
    if os.path.isdir(d):
        return d
    parent, base = os.path.split(header_path)
    try:
        for f in os.listdir(parent or "."):
            if f.lower() == (base + ".data").lower() and os.path.isdir(os.path.join(parent, f)):
                return os.path.join(parent, f)
    except OSError:
        pass
    return None


class GodContainer(StreamContainer):
    format = "god"

    def __init__(self, path):
        super().__init__(path)
        head = FileStream(self.path)
        try:
            h = head.read_at(0, 0x971A)
        finally:
            head.close()
        self._meta = parse_package_header(h)
        if self._meta["descriptor_type"] != 1:
            raise FormatError("descripteur de volume non SVOD")
        d = god_data_dir(self.path)
        if not d:
            raise FormatError("dossier .data introuvable à côté de l'en-tête GOD")
        self.data_dir = d
        self.stream = GodStream(d)
        self.fs = Xdvdfs(self.stream, (0x2000, 0x1000))
        self._meta["data_files"] = len(self.stream.parts)
        self._meta["partition_offset"] = f"0x{self.fs.base:X}"

    def list(self):
        return [{"name": n, "size": s} for n, (_, s) in sorted(self.fs.files().items())]

    def read_range(self, name, off, n):
        return self.fs.read_range(name, off, n)

    def title_fallback(self):
        m = self._meta
        return (m.get("title_name") or m.get("display_name") or None), (m.get("_title_thumbnail") or m.get("_thumbnail"))

    def signature_files(self):
        return [self.path] + [p.path for p in self.stream.parts[:1]]

    def metadata(self):
        return {k: v for k, v in self._meta.items() if not k.startswith("_")}


def iter_packages(root, max_depth=6):
    """Fichiers d'un dossier susceptibles d'être des jeux, parcours récursif (liens suivis)."""
    seen = set()
    base_depth = root.rstrip("/").count("/")
    for d, dirs, files in os.walk(root, followlinks=True):
        rp = os.path.realpath(d)
        if rp in seen or d.count("/") - base_depth >= max_depth:
            dirs[:] = []
            continue
        seen.add(rp)
        dirs.sort()
        for f in sorted(files):
            if not f.startswith("."):
                yield os.path.join(d, f)


class TreeContainer(Container):
    """Arborescence « disque dur » : <TitleID>/<type>/<paquet>. `list` donne les paquets reconnus."""
    format = "tree"

    def __init__(self, path):
        super().__init__(path)
        self._pk = None

    def packages(self):
        if self._pk is None:
            out = []
            for p in iter_packages(self.path):
                try:
                    fmt = detect_file(p)
                except OSError:
                    fmt = None
                if fmt:
                    out.append((p, fmt))
            self._pk = out
        return self._pk

    def list(self):
        return [{"name": os.path.relpath(p, self.path), "size": os.path.getsize(p), "format": f}
                for p, f in self.packages()]

    def children(self):
        for p, _ in self.packages():
            try:
                yield open_container(p)
            except (FormatError, OSError):
                continue

    def read_range(self, name, off, n):
        raise FormatError("arborescence : ouvrir un paquet précis")

    def extract(self, dest, progress=None):
        raise FormatError("arborescence : extraire un paquet précis")


# ---------------------------------------------------------------------------------------------
# Détection et ouverture
# ---------------------------------------------------------------------------------------------
def detect_file(path):
    """Format d'un fichier par sa magie ; None si inconnu."""
    with _fopen(path, "rb") as f:
        head = f.read(0x3B0)
        if head[:4] == b"XEX2":
            return "xex"
        if head[:4] in STFS_MAGICS and len(head) >= 0x3AA:
            if head[0x3A9] == 1:
                return "god" if god_data_dir(os.path.abspath(path)) else "god-sans-data"
            return "stfs"
        size = os.fstat(f.fileno()).st_size
        for base in ISO_OFFSETS:
            if size >= base + 32 * SECTOR + 28:
                f.seek(base + 32 * SECTOR)
                if f.read(20) == XDVDFS_MAGIC:
                    return "xdvdfs"
    return None


def _has_default_xex(d):
    try:
        return any(f.lower() == "default.xex" and os.path.isfile(os.path.join(d, f)) for f in os.listdir(d))
    except OSError:
        return False


def detect(path):
    """'dir' | 'xex' | 'xdvdfs' | 'stfs' | 'god' | 'tree' | None (jamais d'exception)."""
    try:
        path = os.path.abspath(path)
        if os.path.isdir(path):
            if _has_default_xex(path):
                return "dir"
            for p in iter_packages(path, 4):
                if detect_file(p) in ("stfs", "god", "xdvdfs", "xex"):
                    return "tree"
            return None
        if os.path.isfile(path):
            f = detect_file(path)
            return None if f == "god-sans-data" else f
    except OSError:
        pass
    return None


def open_container(path):
    fmt = detect(path)
    if fmt is None:
        raise FormatError(f"format non reconnu : {path}")
    cls = {"dir": DirContainer, "xex": XexContainer, "xdvdfs": XdvdfsContainer, "stfs": StfsContainer,
           "god": GodContainer, "tree": TreeContainer}[fmt]
    try:
        return cls(path)
    except (struct.error, IndexError) as e:
        raise FormatError(f"{fmt} corrompu : {e}")


open = open_container   # noqa: A001 (nom voulu par l'API)


def info(path, decode=True):
    """Dictionnaire JSON : format, métadonnées, identité du XEX, nom et icône du titre."""
    c = open_container(path)
    try:
        out = {"path": c.path, "format": c.format, "metadata": c.metadata()}
        if c.format == "tree":
            out["packages"] = c.list()
            return out
        try:
            x = c.xex_info()
        except (FormatError, KeyError, struct.error) as e:
            x, out["xex_error"] = None, str(e)
        if x:
            x = dict(x)
            x["resources"] = [r["name"] for r in x["resources"]]
            out["xex"] = x
        out["executable"] = bool(x and x["is_title"])
        if decode and x:
            name, icon, origin = c.title_data()
            out["title"] = {"name": name, "icon_bytes": len(icon) if icon else 0, "origin": origin}
        return out
    finally:
        c.close()


# ---------------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------------
def _progress_printer():
    last = [0.0]

    def p(done, total, name):
        now = time.time()
        if now - last[0] < 1.0 and done < total:
            return
        last[0] = now
        pct = 100 * done // total if total else 100
        print(f"[extract] {done >> 20}/{total >> 20} Mo ({pct}%) {name}", file=sys.stderr, flush=True)
    return p


def main(argv=None):
    ap = argparse.ArgumentParser(description="Lecteur unifié des formats de jeu Xbox 360")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("info"); s.add_argument("path"); s.add_argument("--fast", action="store_true", help="sans décoder le XEX")
    s = sub.add_parser("list"); s.add_argument("path"); s.add_argument("--json", action="store_true")
    s = sub.add_parser("cat"); s.add_argument("path"); s.add_argument("file")
    s = sub.add_parser("extract"); s.add_argument("path"); s.add_argument("dest")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "info":
            print(json.dumps(info(a.path, decode=not a.fast), indent=2, ensure_ascii=False))
        elif a.cmd == "list":
            c = open_container(a.path)
            lst = c.list()
            if a.json:
                print(json.dumps(lst, indent=2, ensure_ascii=False))
            else:
                for f in lst:
                    print(f"{f['size']:>12} {f['name']}")
        elif a.cmd == "cat":
            c = open_container(a.path)
            for b in c.iter_file(c.resolve(a.file)):
                sys.stdout.buffer.write(b)
        elif a.cmd == "extract":
            c = open_container(a.path)
            print(c.extract(os.path.abspath(a.dest), _progress_printer()))
    except (FormatError, KeyError, OSError) as e:
        print(f"xcontainer : {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
