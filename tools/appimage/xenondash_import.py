#!/usr/bin/env python3
"""Import au premier lancement de l'AppImage : mise à jour officielle -> game_data_root.

Réutilise les étapes `extract` et `assets` de tools/port_dashboard.py (copié dans l'AppImage avec
tools/stfs.py, xex_resource.py, xzp_alias.py et l'outil xexinfo). Python 3 système requis.

usage: xenondash_import.py <game_data_root> <mise_à_jour> [--modules <dossier XeBuilds>]
                           [--shrdres-xam <xam.xex complet>] [--games-example <games.example.toml>]
"""
import argparse
import os
import shutil
import sys
import tempfile
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import port_dashboard as pd  # noqa: E402  (ROOT = dossier parent de tools/)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="game_data_root à remplir (~/.local/share/xenondash/<version>)")
    ap.add_argument("update", help="dossier $SystemUpdate / mise à jour extraite, paquet STFS ou dash.xex")
    ap.add_argument("--modules", help="modules et polices complets de la version (archive XeBuilds)")
    ap.add_argument("--shrdres-xam", help="xam.xex complet (mise à jour récente, ex. 17559) d'où tirer shrdres.xzp")
    ap.add_argument("--games-example", help="modèle de games.toml")
    a = ap.parse_args()

    root = os.path.abspath(a.root)
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


if __name__ == "__main__":
    main()
