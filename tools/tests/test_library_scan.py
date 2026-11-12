# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests de tools/game_library.py : init, scan (dossiers, liens symboliques, ISO, GOD, non-jeux, erreurs), cache, toml.

Lancer : python3 -m unittest discover -s tools/tests -p 'test_library_*.py'   (depuis la racine du dépôt)
Écrit uniquement dans un dossier temporaire (XD_TEST_TMP ou tempfile) ; XDG_DATA_HOME et XENONDASH_GAMES_DIR y pointent.
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import tomllib
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import game_library as gl  # noqa: E402
import test_library_formats as tf  # noqa: E402
import xcontainer as xc  # noqa: E402


def patched_xex(title_id):
    """Copie en mémoire du XEX de Skate dont le TitleID de l'en-tête d'exécution est changé."""
    x = bytearray(tf.rfile(tf.SKATE_XEX))
    n = struct.unpack_from(">I", x, 0x14)[0]
    for i in range(n):
        k, v = struct.unpack_from(">II", x, 0x18 + 8 * i)
        if k == 0x40006:
            struct.pack_into(">I", x, v + 12, title_id)
    return bytes(x)


@unittest.skipUnless(os.path.isfile(tf.SKATE_XEX) and os.path.isdir(tf.TRIALS_PKG_DIR), "jeux réels absents")
class Scan(tf.TmpCase):
    def setUp(self):
        super().setUp()
        self.games = os.path.join(self.tmp, "Games")
        self.data = os.path.join(self.tmp, "data")
        self.out = os.path.join(self.data, "xenondash", "library")
        os.environ["XDG_DATA_HOME"] = self.data
        os.environ["XENONDASH_GAMES_DIR"] = self.games
        self.addCleanup(os.environ.pop, "XDG_DATA_HOME", None)
        self.addCleanup(os.environ.pop, "XENONDASH_GAMES_DIR", None)
        gl.init()
        # liens symboliques vers les jeux réels (lecture seule)
        os.symlink(tf.SKATE_DIR, os.path.join(self.games, "Skate"))
        os.symlink(os.path.dirname(os.path.dirname(tf.TRIALS_PKG_DIR)), os.path.join(self.games, "hdd"))
        sub = os.path.join(self.games, "disques")
        os.makedirs(sub)
        # ISO synthétique d'un second titre (45410899), à un décalage XGD3
        with open(os.path.join(sub, "autre.iso"), "wb") as o:
            tf.write_xdvdfs(o, {"default.xex": patched_xex(0x45410899), "data/x.bin": b"x"}, base=0x02080000)
        # GOD synthétique du même XEX que Skate : doublon de TitleID
        img = os.path.join(self.tmp, "img.bin")
        with open(img, "wb") as o:
            tf.write_xdvdfs(o, {"default.xex": tf.rfile(tf.SKATE_XEX)})
        tf.write_god(os.path.join(sub, "0123456789ABCDEF"), img, title="Skate GOD")
        os.remove(img)
        # non-jeux et fichiers douteux
        if os.path.isfile(tf.FIRMWARE_PIRS):
            os.symlink(tf.FIRMWARE_PIRS, os.path.join(sub, "mise_a_jour_systeme"))
        tf.wfile(os.path.join(sub, "notes.txt"), b"rien")
        tf.wfile(os.path.join(sub, "corrompu"), b"LIVE" + b"\0" * 0x9800)
        os.symlink(self.games, os.path.join(sub, "boucle"))       # boucle de liens : ne doit pas bloquer

    def test_scan(self):
        lib = gl.scan()
        self.assertEqual(sorted(lib["games"]), ["45410813", "45410899", "58411401"])
        sk = lib["games"]["45410813"]
        self.assertEqual((sk["name"], sk["format"], sk["version"], sk["type"]), ("skate.", "dir", "0.0.0.3", "arcade"))
        self.assertEqual([a["format"] for a in sk["alternates"]], ["god"])
        tr = lib["games"]["58411401"]
        self.assertEqual((tr["name"], tr["format"], tr["type"]), ("Trials Fusion", "stfs", "arcade"))
        self.assertEqual(lib["games"]["45410899"]["format"], "xdvdfs")
        for g in lib["games"].values():
            self.assertTrue(os.path.isabs(g["source"]) and os.path.isabs(g["icon"]))
            self.assertEqual(tf.rfile(g["icon"])[:4], b"\x89PNG")
        reasons = [n["reason"] for n in lib["not_games"]]
        if os.path.isfile(tf.FIRMWARE_PIRS):
            self.assertTrue(any("mise à jour" in r for r in reasons), reasons)
        self.assertTrue(any(e["source"].endswith("corrompu") for e in lib["errors"]) or
                        any(n["source"].endswith("corrompu") for n in lib["not_games"]))
        self.assertTrue(any(u.endswith("notes.txt") for u in lib["unrecognized"]))
        # fichiers produits
        with open(os.path.join(self.out, "games.auto.toml"), "rb") as f:
            t = tomllib.load(f)
        self.assertIs(t["library"], True)
        self.assertEqual(t["game"]["58411401"]["name"], "Trials Fusion")
        self.assertEqual(t["game"]["45410813"]["recipe"], os.path.join(self.games, "Skate"))
        self.assertEqual(t["game"]["45410813"]["icon"], os.path.join(self.out, "icons", "45410813.png"))
        with open(os.path.join(self.out, "library.json")) as f:
            j = json.load(f)
        self.assertEqual(j["schema"], 1)
        self.assertIn("sources", j)

    def test_cache_incremental(self):
        gl.scan()
        t = time.time()
        lib = gl.scan()
        dt = time.time() - t
        self.assertEqual(lib["sources_reused"], lib["sources_total"])
        self.assertLess(dt, 1.0)
        # un fichier modifié est relu, le reste reste en cache
        p = os.path.join(self.games, "disques", "notes.txt")
        tf.wfile(p, b"change")
        os.utime(p, (time.time() + 5, time.time() + 5))
        lib = gl.scan()
        self.assertEqual(lib["sources_reused"], lib["sources_total"] - 1)
        # un jeu retiré disparaît, icône comprise
        os.remove(os.path.join(self.games, "disques", "autre.iso"))
        lib = gl.scan()
        self.assertNotIn("45410899", lib["games"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "icons", "45410899.png")))

    def test_cli(self):
        env = dict(os.environ)
        r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "game_library.py"), "scan", "--json"],
                           capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("45410813", json.loads(r.stdout)["games"])
        r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(HERE), "game_library.py"), "list"],
                           capture_output=True, text=True, env=env)
        self.assertIn("Trials Fusion", r.stdout)


class Init(tf.TmpCase):
    def test_init(self):
        d = os.path.join(self.tmp, "a", "Games")
        r = gl.init(d)
        self.assertTrue(r["readme_created"])
        self.assertTrue(os.path.isfile(os.path.join(d, "LISEZMOI.txt")))
        self.assertFalse(gl.init(d)["readme_created"])
        self.assertEqual(gl.scan(d, os.path.join(self.tmp, "out"))["games"], {})


if __name__ == "__main__":
    unittest.main()
