#!/usr/bin/env python3
"""Import au premier lancement de l'AppImage : mise à jour officielle -> game_data_root.

Réutilise les étapes `extract` et `assets` de tools/port_dashboard.py (copié dans l'AppImage avec
tools/stfs.py, xex_resource.py, xzp_alias.py, xex_rekey.py, dash_shaders.py, hud_shaders.py et l'outil xexinfo).
Python 3 système requis.

Avec --native-shaders (AppImage native, 13604), génère aussi les shaders SPIR-V du rendu natif, à partir des
fichiers importés et des outils de l'AppImage (XenosRecomp et dxc : XD_XENOSRECOMP, xxhsum : XD_XXHSUM, définis par AppRun) :
  <root>/shaders/dash  <- dash.xex (conteneurs 0x102A11xx)
  <root>/shaders/avatar <- SystemExtPartition/20352400/AvatarEditor.xex (éditeur d'avatar, 2e exécutable ; absent : ignoré)
  <root>/shaders/hud   <- xam.xex de la mise à jour, copié à l'import (conteneurs 0x102A0E0x du HUD ; rechiffré si besoin)
Idempotent : <part>.stamp décrit les entrées (empreintes du XEX, de XenosRecomp, version du format) ; une génération n'est
refaite que si elles changent. Un échec est signalé (ligne « ÉCHEC ») sans interrompre l'import ; les shaders non traduits
sont listés dans <root>/shaders/<part>-echecs.txt.

usage: xenondash_import.py <game_data_root> <mise_à_jour> [--modules <dossier XeBuilds>]
                           [--shrdres-xam <xam.xex complet>] [--games-example <games.example.toml>] [--native-shaders]
                           [--system-ext <dossier>]
       xenondash_import.py <game_data_root> <mise_à_jour> --system-ext-only   partition système étendue seule
       xenondash_import.py <game_data_root> --native-shaders   génération seule, si nécessaire
       xenondash_import.py <game_data_root> --regen-shaders    génération seule, forcée
"""
import argparse
import hashlib
import os
import re
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
SHADER_VERSION = "2"


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
    if part in ("dash", "avatar"):  # mêmes conteneurs de shaders (D3D 2.0.20158 identique)
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
    avatar_xex = os.path.join(root, "SystemExtPartition", "20352400", "AvatarEditor.xex")
    for part, xex in (("dash", os.path.join(root, "dash.xex")), ("hud", os.path.join(root, "xam.xex")), ("avatar", avatar_xex)):
        if part == "avatar" and not os.path.isfile(xex):
            say("avatar : AvatarEditor.xex absent de la partition système étendue : shaders de l'éditeur non générés (Customize Avatar : MiniCreator)")
            continue
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


def build_system_ext(update, dst):
    """Reconstruit la partition système étendue de XAM à partir d'un dossier $SystemUpdate à plat (mise à jour dézippée).

    Disposition (relevée dans les accès de XAM et vérifiée à l'exécution : index des paquets rempli, création d'avatar) :
      system.manifest                            -> <dst>/system.manifest
      FFFE07DF0000000{1,2,6,8}                   -> <dst>/Content/0000000000000000/FFFE07DF/00008000/
      Xna_TitleLauncher.xex                      -> <dst>/32000100/
      autres .xex / .lex / .xzp, su20076000_*    -> <dst>/20352400/
    Construit dans <dst>.new puis remplace <dst> à la fin seulement : un échec laisse l'ancienne partition intacte.
    Renvoie le nombre de fichiers copiés (0 si system.manifest est absent : rien n'est touché).
    """
    manifest = os.path.join(update, "system.manifest")
    if not os.path.isfile(manifest):
        pd.log(f"ERREUR : pas de system.manifest dans {update} : partition système étendue non construite "
               "(index des paquets XAM vide : création d'avatar indisponible)")
        return 0
    new = dst + ".new"
    shutil.rmtree(new, ignore_errors=True)
    try:
        os.makedirs(new)
        shutil.copy2(manifest, os.path.join(new, "system.manifest"))
        n = 1
        for name in sorted(os.listdir(update)):
            src = os.path.join(update, name)
            if name == "system.manifest" or not os.path.isfile(src):
                continue
            if re.fullmatch(r"FFFE07DF0000000[1268]", name):
                sub = os.path.join("Content", "0000000000000000", "FFFE07DF", "00008000")
            elif name == "Xna_TitleLauncher.xex":
                sub = "32000100"
            elif name.lower().endswith((".xex", ".lex", ".xzp")) or name.startswith("su20076000_"):
                sub = "20352400"
            else:
                continue
            os.makedirs(os.path.join(new, sub), exist_ok=True)
            shutil.copy2(src, os.path.join(new, sub, name))
            n += 1
    except OSError:
        shutil.rmtree(new, ignore_errors=True)
        raise
    replace_dir(new, dst)
    return n


def replace_dir(new, dst):
    """Remplace dst par new ; l'ancien dst n'est supprimé qu'une fois new en place."""
    old = dst + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.lexists(dst):
        os.replace(dst, old)
    os.replace(new, dst)
    shutil.rmtree(old, ignore_errors=True)


def system_ext_source(update):
    """Dossier d'où construire la partition : la mise à jour si c'est un dossier.

    Un paquet STFS su20076000_* ne contient pas system.manifest ni les modules de la partition (seulement les
    $flash_*, cf. tools/stfs.py) : l'extraire ne suffit pas. On prend alors le dossier qui le contient s'il porte
    system.manifest (paquet laissé dans son $SystemUpdate) ; sinon None (partition impossible à construire).
    """
    if os.path.isdir(update):
        return update
    parent = os.path.dirname(os.path.abspath(update))
    if os.path.isfile(os.path.join(parent, "system.manifest")):
        return parent
    return None


def import_system_ext(root, update, system_ext=None):
    """Installe <root>/SystemExtPartition : copie d'un dossier fourni (--system-ext), sinon construit depuis la mise à jour.

    XAM lit son index des paquets (Guide.AvatarMiniCreator.xex, etc.) dans system.manifest de cette partition ; sans elle,
    la création d'avatar est sautée après la connexion.
    """
    dst = os.path.join(root, "SystemExtPartition")
    if system_ext:
        if os.path.isdir(system_ext):
            new = dst + ".new"
            shutil.rmtree(new, ignore_errors=True)
            shutil.copytree(system_ext, new)
            replace_dir(new, dst)
            pd.log(f"partition système étendue copiée depuis {system_ext}")
            return
        pd.log(f"avertissement : --system-ext {system_ext} n'est pas un dossier : ignoré, construction depuis la mise à jour")
    if os.path.isdir(update) and os.path.isdir(os.path.join(update, "SystemExtPartition")):
        return import_system_ext(root, update, os.path.join(update, "SystemExtPartition"))
    src = system_ext_source(update)
    if src is None:
        pd.log(f"ERREUR : {update} est un paquet STFS (ou un fichier isolé) sans system.manifest à côté : partition "
               "système étendue non construite, création d'avatar indisponible. Importer le dossier $SystemUpdate "
               "complet (mise à jour dézippée) pour l'obtenir.")
        return
    n = build_system_ext(src, dst)
    if n:
        pd.log(f"partition système étendue construite depuis {src} : {n} fichiers dans {dst}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("root", help="game_data_root à remplir (~/.local/share/xenondash/<version>)")
    ap.add_argument("update", nargs="?", help="dossier $SystemUpdate / mise à jour extraite, paquet STFS ou dash.xex")
    ap.add_argument("--modules", help="modules et polices complets de la version (archive XeBuilds)")
    ap.add_argument("--fonts-dir", help="dossier de polices XenonCLatin/XenonJKLatin/XenonSCLatin.xtt "
                                       "(sans les modules .xex d'une archive XeBuilds)")
    ap.add_argument("--shrdres-xam", help="xam.xex complet (mise à jour récente, ex. 17559) d'où tirer shrdres.xzp")
    ap.add_argument("--system-ext", help="partition système étendue (dossier avec system.manifest, 20352400/, 32000100/, "
                                         "Content/) ; par défaut <mise à jour>/SystemExtPartition si présent")
    ap.add_argument("--system-ext-only", action="store_true",
                    help="(re)construire seulement <root>/SystemExtPartition depuis la mise à jour (installations "
                         "importées avant la partition système étendue)")
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
    if a.system_ext_only:
        if not os.path.isfile(os.path.join(root, ".ready")):
            sys.exit("[import] --system-ext-only : données non importées sous " + root)
        import_system_ext(root, a.update, a.system_ext)
        sys.exit(0)

    os.makedirs(root, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="xenondash-import-")
    try:
        ns = types.SimpleNamespace(source=a.update, flash_dir=a.modules, fonts_dir=a.fonts_dir, shrdres=None,
                                   shrdres_xam=a.shrdres_xam)
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
        # xam.xex de la mise à jour elle-même (même version que le Dashboard) : source des shaders du HUD.
        # La racine des données doit contenir la version REKEYÉE (clé retail), pas le brut : le chargeur
        # résout game:\xam.xex tantôt vers la flash (montage dash::xamrt), tantôt vers la racine (montage
        # « game: » du noyau = répertoire de l'exécutable ; la correspondance des préfixes du VFS est
        # insensible à la casse et l'ordre de la map arbitre — le brut, clé de session chiffrée XEX1,
        # échouait au déchiffrement retail/devkit : « XEX load failed with code 2 », Guide mort ;
        # mesure strace du 2026-11-21 : le loader ouvrait <racine>/xam.xex, pas flash/xam.xex).
        own_xam = next((f for f in ctx["files"] if os.path.basename(f).lower() == "$flash_xam.xex"), None)
        root_xam = os.path.join(root, "xam.xex")
        flash_xam = os.path.join(root, "flash", "xam.xex")
        if os.path.isfile(flash_xam):
            shutil.copyfile(flash_xam, root_xam)  # rekeyé par stage_assets (port_dashboard)
        elif own_xam:
            shutil.copyfile(own_xam, root_xam)
        else:
            if os.path.exists(root_xam):
                os.remove(root_xam)
            pd.log("avertissement : pas de $flash_xam.xex dans la mise à jour : shaders du HUD non générés")
    except pd.StageError as e:
        sys.exit(f"[import] ÉCHEC : {e}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    import_system_ext(root, a.update, a.system_ext)
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
    if a.shrdres_xam and os.path.isfile(a.shrdres_xam) and not os.path.exists(os.path.join(root, "xam.xex")):
        # Les shaders HUD (interface XAM : Guide, boîtes) se traduisent depuis le xam.xex complet ;
        # generate_native_shaders() l'attend à la racine des données. Rekeyé (clé retail) : le
        # chargeur du Dashboard doit aussi pouvoir le déchiffrer où qu'il résolve game:\xam.xex.
        pd.run([sys.executable, os.path.join(pd.TOOLS, "xex_rekey.py"), a.shrdres_xam, os.path.join(root, "xam.xex")])
        pd.log("xam.xex (rekeyé) copié à la racine (--shrdres-xam) : shaders HUD générables, game:\\xam.xex lisible")
    with open(os.path.join(root, ".ready"), "w") as f:
        f.write("ok\n")
    pd.log(f"import terminé : {root}")
    if a.native_shaders:
        # Un échec de génération n'annule pas l'import : le Dashboard signale les shaders manquants au démarrage.
        generate_native_shaders(root)


if __name__ == "__main__":
    main()
