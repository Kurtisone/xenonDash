#!/usr/bin/env python3
"""Import au premier lancement de l'AppImage : mise à jour officielle -> game_data_root.

Réutilise les étapes `extract` et `assets` de tools/port_dashboard.py (copié dans l'AppImage avec
tools/stfs.py, xex_resource.py, xzp_alias.py, xex_rekey.py, dash_shaders.py, hud_shaders.py et l'outil xexinfo).
Python 3 système requis.

Avec --native-shaders (AppImage native, 13604), génère aussi les shaders SPIR-V du rendu natif, à partir des
fichiers importés et des outils de l'AppImage (XenosRecomp et dxc : XD_XENOSRECOMP, xxhsum : XD_XXHSUM, définis par AppRun) :
  <root>/shaders/dash  <- dash.xex (conteneurs 0x102A11xx)
  <root>/shaders/hud   <- xam.xex de la mise à jour, copié à l'import (conteneurs 0x102A0E0x du HUD ; rechiffré si besoin)
Idempotent : <part>.stamp décrit les entrées (empreintes du XEX, de XenosRecomp, version du format) ; une génération n'est
refaite que si elles changent. Un échec est signalé (ligne « ÉCHEC ») sans interrompre l'import ; les shaders non traduits
sont listés dans <root>/shaders/<part>-echecs.txt.

usage: xenondash_import.py <game_data_root> <mise_à_jour> [--modules <dossier XeBuilds>]
                           [--shrdres-xam <xam.xex complet>] [--games-example <games.example.toml>] [--native-shaders]
       xenondash_import.py <game_data_root> --native-shaders   génération seule, si nécessaire
       xenondash_import.py <game_data_root> --regen-shaders    génération seule, forcée
"""
import argparse
import hashlib
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import port_dashboard as pd  # noqa: E402  (ROOT = dossier parent de tools/)
import dash_shaders as ds  # noqa: E402
import hud_shaders as hs  # noqa: E402

# À incrémenter si le format des shaders générés change (force une régénération chez les utilisateurs).
SHADER_VERSION = "1"


def say(msg):
    print(f"[shaders] {msg}", flush=True)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def generate_part(root, part, xex, xenos_root, tmp, force):
    """Génère <root>/shaders/<part> à partir de `xex` (dash.xex ou xam.xex). Lève pd.StageError en cas d'échec."""
    dest = os.path.join(root, "shaders", part)
    stamp_path = dest + ".stamp"
    xenos_bin = os.path.join(xenos_root, "build", "XenosRecomp", "XenosRecomp")
    stamp = "|".join([sha(xex), sha(xenos_bin), f"v{SHADER_VERSION}"])
    if not force and os.path.isfile(stamp_path) and os.path.isdir(dest):
        if open(stamp_path).read().strip() == stamp:
            say(f"{part} : déjà généré ({len(os.listdir(dest))} fichiers)")
            return
    say(f"{part} : génération depuis {os.path.basename(xex)} (XenosRecomp {xenos_root})")
    bins = os.path.join(tmp, f"{part}-bins")
    new = os.path.join(root, "shaders", f".new-{part}")
    shutil.rmtree(new, ignore_errors=True)
    if os.path.exists(stamp_path):
        os.remove(stamp_path)
    if part == "dash":
        src = xex
        pe = os.path.join(tmp, "dash.pe")
        pd.run([pd.XEXINFO, src, pe], log_path=os.path.join(tmp, "dash_xexinfo.txt"))
        ds.extract(pe, bins)
        done, fails = ds.translate(bins, new, xenos_root, log=say)
    else:
        src = os.path.join(tmp, "xam_rekey.xex")
        pd.run([sys.executable, os.path.join(HERE, "xex_rekey.py"), xex, src])
        pe = os.path.join(tmp, "xam.pe")
        pd.run([pd.XEXINFO, src, pe], log_path=os.path.join(tmp, "xam_xexinfo.txt"))
        hs.extract(pe, bins)
        done, fails = hs.translate(bins, new, xenos_root)
    if done == 0:
        raise pd.StageError("aucun shader traduit (voir ci-dessus)")
    with open(os.path.join(root, "shaders", f"{part}-echecs.txt"), "w") as f:
        for name, stage, cause in fails:
            f.write(f"{name}\t{stage}\t{cause}\n")
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(new, dest)
    with open(stamp_path, "w") as f:
        f.write(stamp + "\n")
    say(f"{part} : {done} shaders dans {dest}, {len(fails)} non traduit(s) (liste : {part}-echecs.txt)")
    for name, stage, cause in fails[:5]:
        say(f"  non traduit : {name} ({stage}) {cause}")


def generate_native_shaders(root, force=False):
    """Génère les shaders des deux parties. Renvoie la liste des parties en échec (l'import continue malgré un échec)."""
    xenos = ds.xenos_root()
    miss = ds.xenos_missing(xenos)
    if miss:
        say(f"ÉCHEC : XenosRecomp incomplet sous {xenos} (manque : {', '.join(miss)}) : tools/build_xenosrecomp.sh ou XD_XENOSRECOMP")
        return ["dash", "hud"]
    if not os.environ.get("XD_XXHSUM") and not shutil.which("xxhsum"):
        say("ÉCHEC : xxhsum introuvable (XD_XXHSUM=<chemin>)")
        return ["dash", "hud"]
    failed = []
    for part, xex in (("dash", os.path.join(root, "dash.xex")), ("hud", os.path.join(root, "xam.xex"))):
        if not os.path.isfile(xex):
            say(f"{part} : {os.path.basename(xex)} absent de la mise à jour : shaders non générés")
            failed.append(part)
            continue
        os.makedirs(os.path.join(root, "shaders"), exist_ok=True)
        tmp = tempfile.mkdtemp(prefix=f"xenondash-{part}-")
        try:
            generate_part(root, part, xex, xenos, tmp, force)
        except (pd.StageError, OSError, RuntimeError) as e:
            say(f"ÉCHEC {part} : {e}")
            failed.append(part)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
            shutil.rmtree(os.path.join(root, "shaders", f".new-{part}"), ignore_errors=True)
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="game_data_root à remplir (~/.local/share/xenondash/<version>)")
    ap.add_argument("update", nargs="?", help="dossier $SystemUpdate / mise à jour extraite, paquet STFS ou dash.xex")
    ap.add_argument("--modules", help="modules et polices complets de la version (archive XeBuilds)")
    ap.add_argument("--shrdres-xam", help="xam.xex complet (mise à jour récente, ex. 17559) d'où tirer shrdres.xzp")
    ap.add_argument("--games-example", help="modèle de games.toml")
    ap.add_argument("--native-shaders", action="store_true", help="générer les shaders SPIR-V du rendu natif (si nécessaire)")
    ap.add_argument("--regen-shaders", action="store_true", help="régénérer les shaders SPIR-V du rendu natif (sans import)")
    a = ap.parse_args()

    root = os.path.abspath(a.root)
    if a.regen_shaders or (a.native_shaders and not a.update):
        if not os.path.isfile(os.path.join(root, "dash.xex")):
            sys.exit("[shaders] dash.xex absent : importer d'abord la mise à jour (AppImage --import <mise à jour>)")
        failed = generate_native_shaders(root, force=a.regen_shaders)
        sys.exit(1 if failed else 0)
    if not a.update:
        ap.error("mise à jour requise (ou --native-shaders / --regen-shaders)")

    os.makedirs(root, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="xenondash-import-")
    try:
        ns = types.SimpleNamespace(source=a.update, flash_dir=a.modules, shrdres=None, shrdres_xam=a.shrdres_xam)
        ctx = {"dest": os.path.join(tmp, "proj"), "work": os.path.join(tmp, "work")}
        os.makedirs(ctx["work"])
        for name, fn in (("extract", pd.stage_extract), ("assets", pd.stage_assets)):
            pd.log(f"=== {name} ===")
            fn(ns, ctx)
        src = os.path.join(ctx["dest"], "assets")
        stage = root + ".import"
        shutil.rmtree(stage, ignore_errors=True)
        shutil.copytree(src, stage)
        for d in ("dash.xex", "flash"):  # remplace seulement les fichiers importés (profils et games.toml conservés)
            dst = os.path.join(root, d)
            if os.path.isdir(dst):
                shutil.rmtree(dst)
            elif os.path.exists(dst):
                os.remove(dst)
            shutil.move(os.path.join(stage, d), dst)
        shutil.rmtree(stage, ignore_errors=True)
        # xam.xex de la mise à jour elle-même (même version que le Dashboard) : source des shaders du HUD
        own_xam = next((f for f in ctx["files"] if os.path.basename(f).lower() == "$flash_xam.xex"), None)
        if own_xam:
            shutil.copyfile(own_xam, os.path.join(root, "xam.xex"))
        elif os.path.exists(os.path.join(root, "xam.xex")):
            os.remove(os.path.join(root, "xam.xex"))
            pd.log("avertissement : pas de $flash_xam.xex dans la mise à jour : shaders du HUD non générés")
    except pd.StageError as e:
        sys.exit(f"[import] ÉCHEC : {e}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    os.makedirs(os.path.join(root, "profiles"), exist_ok=True)  # profil par défaut : créé vierge par le Dashboard
    games = os.path.join(root, "games.toml")
    if not os.path.exists(games):
        with open(games, "w") as f:
            f.write("# Registre des jeux (voir games.example.toml). Décommenter / copier des entrées [game.<TitleID>].\n"
                    "library = true\n")
    if a.games_example and os.path.exists(a.games_example):
        shutil.copy(a.games_example, os.path.join(root, "games.example.toml"))
    if not os.path.exists(os.path.join(root, "flash", "shrdres.xzp")):
        pd.log("avertissement : pas de shrdres.xzp (--shrdres-xam absent) : icônes et chaînes partagées manquantes")
    with open(os.path.join(root, ".ready"), "w") as f:
        f.write("ok\n")
    pd.log(f"import terminé : {root}")
    if a.native_shaders:
        # Un échec de génération n'annule pas l'import : le Dashboard signale les shaders manquants au démarrage.
        generate_native_shaders(root)


if __name__ == "__main__":
    main()
