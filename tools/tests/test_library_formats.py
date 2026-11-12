# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests de tools/xcontainer.py : formats réels (si présents sur la machine) et synthétiques (générés ici).

Lancer : python3 -m unittest discover -s tools/tests -p 'test_library_*.py'   (depuis la racine du dépôt)
Variable XD_TEST_TMP : dossier temporaire (défaut : tempfile). Les fichiers réels sont lus, jamais modifiés ni copiés.
Les ISO, GOD et paquets STFS synthétiques sont fabriqués par les générateurs ci-dessous (aucun fichier de jeu versionné).
"""
import hashlib
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import xcontainer as xc  # noqa: E402

HOME = os.path.expanduser("~")
SKATE_DIR = os.path.join(HOME, "Projects/Skate1Recompiled/game")
TRIALS_PKG_DIR = os.path.join(HOME, "Projects/TrialsReVolution/Trials Fusion XBLA/58411401/000D0000")
TRIALS_XEX = os.path.join(HOME, "Projects/TrialsReVolution/games/trials_fusion/assets/default.xex")
FIRMWARE_PIRS = os.path.join(os.path.dirname(os.path.dirname(HERE)), "firmware/2.0.13604.0/su20076000_00000000")
SKATE_XEX = os.path.join(SKATE_DIR, "default.xex")


# ---------------------------------------------------------------------------------------------
# Générateurs synthétiques
# ---------------------------------------------------------------------------------------------
def _tree(files):
    """{'a/b.bin': contenu} -> arbre {nom: sous-arbre | contenu}."""
    root = {}
    for p, v in files.items():
        node = root
        parts = p.split("/")
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = v
    return root


def _bst(entries):
    """Entrées triées -> arbre binaire équilibré ; renvoie (racine, liste de (entrée, gauche, droite))."""
    order = []

    def rec(lo, hi):
        if lo >= hi:
            return None
        mid = (lo + hi) // 2
        idx = len(order)
        order.append([entries[mid], None, None])
        order[idx][1] = rec(lo, mid)
        order[idx][2] = rec(mid + 1, hi)
        return idx
    return rec(0, len(entries)), order


def write_xdvdfs(f, files, base=0, filler_fn=None):
    """Écrit un volume XDVDFS dans le fichier f à `base`. files : {chemin: bytes | int (taille, contenu nul)}."""
    S = 2048
    state = {"next": 34}      # prochain secteur libre ; 32 = descripteur, 33.. = répertoires

    def alloc(nbytes):
        s = state["next"]
        state["next"] += max(1, (nbytes + S - 1) // S)
        return s

    # les offsets suivent l'ordre de `_bst` : la racine est à l'offset 0
    def build_dir(tree):
        names = sorted(tree, key=str.lower)
        ents = [(n, tree[n]) for n in names]
        rs = [(14 + len(n.encode("latin1")) + 3) // 4 * 4 for n, _ in ents]
        dsize = max(S, (sum(rs) + S - 1) // S * S)
        dsec = alloc(dsize)
        info = []
        for n, v in ents:
            if isinstance(v, dict):
                info.append((0x10,) + build_dir(v))
            else:
                size = v if isinstance(v, int) else len(v)
                sec = alloc(size) if size else 0
                if isinstance(v, bytes) and size:
                    f.seek(base + sec * S)
                    f.write(v)
                info.append((0x20, sec, size))
        root, order = _bst(list(range(len(ents))))
        pos = {}
        o = 0
        for ent, _, _ in order:
            pos[ent] = o
            o += rs[ent]
        buf = bytearray(b"\xff" * dsize)
        for ent, left, right in order:
            n = ents[ent][0].encode("latin1")
            attr, sec, size = info[ent]
            struct.pack_into("<HHIIBB", buf, pos[ent], pos[order[left][0]] // 4 if left is not None else 0,
                             pos[order[right][0]] // 4 if right is not None else 0, sec, size, attr, len(n))
            buf[pos[ent] + 14:pos[ent] + 14 + len(n)] = n
        f.seek(base + dsec * S)
        f.write(bytes(buf))
        return dsec, dsize

    root_sec, root_size = build_dir(_tree(files))
    vd = bytearray(S)
    vd[:20] = xc.XDVDFS_MAGIC
    struct.pack_into("<II", vd, 20, root_sec, root_size)
    vd[0x7EC:0x7EC + 20] = xc.XDVDFS_MAGIC
    f.seek(base + 32 * S)
    f.write(bytes(vd))
    f.truncate(max(f.seek(0, 2), base + state["next"] * S))
    return state["next"] * S


def header_bytes(magic=b"LIVE", content_type=0xD0000, title_id=0x58411401, media_id=0x1234, version=3, title="Synth",
                 descriptor_type=0, hsize=0xAD0E, thumb=None):
    h = bytearray(0xB000)
    h[:4] = magic
    struct.pack_into(">I", h, 0x340, hsize)
    struct.pack_into(">IIIII", h, 0x344, content_type, 0, 0, 0, 0)
    struct.pack_into(">III", h, 0x354, media_id, version, version)
    struct.pack_into(">I", h, 0x360, title_id)
    h[0x366], h[0x367] = 1, 1
    h[0x379] = 0x24
    h[0x3A9] = descriptor_type
    h[0x411:0x411 + 2 * len(title)] = title.encode("utf-16-be")
    h[0x1691:0x1691 + 2 * len(title)] = title.encode("utf-16-be")
    if thumb:
        struct.pack_into(">II", h, 0x1712, len(thumb), len(thumb))
        h[0x171A:0x171A + len(thumb)] = thumb
        h[0x571A:0x571A + len(thumb)] = thumb
    return h


TINY_PNG = (b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x06\x00\x00\x00\x1f\x15\xc4\x89"
            b"\x00\x00\x00\rIDATx\x9cc\xf8\xff\xff?\x00\x05\xfe\x02\xfe\xa7\x9a\xa0\xa0\x00\x00\x00\x00IEND\xaeB`\x82")


def write_god(header_path, image_path, content_type=0x7000, title_id=0x45410813, title="Synth GOD", thumb=TINY_PNG):
    """GOD/SVOD à partir d'une image XDVDFS (partition de jeu, base 0) : <header> + <header>.data/Data0000..."""
    B, PER_SUB, SUBS = 0x1000, 204, 203
    hdr = header_bytes(b"LIVE", content_type, title_id, 0x1111, 3, title, descriptor_type=1, thumb=thumb)
    with open(header_path, "wb") as o:
        o.write(hdr)
    ddir = header_path + ".data"
    os.makedirs(ddir, exist_ok=True)
    with open(image_path, "rb") as img:
        part = 0
        while True:
            chunk = img.read(B * PER_SUB * SUBS)
            if not chunk:
                break
            nblk = (len(chunk) + B - 1) // B
            chunk = chunk.ljust(nblk * B, b"\0")
            subs = []
            body = bytearray()
            for s in range(0, nblk, PER_SUB):
                blocks = chunk[s * B:(s + PER_SUB) * B]
                n = len(blocks) // B
                table = b"".join(hashlib.sha1(blocks[i * B:(i + 1) * B]).digest() for i in range(n)).ljust(B, b"\0")
                subs.append(hashlib.sha1(table).digest())
                body += table + blocks
            master = b"".join(subs).ljust(B, b"\0")
            with open(os.path.join(ddir, f"Data{part:04d}"), "wb") as o:
                o.write(master + bytes(body))
            part += 1
    return ddir


class StfsBuilder:
    """Paquet STFS en lecture seule (LIVE/PIRS) : tables L0 tous les 170 blocs, L1 après le premier groupe."""

    def __init__(self, magic=b"LIVE", content_type=0xD0000, title_id=0x58411401, title="Synth STFS", thumb=TINY_PNG):
        self.args = dict(magic=magic, content_type=content_type, title_id=title_id, title=title, thumb=thumb)
        self.files = []   # (nom, contenu, consecutif)

    def add(self, name, data, consecutive=True):
        self.files.append((name, data, consecutive))

    @staticmethod
    def true_block(i):
        # écriture indépendante de xcontainer, valable pour moins de 28900 blocs
        return i + (i // 170 + 1) + (1 if i >= 170 else 0)

    def build(self, path):
        entries = []     # (nom, start, nblocks, size, flags)
        nxt = 1          # le bloc 0 est la table de fichiers
        alloc = {}       # bloc logique -> octets
        chains = {}      # bloc -> suivant (fichiers non consécutifs)
        order = []
        for name, data, consec in self.files:
            nb = max(1, (len(data) + 0xFFF) // 0x1000)
            if consec:
                blocks = list(range(nxt, nxt + nb))
            else:   # blocs entrelacés : on réserve le double et on en prend un sur deux (dans l'ordre inverse)
                blocks = list(range(nxt + 2 * nb - 2, nxt - 1, -2))
                nb_res = 2 * nb
            nxt += nb if consec else nb_res
            for k, b in enumerate(blocks):
                alloc[b] = data[k * 0x1000:(k + 1) * 0x1000].ljust(0x1000, b"\0")
                chains[b] = blocks[k + 1] if k + 1 < len(blocks) else 0xFFFFFF
            entries.append((name, blocks[0], nb, len(data), 0x40 if consec else 0))
        ft = bytearray(0x1000)
        for i, (n, start, nb, size, fl) in enumerate(entries):
            nb_ = n.encode("latin1")
            e = bytearray(0x40)
            e[:len(nb_)] = nb_
            e[0x28] = len(nb_) | fl
            e[0x29:0x2C] = nb.to_bytes(3, "little")
            e[0x2C:0x2F] = nb.to_bytes(3, "little")
            e[0x2F:0x32] = start.to_bytes(3, "little")
            struct.pack_into(">hI", e, 0x32, -1, size)
            ft[i * 0x40:(i + 1) * 0x40] = e
        alloc[0] = bytes(ft)
        total = nxt
        hdr = header_bytes(**self.args)
        hdr[0x37B] = 0x01                       # lecture seule : tables de 1 bloc
        struct.pack_into("<H", hdr, 0x37C, 1)
        hdr[0x37E:0x381] = (0).to_bytes(3, "little")
        struct.pack_into(">I", hdr, 0x395, total)
        ngroups = (total + 169) // 170
        phys = {}
        for b, data in alloc.items():
            phys[self.true_block(b)] = data
        for g in range(ngroups):
            tab = bytearray(0x1000)
            for j in range(170):
                b = g * 170 + j
                if b in alloc:
                    tab[j * 24:j * 24 + 20] = hashlib.sha1(alloc[b]).digest()
                    tab[j * 24 + 0x14] = 0x80
                    tab[j * 24 + 0x15:j * 24 + 0x18] = chains.get(b, 0xFFFFFF).to_bytes(3, "big")
            phys[self.true_block(g * 170) - 1] = bytes(tab)
        last = max(phys)
        with open(path, "wb") as o:
            o.write(hdr)
            for p in range(last + 1):
                o.write(phys.get(p, b"\0" * 0x1000))


# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
TMP_ROOT = os.environ.get("XD_TEST_TMP")


def wfile(p, data):
    with open(p, "wb") as f:
        f.write(data)


def rfile(p):
    with open(p, "rb") as f:
        return f.read()


class TmpCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xd-test-", dir=TMP_ROOT)
        self.addCleanup(shutil.rmtree, self.tmp, True)


@unittest.skipUnless(os.path.isfile(SKATE_XEX), "Skate absent")
class RealSkate(unittest.TestCase):
    def test_dir(self):
        self.assertEqual(xc.detect(SKATE_DIR), "dir")
        i = xc.info(SKATE_DIR)
        self.assertEqual(i["xex"]["title_id"], "45410813")
        self.assertEqual(i["xex"]["version"], "0.0.0.3")
        self.assertEqual(i["xex"]["media_id"], "21D6D331")
        self.assertTrue(i["executable"])
        self.assertEqual(i["title"]["name"], "skate.")
        self.assertGreater(i["title"]["icon_bytes"], 1000)

    def test_xex_nu(self):
        self.assertEqual(xc.detect(SKATE_XEX), "xex")
        self.assertEqual(xc.info(SKATE_XEX)["xex"]["title_id"], "45410813")


@unittest.skipUnless(os.path.isdir(TRIALS_PKG_DIR), "paquet Trials Fusion absent")
class RealTrials(unittest.TestCase):
    def setUp(self):
        self.pkg = os.path.join(TRIALS_PKG_DIR, os.listdir(TRIALS_PKG_DIR)[0])

    def test_detect_info(self):
        self.assertEqual(xc.detect(self.pkg), "stfs")
        c = xc.open(self.pkg)
        m = c.metadata()
        self.assertEqual((m["magic"], m["title_id"], m["content_type"]), ("LIVE", "58411401", 0xD0000))
        self.assertEqual(m["title_name"], "Trials Fusion")
        self.assertEqual(xc.info(self.pkg)["title"]["name"], "Trials Fusion")

    def test_blocs_hachage(self):
        """Tous les blocs de données de fichiers au-delà de 170 puis 28900 blocs correspondent à leur SHA-1 L0."""
        c = xc.open(self.pkg)
        fs = c.fs
        checked = 0
        for e in fs.entries:
            if e["dir"] or e["size"] < 0x1000:
                continue
            ch = list(fs.block_chain(e))
            for b in (ch[0], ch[len(ch) // 2], ch[-1]):     # début, milieu, fin de chaque fichier
                self.assertTrue(fs.verify_block(b), (e["path"], b))
                checked += 1
        self.assertGreater(checked, 100)
        self.assertGreater(fs.total_blocks, 28900)

    @unittest.skipUnless(os.path.isfile(TRIALS_XEX), "default.xex de référence absent")
    def test_xex_identique(self):
        c = xc.open(self.pkg)
        h = hashlib.sha256()
        for b in c.iter_file("default.xex"):
            h.update(b)
        with open(TRIALS_XEX, "rb") as f:
            ref = hashlib.sha256(f.read()).hexdigest()
        self.assertEqual(h.hexdigest(), ref)


@unittest.skipUnless(os.path.isfile(FIRMWARE_PIRS), "firmware absent")
class RealFirmware(unittest.TestCase):
    def test_pas_un_jeu(self):
        c = xc.open(FIRMWARE_PIRS)
        self.assertEqual(c.metadata()["magic"], "PIRS")
        self.assertEqual(c.metadata()["content_type"], 0xB0000)
        self.assertNotIn(0xB0000, xc.GAME_TYPES)


@unittest.skipUnless(os.path.isfile(SKATE_XEX), "Skate absent")
class SyntheticISO(TmpCase):
    def make_iso(self, base):
        p = os.path.join(self.tmp, "t.iso")
        with open(SKATE_XEX, "rb") as f:
            xex = f.read()
        with open(p, "wb") as o:
            write_xdvdfs(o, {"default.xex": xex, "data/a.bin": b"alpha" * 1000, "data/sub/b.bin": b"beta",
                             "Media/z.txt": b"z"}, base=base)
        return p, xex

    def test_offsets(self):
        for base in (0, 0x0FD90000, 0x02080000, 0x18300000):
            with self.subTest(base=hex(base)):
                p, xex = self.make_iso(base)
                self.assertEqual(xc.detect(p), "xdvdfs")
                c = xc.open(p)
                self.assertEqual(c.fs.base, base)
                self.assertEqual(c.read("default.xex"), xex)
                self.assertEqual(c.read("DATA/sub/b.bin"), b"beta")
                self.assertEqual(c.read("data/a.bin"), b"alpha" * 1000)
                self.assertEqual(c.xex_info()["title_id"], "45410813")
                os.remove(p)

    def test_extract_idempotent(self):
        p, xex = self.make_iso(0x02080000)
        dest = os.path.join(self.tmp, "out")
        seen = []
        c = xc.open(p)
        self.assertEqual(c.extract(dest, lambda d, t, n: seen.append(n)), dest)
        self.assertEqual(rfile(os.path.join(dest, "default.xex")), xex)
        self.assertEqual(rfile(os.path.join(dest, "data/sub/b.bin")), b"beta")
        self.assertTrue(os.path.isfile(os.path.join(dest, ".extracted.ok")))
        os.remove(os.path.join(dest, "data/a.bin"))
        c.extract(dest)                                  # marqueur valide : rien n'est refait
        self.assertFalse(os.path.exists(os.path.join(dest, "data/a.bin")))
        c.close()

    def test_non_iso(self):
        p = os.path.join(self.tmp, "x.bin")
        wfile(p, os.urandom(5000))
        self.assertIsNone(xc.detect(p))


@unittest.skipUnless(os.path.isfile(SKATE_XEX), "Skate absent")
class SyntheticGOD(TmpCase):
    def test_god_deux_parties(self):
        img = os.path.join(self.tmp, "img.bin")
        with open(SKATE_XEX, "rb") as f:
            xex = f.read()
        tail = os.urandom(300000)
        with open(img, "wb") as o:
            # le remplissage de 0xA1C4 blocs (une partie) rejette tail dans Data0001
            write_xdvdfs(o, {"default.xex": xex, "filler.bin": 0xA1C4 * 0x1000, "zz/tail.bin": tail})
        hdr = os.path.join(self.tmp, "ABCDEF0123456789")
        ddir = write_god(hdr, img)
        os.remove(img)
        self.assertEqual(sorted(os.listdir(ddir)), ["Data0000", "Data0001"])
        self.assertEqual(xc.detect(hdr), "god")
        c = xc.open(hdr)
        self.assertEqual(c.metadata()["title_id"], "45410813")
        self.assertEqual(c.metadata()["content_type"], 0x7000)
        self.assertEqual(c.read("default.xex"), xex)
        self.assertEqual(c.read("zz/tail.bin"), tail)
        self.assertEqual(c.xex_info()["title_id"], "45410813")
        names = [f["name"] for f in c.list()]
        self.assertIn("filler.bin", names)
        c.close()

    def test_god_sans_data(self):
        hdr = os.path.join(self.tmp, "H")
        wfile(hdr, header_bytes(descriptor_type=1))
        self.assertIsNone(xc.detect(hdr))


class SyntheticSTFS(TmpCase):
    def test_gros_fichier_et_chaine(self):
        big = os.urandom(0x1000 * 400 + 123)         # 401 blocs : franchit la limite de 170 et la table L1
        small = b"hello" * 100
        shuffled = os.urandom(0x1000 * 5 + 7)
        b = StfsBuilder(b"LIVE")
        b.add("default.xex", b"XEX2" + os.urandom(100))
        b.add("big.bin", big)
        b.add("small.txt", small)
        b.add("scatter.bin", shuffled, consecutive=False)
        p = os.path.join(self.tmp, "pkg")
        b.build(p)
        self.assertEqual(xc.detect(p), "stfs")
        c = xc.open(p)
        self.assertEqual(c.read("big.bin"), big)
        self.assertEqual(c.read("small.txt"), small)
        self.assertEqual(c.read("scatter.bin"), shuffled)
        self.assertEqual(c.metadata()["title_name"], "Synth STFS")
        self.assertEqual(c.title_fallback()[1][:4], b"\x89PNG")
        self.assertTrue(c.fs.verify_block(c.fs.block_chain(c.fs.files()["big.bin"])[300]))


class Robustesse(TmpCase):
    def test_fichiers_invalides(self):
        for name, data in (("vide", b""), ("xex_court", b"XEX2"), ("stfs_court", b"LIVE" + b"\0" * 100),
                           ("stfs_zero", b"LIVE" + b"\0" * 0x9800)):
            p = os.path.join(self.tmp, name)
            wfile(p, data)
            try:
                xc.info(p, decode=False)
            except (xc.FormatError, OSError):
                pass     # erreur propre
        self.assertIsNone(xc.detect(os.path.join(self.tmp, "vide")))


if __name__ == "__main__":
    unittest.main()
