#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Moteur de recettes de XenonDash (P8, etape « Moteur de recettes »).

Voit un XEX de jeu, trouve sa recette `games/<TitleID>/`, le recompile en natif au premier lancement (cache), puis
donne la commande de lancement. Aucun fichier du jeu, shader ni code genere n'est ecrit dans le depot : tout vit dans le
cache `$XDG_DATA_HOME/xenondash/games/<TitleID>/<cle>/` (defaut ~/.local/share).

  recipe_build.py identify <default.xex>                identifie le XEX (TitleID, version, hachage de l'image decodee)
  recipe_build.py build    <default.xex|dossier|iso> [--force] [--stages a,b,...]   construit (ou reutilise) le cache
  recipe_build.py status   <default.xex|dossier|iso>    etat du cache (build.ok, cle, chemin, commande de lancement)
  recipe_build.py launch-info <TitleID> --source ...    JSON : commande + cwd du jeu construit (pour games.toml)
  recipe_build.py clean    <TitleID> [--all]            supprime le cache du jeu

Etapes : identify, prepare, extract, scaffold, codegen, shaders, compile, bootstrap.
La progression est ecrite dans `<cache>/progress.json` (et en lignes `[recipe] ...` sur la sortie) pour le Dashboard.
Format de la recette et flux : docs/recettes.md et docs/fusion_ports.md section 5.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAMES = os.path.join(ROOT, "games")
TOOLS = os.path.join(ROOT, "tools")
TEMPLATE = os.path.join(TOOLS, "templates", "game")
SDK = os.environ.get("XD_SDK", os.path.join(ROOT, "sdk"))
REXGLUE = os.path.join(SDK, "bin", "rexglue")
XENOSRECOMP = os.environ.get(
    "XD_XENOSRECOMP", os.path.join(ROOT, "work", "XenosRecomp-unified", "build", "XenosRecomp", "XenosRecomp"))
XENOSRECOMP_DIR = os.path.dirname(os.path.dirname(os.path.dirname(XENOSRECOMP)))
STAGES = ["identify", "prepare", "extract", "scaffold", "codegen", "shaders", "compile", "bootstrap"]
RECIPE_SCHEMA = 1


class RecipeError(Exception):
    pass


def data_home():
    return os.path.join(os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share"), "xenondash")


def log(msg):
    print(f"[recipe] {msg}", flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_tree(paths):
    """Hachage de fichiers (chemins relatifs inclus) : revision d'un ensemble de sources."""
    h = hashlib.sha256()
    for base, rel in paths:
        p = os.path.join(base, rel)
        h.update(rel.encode() + b"\0")
        with open(p, "rb") as f:
            h.update(f.read())
    return h.hexdigest()


def tree_files(root, skip=("__pycache__", ".git")):
    out = []
    for d, dirs, files in os.walk(root):
        dirs[:] = sorted(x for x in dirs if x not in skip)
        for f in sorted(files):
            out.append((root, os.path.relpath(os.path.join(d, f), root)))
    return out


# --------------------------------------------------------------------------------------------------
# Identification du XEX
# --------------------------------------------------------------------------------------------------
def xex_header(path):
    """TitleID, version, MediaID lus dans l'en-tete du XEX (en clair)."""
    with open(path, "rb") as f:
        head = f.read(0x40000)
    if head[:4] != b"XEX2":
        raise RecipeError(f"{path} n'est pas un XEX2")
    count = struct.unpack(">I", head[0x14:0x18])[0]
    opt = {}
    for i in range(count):
        k, v = struct.unpack(">II", head[0x18 + 8 * i:0x20 + 8 * i])
        opt[k] = v
    if 0x40006 not in opt:
        raise RecipeError("en-tete d'execution absent du XEX")
    media_id, version, base_version, title_id = struct.unpack(">IIII", head[opt[0x40006]:opt[0x40006] + 16])

    def ver(v):
        return f"{v >> 28}.{(v >> 24) & 0xF}.{(v >> 8) & 0xFFFF}.{v & 0xFF}"
    return {"title_id": f"{title_id:08X}", "media_id": f"{media_id:08X}", "version": ver(version),
            "base_version": ver(base_version)}


def decode_image(xex, work):
    """Image PE decodee (dechiffree/decompressee) par `rexglue dump-xex` ; renvoie (chemin, sha256)."""
    out = os.path.join(work, "image")
    shutil.rmtree(out, ignore_errors=True)
    r = subprocess.run([REXGLUE, "dump-xex", xex, out], env=dict(os.environ, LD_LIBRARY_PATH=os.path.join(SDK, "lib")),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    bins = [f for f in os.listdir(out)] if os.path.isdir(out) else []
    bins = [f for f in bins if f.endswith(".bin")]
    if r.returncode != 0 or not bins:
        raise RecipeError("rexglue dump-xex a echoue :\n" + r.stdout[-800:])
    p = os.path.join(out, bins[0])
    return p, sha256_file(p)


def identify(xex, work):
    info = xex_header(xex)
    image, digest = decode_image(xex, work)
    info["sha256_image"] = digest
    info["image"] = image
    return info


# --------------------------------------------------------------------------------------------------
# Recette
# --------------------------------------------------------------------------------------------------
def load_recipe(title_id):
    d = os.path.join(GAMES, title_id.upper())
    p = os.path.join(d, "recipe.toml")
    if not os.path.isfile(p):
        return None, d
    with open(p, "rb") as f:
        r = tomllib.load(f)
    if r.get("schema") != RECIPE_SCHEMA:
        raise RecipeError(f"{p} : schema {r.get('schema')} non gere (attendu {RECIPE_SCHEMA})")
    return r, d


def match_xex(recipe, info):
    for x in recipe.get("xex", []):
        if x["sha256_image"].lower() == info["sha256_image"].lower():
            return x
    return None


def sdk_id():
    h = hashlib.sha256()
    for p in (REXGLUE, os.path.join(SDK, "lib", "librexruntime.so")):
        if os.path.isfile(p):
            h.update(open(p, "rb").read())
    v = subprocess.run([REXGLUE, "--version"], env=dict(os.environ, LD_LIBRARY_PATH=os.path.join(SDK, "lib")),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout.strip().splitlines()
    return (v[-1] if v else "?"), h.hexdigest()[:12]


def xenosrecomp_id():
    pdir = os.path.join(ROOT, "patches", "xenosrecomp")
    h = hashlib.sha256()
    if os.path.isdir(pdir):
        for f in sorted(os.listdir(pdir)):
            h.update(open(os.path.join(pdir, f), "rb").read())
    if os.path.isfile(XENOSRECOMP):
        h.update(str(os.path.getsize(XENOSRECOMP)).encode())
    return h.hexdigest()[:12]


def recipe_codegen_rev(rdir, recipe):
    """Ce qui change le code genere : la section [codegen] de la recette et les fichiers de codegen."""
    h = hashlib.sha256()
    h.update(json.dumps(recipe.get("codegen", {}), sort_keys=True).encode())
    for name in recipe.get("codegen", {}).get("files", []):
        h.update(name.encode() + open(os.path.join(rdir, name), "rb").read())
    return h.hexdigest()[:12]


def recipe_rev(rdir):
    """Revision complete de la recette (tous ses fichiers) + du noyau de rendu : invalide la compilation incrementale."""
    files = [(rdir, r) for _, r in tree_files(rdir)]
    files += [(os.path.join(ROOT, "ngpu_core"), r) for _, r in tree_files(os.path.join(ROOT, "ngpu_core"))
              if not r.startswith("tools/")]
    files += [(TEMPLATE, r) for _, r in tree_files(TEMPLATE)]
    return sha256_tree(files)[:12]


def cache_key(info, recipe, rdir):
    sdk_ver, sdk_hash = sdk_id()
    parts = [info["sha256_image"], str(recipe["schema"]), sdk_hash, xenosrecomp_id(), recipe_codegen_rev(rdir, recipe)]
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:16], {
        "xex_image": info["sha256_image"], "sdk": sdk_ver, "sdk_hash": sdk_hash, "xenosrecomp": xenosrecomp_id(),
        "codegen_rev": parts[-1]}


# --------------------------------------------------------------------------------------------------
# Progression
# --------------------------------------------------------------------------------------------------
class Progress:
    """progress.json : lu par le Dashboard (superposition de construction) ; ecrit de facon atomique."""

    def __init__(self, cache, estimate):
        self.path = os.path.join(cache, "progress.json") if cache else None
        self.state = {"stage": "", "index": 0, "total": len(STAGES), "percent": 0.0, "message": "", "done": False,
                      "error": "", "pid": os.getpid(), "started": time.time(), "estimate_s": estimate, "stages": {}}

    def update(self, **kw):
        self.state.update(kw)
        self.state["elapsed_s"] = round(time.time() - self.state["started"], 1)
        if self.path:
            tmp = self.path + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.state, f)
            os.replace(tmp, self.path)

    def stage(self, name, message=""):
        if self.state["stage"] and self.state["stage"] != name:
            self.state["stages"][self.state["stage"]] = round(time.time() - self.state.get("stage_start", time.time()), 1)
        self.state["stage_start"] = time.time()
        idx = STAGES.index(name) if name in STAGES else 0
        self.update(stage=name, index=idx + 1, message=message, percent=100.0 * idx / len(STAGES))
        log(f"[{idx + 1}/{len(STAGES)}] {name} : {message}")

    def sub(self, fraction, message=None):
        """Avancement a l'interieur de l'etape courante (0..1)."""
        idx = self.state["index"] - 1
        pct = 100.0 * (idx + max(0.0, min(1.0, fraction))) / len(STAGES)
        kw = {"percent": round(pct, 1)}
        if message is not None:
            kw["message"] = message
        self.update(**kw)


def run_logged(cmd, log_path, cwd=None, env=None, on_line=None, check=True, timeout=None):
    """Execute une commande dont la sortie va dans log_path ; on_line(ligne) suit la progression."""
    e = dict(os.environ)
    if env:
        e.update(env)
    with open(log_path, "a") as lf:
        lf.write(f"\n$ {' '.join(map(str, cmd))}\n")
        lf.flush()
        p = subprocess.Popen(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             errors="replace", bufsize=1)
        t0 = time.time()
        for line in p.stdout:
            lf.write(line)
            if on_line:
                on_line(line)
            if timeout and time.time() - t0 > timeout:
                p.kill()
                break
        p.wait()
    if check and p.returncode != 0:
        tail = "".join(open(log_path, errors="replace").readlines()[-25:])
        raise RecipeError(f"commande en echec ({p.returncode}) : {' '.join(map(str, cmd))}\n{tail}")
    return p.returncode


# --------------------------------------------------------------------------------------------------
# Sources (dossier du jeu, ISO)
# --------------------------------------------------------------------------------------------------
def resolve_source(arg, cache_parent_for_iso):
    """Renvoie (dossier_du_jeu, chemin_default_xex). Un dossier contient default.xex ; une ISO est extraite (cache)."""
    arg = os.path.abspath(os.path.expanduser(arg))
    if os.path.isdir(arg):
        x = os.path.join(arg, "default.xex")
        if not os.path.isfile(x):
            raise RecipeError(f"{arg} ne contient pas default.xex")
        return arg, x
    if arg.lower().endswith(".xex") and os.path.isfile(arg):
        return os.path.dirname(arg), arg
    if arg.lower().endswith(".iso") and os.path.isfile(arg):
        out = os.path.join(cache_parent_for_iso, "iso-" + hashlib.sha256(arg.encode()).hexdigest()[:10])
        if not os.path.isfile(os.path.join(out, "default.xex")):
            os.makedirs(out, exist_ok=True)
            log(f"extraction de l'ISO {arg} -> {out}")
            r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "extract_iso.py"), arg, out])
            if r.returncode != 0:
                raise RecipeError("extraction de l'ISO echouee")
        return out, os.path.join(out, "default.xex")
    raise RecipeError(f"source non reconnue : {arg} (dossier avec default.xex, .xex ou .iso)")


# --------------------------------------------------------------------------------------------------
# Profil D3D -> en-tetes generes
# --------------------------------------------------------------------------------------------------
def gen_d3d_profile(rdir, recipe, dest):
    """d3d_profile.toml -> d3d_profile.generated.h (constantes) + d3d_hooks.generated.inc (crochets D3D)."""
    p = os.path.join(rdir, recipe.get("render", {}).get("profile", "d3d_profile.toml"))
    with open(p, "rb") as f:
        prof = tomllib.load(f)

    def addr(v):
        return int(v, 16) if isinstance(v, str) else int(v)
    h = ["// genere par tools/recipe_build.py depuis d3d_profile.toml : ne pas versionner",
         "#pragma once", "#include <cstdint>", "namespace xd_profile {"]
    h.append(f"constexpr uint32_t kDevice = 0x{addr(prof['device']['address']):08X}u;")
    h.append(f"constexpr uint32_t kStateSize = {prof['device'].get('state_size', 0x3400)};")
    for k, v in prof.get("functions", {}).items():
        h.append(f"constexpr uint32_t k_{k} = 0x{addr(v):08X}u;")
    for k, v in prof.get("constants", {}).items():
        if isinstance(v, int):
            h.append(f"constexpr uint32_t k_{k} = 0x{v:X}u;")
        elif isinstance(v, str):
            h.append(f"constexpr uint32_t k_{k} = 0x{addr(v):08X}u;")
    nl = prof.get("null_gpu", {})
    for k, v in nl.items():
        if isinstance(v, (int, str)):
            h.append(f"constexpr uint32_t kNull_{k} = 0x{addr(v):X}u;")
    pr = prof.get("profile", {})
    h.append("constexpr uint64_t kInterfaceVertexShaders[] = {" +
             ", ".join(f"0x{addr(x):016X}ull" for x in pr.get("interface_vertex_shaders", [])) + ", 0};")
    h.append("constexpr uint64_t kCornerVertexShaders[] = {" +
             ", ".join(f"0x{addr(x):016X}ull" for x in pr.get("corner_vertex_shaders", [])) + ", 0};")
    h.append(f"constexpr uint32_t kScenePitchMin = {pr.get('scene_pitch_min', 1024)};")
    h.append(f"constexpr uint32_t kScenePitchMax = {pr.get('scene_pitch_max', 1280)};")
    h.append("}  // namespace xd_profile")
    with open(os.path.join(dest, "d3d_profile.generated.h"), "w") as f:
        f.write("\n".join(h) + "\n")
    # crochets : chaque [[hook]] = adresse invitee -> appel du facade (avant/apres la fonction d'origine)
    inc = ["// genere par tools/recipe_build.py depuis d3d_profile.toml : ne pas versionner"]
    for hk in prof.get("hook", []):
        a = f"{addr(hk['address']):08X}"
        kind = hk.get("kind", "after")
        if kind == "after":    # appelle la fonction, puis transmet (arguments avant, r3 apres) au facade
            inc.append(f"REX_HOOK_RAW(sub_{a}) {{ const PPCContext before = ctx; __imp__sub_{a}(ctx, base);"
                       f" XdFrontOnCall(0x{a}u, before, ctx.r3.u32, base); }}")
        elif kind == "before":
            inc.append(f"REX_HOOK_RAW(sub_{a}) {{ XdFrontOnCall(0x{a}u, ctx, 0, base); __imp__sub_{a}(ctx, base); }}")
    with open(os.path.join(dest, "d3d_hooks.generated.inc"), "w") as f:
        f.write("\n".join(inc) + "\n")
    return prof


# --------------------------------------------------------------------------------------------------
# Etapes
# --------------------------------------------------------------------------------------------------
def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, str):
        return "'" + v + "'"
    return str(v)


def free_gb(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize / 1e9


def mem_gb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemTotal"):
            return int(line.split()[1]) / 1e6
    return 0


def mem_available_gb():
    for line in open("/proc/meminfo"):
        if line.startswith("MemAvailable"):
            return int(line.split()[1]) / 1e6
    return 0


class Build:
    def __init__(self, source_arg, force=False, stages=None, jobs=None, container=None):
        self.force = force
        self.stages = stages or STAGES
        self.jobs = jobs
        self.container = container
        self.source_arg = source_arg
        self.t = {}

    # -- etape identify
    def do_identify(self):
        home = data_home()
        os.makedirs(home, exist_ok=True)
        self.game_dir, self.xex = resolve_source(self.source_arg, os.path.join(home, "sources"))
        self.info = xex_header(self.xex)
        recipe, rdir = load_recipe(self.info["title_id"])
        if recipe is None:
            raise RecipeError(f"jeu {self.info['title_id']} sans recette (games/{self.info['title_id']}/) : "
                              "pas d'execution implicite ; la recompilation generique n'existe pas encore (P8, etape 6)")
        self.recipe, self.rdir = recipe, rdir
        tmp = os.path.join(home, "tmp-identify")
        os.makedirs(tmp, exist_ok=True)
        info = identify(self.xex, tmp)
        shutil.rmtree(tmp, ignore_errors=True)
        info["image"] = None
        self.info = info
        locked = match_xex(recipe, info)
        if not locked:
            known = ", ".join(x["sha256_image"][:12] for x in recipe.get("xex", []))
            raise RecipeError(f"XEX {info['title_id']} version {info['version']} (image {info['sha256_image'][:12]}) "
                              f"non reconnu par la recette (connus : {known}) ; mode generique non disponible")
        if locked.get("version") and locked["version"] != info["version"]:
            raise RecipeError(f"version de l'en-tete {info['version']} differente de la recette {locked['version']}")
        self.locked = locked
        self.key, self.keyparts = cache_key(info, recipe, rdir)
        self.cache = os.path.join(home, "games", info["title_id"], self.key)
        self.name = recipe.get("name", info["title_id"])
        return True

    def stamp(self, name):
        return os.path.join(self.cache, "stamps", name)

    def stamped(self, name, rev):
        p = self.stamp(name)
        return os.path.isfile(p) and open(p).read().strip() == rev

    def set_stamp(self, name, rev):
        os.makedirs(os.path.dirname(self.stamp(name)), exist_ok=True)
        open(self.stamp(name), "w").write(rev + "\n")

    def ok(self):
        p = os.path.join(self.cache, "build.ok")
        if not os.path.isfile(p):
            return False
        try:
            return json.load(open(p)).get("recipe_rev") == recipe_rev(self.rdir)
        except Exception:
            return False

    # -- etape prepare
    def do_prepare(self, prog):
        b = self.recipe.get("build", {})
        need_disk, need_ram = b.get("min_disk_gb", 40), b.get("min_ram_gb", 16)
        os.makedirs(self.cache, exist_ok=True)
        fd = free_gb(self.cache)
        errs = []
        if fd < need_disk:
            errs.append(f"espace disque : {fd:.0f} Go libres, {need_disk} Go necessaires")
        if mem_gb() < need_ram:
            log(f"avertissement : {mem_gb():.0f} Go de memoire, {need_ram} Go conseilles")
        for tool in ("clang", "clang++", "cmake", "ninja", "python3"):
            if not shutil.which(tool):
                errs.append(f"outil manquant : {tool}")
        for p, hint in ((REXGLUE, "SDK installe dans sdk/ (voir README)"),
                        (XENOSRECOMP, "XenosRecomp unifie : tools/build_xenosrecomp.sh")):
            if not os.path.isfile(p):
                errs.append(f"{p} manquant : {hint}")
        if self.container:
            errs.append("l'option conteneur (--container) est documentee (docs/recettes.md) mais non implementee ici")
        if errs:
            raise RecipeError("preparation impossible :\n  - " + "\n  - ".join(errs))
        log(f"{fd:.0f} Go libres, {mem_gb():.0f} Go de memoire, cache : {self.cache}")

    # -- etape extract
    def do_extract(self, prog):
        """Le jeu n'est jamais copie dans le depot ; le cache memorise sa source (dossier du jeu, par chemin)."""
        src = {"game_dir": self.game_dir, "xex": self.xex, "needs": self.recipe.get("extract", {}).get("needs", [])}
        for n in src["needs"]:
            if not os.path.exists(os.path.join(self.game_dir, n)):
                raise RecipeError(f"{n} absent de {self.game_dir}")
        json.dump(src, open(os.path.join(self.cache, "source.json"), "w"), indent=1)

    # -- etape scaffold
    def do_scaffold(self, prog):
        proj = os.path.join(self.cache, "project")
        os.makedirs(proj, exist_ok=True)
        r = self.recipe
        cg = r["codegen"]
        # sources du jeu (hooks) : copiees depuis la recette (resynchronisees a chaque build)
        shutil.rmtree(os.path.join(proj, "src"), ignore_errors=True)
        shutil.copytree(os.path.join(self.rdir, "src"), os.path.join(proj, "src"))
        shutil.rmtree(os.path.join(proj, "native"), ignore_errors=True)
        if os.path.isdir(os.path.join(self.rdir, "native")):
            shutil.copytree(os.path.join(self.rdir, "native"), os.path.join(proj, "native"))
        for n in cg.get("files", []):
            shutil.copy(os.path.join(self.rdir, n), os.path.join(proj, n))
        gen = os.path.join(proj, "gen")
        os.makedirs(gen, exist_ok=True)
        prof = gen_d3d_profile(self.rdir, r, gen)
        name = cg["project"]
        # manifeste ReXGlue
        includes = ", ".join(f'"{n}"' for n in cg.get("files", []))
        manifest = (f'[project]\nname = "{name}"\nsdk_version = "0.10.0"\ngame_root = "{self.game_dir}"\n\n'
                    f'[entrypoint]\nfile_path = "{self.xex}"\nout_directory_path = "{cg["out_directory"]}"\n'
                    f'includes = [{includes}]\n')
        open(os.path.join(proj, f"{name}_manifest.toml"), "w").write(manifest)
        # CMakeLists du jeu depuis le gabarit
        t = open(os.path.join(TEMPLATE, "CMakeLists.txt")).read()
        t = t.replace("@NAME@", name).replace("@XENONDASH_ROOT@", ROOT).replace("@OUT@", cg["out_directory"])
        open(os.path.join(proj, "CMakeLists.txt"), "w").write(t)
        # configuration d'execution : runtime de la recette + game_data_root absolu (le jeu reste ou il est)
        rt = open(os.path.join(self.rdir, r.get("runtime", {}).get("file", "runtime.toml"))).read()
        rt = re.sub(r"(?m)^game_data_root\s*=.*\n", "", rt)
        rt += f"\ngame_data_root = '{self.game_dir}'\n"
        if r.get("render", {}).get("mode") == "native":
            # reglages d'execution propres au rendu natif (d3d_profile.toml [runtime]) : ils remplacent ceux de runtime.toml
            for k, v in prof.get("runtime", {}).items():
                rt = re.sub(rf"(?m)^{k}\s*=.*\n", "", rt)
                rt += f"{k} = {toml_value(v)}\n"
        open(os.path.join(proj, f"{name}.runtime.toml"), "w").write(rt)

    # -- etape codegen
    def do_codegen(self, prog):
        proj = os.path.join(self.cache, "project")
        name = self.recipe["codegen"]["project"]
        out = os.path.join(proj, self.recipe["codegen"]["out_directory"])
        rev = self.keyparts["codegen_rev"] + self.keyparts["sdk_hash"]
        if self.stamped("codegen", rev) and os.path.isfile(os.path.join(out, "sources.cmake")) and not self.force:
            log("codegen deja fait (cache)")
            return
        est = self.recipe.get("build", {}).get("estimate_minutes", {}).get("codegen", 10) * 60
        t0 = time.time()
        # progression : le codegen n'a pas de pourcentage ; on estime par le temps
        done = {"v": False}

        def tick(line):
            prog.sub(min(0.95, (time.time() - t0) / est), None)
        env = {"LD_LIBRARY_PATH": os.path.join(SDK, "lib")}
        run_logged([REXGLUE, "--force", "codegen", f"{name}_manifest.toml"], os.path.join(self.cache, "codegen.log"),
                   cwd=proj, env=env, on_line=tick)
        if not os.path.isfile(os.path.join(out, "sources.cmake")):
            raise RecipeError("codegen : sources.cmake absent")
        self.set_stamp("codegen", rev)

    # -- etape shaders
    def do_shaders(self, prog):
        proj = os.path.join(self.cache, "project")
        sh = os.path.join(self.cache, "shaders")
        rev = self.keyparts["xex_image"][:12] + self.keyparts["xenosrecomp"] + self.keyparts["sdk_hash"]
        if self.stamped("shaders", rev) and os.path.isfile(os.path.join(sh, "shader_cache.cpp")) and not self.force:
            log("cache de shaders deja fait")
            return
        s = self.recipe.get("shaders")
        if not s:
            return
        shutil.rmtree(sh, ignore_errors=True)
        os.makedirs(sh)
        lp = os.path.join(self.cache, "shaders.log")
        py = [sys.executable, "-I"]
        tools = os.path.join(self.rdir, "tools")
        inp = os.path.join(sh, "input")
        os.makedirs(inp)
        # 1. conteneurs des archives du jeu (decompresses, jamais versionnes)
        prog.sub(0.05, "recolte des shaders dans les archives du jeu")
        harvested = os.path.join(sh, "harvest")
        run_logged(py + [os.path.join(tools, s["harvest"]), os.path.join(self.game_dir, s["archives"]), harvested], lp)
        for f in os.listdir(harvested):
            if f.endswith(".bin"):
                shutil.copy(os.path.join(harvested, f), inp)
        # 2. shaders compiles dans l'executable (lus dans l'image decodee)
        prog.sub(0.3, "shaders de l'executable")
        img = os.path.join(sh, "image")
        run_logged([REXGLUE, "dump-xex", self.xex, img], lp, env={"LD_LIBRARY_PATH": os.path.join(SDK, "lib")})
        run_logged(py + [os.path.join(tools, s["image_shaders"]), img, inp], lp)
        # 3. XenosRecomp unifie
        prog.sub(0.45, "XenosRecomp (SPIR-V)")
        env = {"XENOS_RECOMP_MICROCODE_KEY": "1", "XENOS_RECOMP_NO_ZSTD": "1",
               "LD_LIBRARY_PATH": os.path.join(XENOSRECOMP_DIR, "thirdparty", "dxc-bin", "lib", "x64")}
        run_logged([XENOSRECOMP, inp, os.path.join(sh, "shader_cache.cpp"),
                    os.path.join(XENOSRECOMP_DIR, "XenosRecomp", "shader_common.h")], lp, env=env)
        n = len(re.findall(r"(?m)^\t\{ 0x", open(os.path.join(sh, "shader_cache.cpp")).read()))
        log(f"{n} shaders dans le cache")
        json.dump({"shaders": n}, open(os.path.join(sh, "info.json"), "w"))
        shutil.rmtree(inp, ignore_errors=True)
        shutil.rmtree(harvested, ignore_errors=True)
        shutil.rmtree(img, ignore_errors=True)
        self.set_stamp("shaders", rev)

    # -- etape compile
    def do_compile(self, prog):
        proj = os.path.join(self.cache, "project")
        bdir = os.path.join(proj, "out", "build")
        b = self.recipe.get("build", {})
        lp = os.path.join(self.cache, "compile.log")
        native = self.recipe.get("render", {}).get("mode") == "native"
        cm = ["cmake", "-S", proj, "-B", bdir, "-G", "Ninja", f"-DCMAKE_PREFIX_PATH={SDK}", "-DCMAKE_BUILD_TYPE=Release",
              "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
              f"-DCMAKE_C_FLAGS={b.get('arch_flags', '-march=x86-64-v2')}",
              f"-DCMAKE_CXX_FLAGS={b.get('arch_flags', '-march=x86-64-v2')}",
              "-DREXGLUE_ENABLE_TRACY=OFF", "-DREXGLUE_ENABLE_PERF_COUNTERS=OFF", "-DREXGLUE_RECOMP_DEBUG_INFO=none",
              f"-DXD_NATIVE_GPU={'ON' if native else 'OFF'}",
              f"-DXD_SHADER_CACHE={os.path.join(self.cache, 'shaders', 'shader_cache.cpp')}"]
        prog.sub(0.0, "configuration (cmake)")
        run_logged(cm, lp)
        jobs = self.jobs or int(b.get("jobs", 0)) or max(2, min(os.cpu_count() or 4, int(mem_available_gb() / 2.2)))
        log(f"compilation avec {jobs} taches")
        total = {"n": 0}
        rx = re.compile(r"^\[(\d+)/(\d+)\]")

        def on_line(line):
            m = rx.match(line)
            if m:
                a, n = int(m.group(1)), int(m.group(2))
                total["n"] = n
                prog.sub(a / n, f"compilation {a}/{n}")
        run_logged(["cmake", "--build", bdir, "-j", str(jobs), "--target", self.recipe["codegen"]["project"]], lp,
                   on_line=on_line)
        # produits du cache : binaire + bibliotheques dans bin/
        bin_dir = os.path.join(self.cache, "bin")
        shutil.rmtree(bin_dir, ignore_errors=True)
        os.makedirs(bin_dir)
        exe = self.recipe["codegen"]["project"]
        shutil.copy(os.path.join(bdir, exe), bin_dir)
        for f in os.listdir(bdir):
            if f.endswith(".so") or ".so." in f:
                shutil.copy(os.path.join(bdir, f), bin_dir)
        shutil.copy(os.path.join(proj, f"{exe}.runtime.toml"), os.path.join(bin_dir, f"{exe}.toml"))

    # -- etape bootstrap (test d'amorcage)
    def do_bootstrap(self, prog):
        bt = self.recipe.get("bootstrap", {})
        if not bt:
            return
        exe = self.recipe["codegen"]["project"]
        seconds = bt.get("seconds", 30)
        display = os.environ.get("XD_DISPLAY", ":97")
        lp = os.path.join(self.cache, "bootstrap.log")
        env = dict(os.environ, DISPLAY=display, LD_LIBRARY_PATH=os.path.join(self.cache, "bin") + ":" + os.path.join(SDK, "lib"))
        # Test sans affichage : Xvfb n'a pas DRI3, on presente par lavapipe (rendu logiciel) s'il existe
        lvp = "/usr/share/vulkan/icd.d/lvp_icd.json"
        if bt.get("headless", True) and os.path.isfile(lvp) and "VK_ICD_FILENAMES" not in env:
            env["VK_ICD_FILENAMES"] = lvp
            env["MESA_VK_WSI_PRESENT_MODE"] = "immediate"
        if os.environ.get("XD_BOOT_ENV"):
            for kv in os.environ["XD_BOOT_ENV"].split():
                k, _, v = kv.partition("=")
                env[k] = v
        xv = None
        fb = os.path.join(self.cache, "bootstrap.fb")
        if bt.get("headless", True) and shutil.which("Xvfb"):
            fb = os.path.join(self.cache, "bootstrap.fb")
            shutil.rmtree(fb, ignore_errors=True)
            os.makedirs(fb)
            xv = subprocess.Popen(["Xvfb", display, "-screen", "0", "1280x720x24", "-fbdir", fb],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            time.sleep(1)
        try:
            with open(lp, "w") as lf:
                p = subprocess.Popen([os.path.join(self.cache, "bin", exe)], cwd=os.path.join(self.cache, "bin"), env=env,
                                     stdout=lf, stderr=subprocess.STDOUT, start_new_session=True)
                for i in range(int(seconds * 2)):
                    time.sleep(0.5)
                    prog.sub(i / (seconds * 2), None)
                    if p.poll() is not None:
                        break
                alive = p.poll() is None
                if alive and os.path.isfile(os.path.join(fb, "Xvfb_screen0")):
                    # capture de l'ecran (reste dans le cache, jamais versionnee)
                    subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "xwd2png.py"), os.path.join(fb, "Xvfb_screen0"),
                                    os.path.join(self.cache, "bootstrap.png")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if alive:
                    os.killpg(p.pid, signal.SIGTERM)
                    try:
                        p.wait(10)
                    except subprocess.TimeoutExpired:
                        os.killpg(p.pid, signal.SIGKILL)
        finally:
            if xv:
                xv.terminate()
        text = open(lp, errors="replace").read()
        logs = os.path.join(self.cache, "bin", "logs")
        if os.path.isdir(logs):   # le journal du jeu (REXLOG) est dans bin/logs/, pas sur la sortie standard
            newest = sorted(os.listdir(logs), key=lambda f: os.path.getmtime(os.path.join(logs, f)))[-1:]
            for f in newest:
                text += open(os.path.join(logs, f), errors="replace").read()
        if "FATAL" in text or "Segmentation" in text:
            raise RecipeError("test d'amorcage : erreur fatale, voir " + lp)
        for pat in bt.get("expect", []):
            if pat not in text:
                raise RecipeError(f"test d'amorcage : '{pat}' absent du journal ({lp})")
        if not alive:
            raise RecipeError("test d'amorcage : le jeu s'est arrete avant " + str(seconds) + " s, voir " + lp)

    # -- orchestration
    def run(self):
        self.do_identify()
        info = self.info
        os.makedirs(self.cache, exist_ok=True)
        est = sum(self.recipe.get("build", {}).get("estimate_minutes", {"x": 30}).values()) * 60
        prog = Progress(self.cache, est)
        prog.update(title_id=info["title_id"], name=self.name, cache=self.cache, source=self.game_dir)
        if self.ok() and not self.force and self.stages == STAGES:
            log(f"{self.name} : cache a jour ({self.cache})")
            prog.update(done=True, percent=100.0, message="deja construit")
            return self.cache
        t0 = time.time()
        steps = [("identify", None), ("prepare", self.do_prepare), ("extract", self.do_extract),
                 ("scaffold", self.do_scaffold), ("codegen", self.do_codegen), ("shaders", self.do_shaders),
                 ("compile", self.do_compile), ("bootstrap", self.do_bootstrap)]
        msgs = {"identify": f"XEX {info['title_id']} v{info['version']} reconnu", "prepare": "controles",
                "extract": "source du jeu", "scaffold": "projet de la recette",
                "codegen": "recompilation du XEX en C++ (rexglue codegen)",
                "shaders": "cache de shaders (XenosRecomp)", "compile": "compilation native",
                "bootstrap": "test d'amorcage"}
        try:
            for name, fn in steps:
                if name not in self.stages:
                    continue
                prog.stage(name, msgs[name])
                t = time.time()
                if fn:
                    fn(prog)
                self.t[name] = round(time.time() - t, 1)
            if self.stages == STAGES:
                json.dump({"title_id": info["title_id"], "name": self.name, "xex": info["sha256_image"],
                           "version": info["version"], "key": self.key, "keyparts": self.keyparts,
                           "recipe_rev": recipe_rev(self.rdir), "seconds": self.t, "built": time.strftime("%F %T"),
                           "game_dir": self.game_dir}, open(os.path.join(self.cache, "build.ok"), "w"), indent=1)
            prog.update(done=True, percent=100.0, message="termine", stage_times=self.t)
            log(f"{self.name} : " + (f"construit en {time.time() - t0:.0f} s" if self.stages == STAGES else
                                    "etapes " + ",".join(self.stages) + f" faites en {time.time() - t0:.0f} s") + f" ({self.cache})")
        except RecipeError as e:
            prog.update(error=str(e), message="echec")
            raise
        return self.cache


def launch_info(b):
    exe = b.recipe["codegen"]["project"]
    return {"title_id": b.info["title_id"], "name": b.name, "native": f"./{exe}",
            "cwd": os.path.join(b.cache, "bin"), "cache": b.cache, "built": b.ok()}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("identify")
    a.add_argument("source")
    for n in ("build", "status", "launch-info"):
        s = sub.add_parser(n)
        s.add_argument("source")
        if n == "build":
            s.add_argument("--force", action="store_true")
            s.add_argument("--stages", default="")
            s.add_argument("--jobs", type=int, default=0)
            s.add_argument("--container", action="store_true")
    c = sub.add_parser("clean")
    c.add_argument("title_id")
    c.add_argument("--all", action="store_true")
    args = ap.parse_args()
    try:
        if args.cmd == "clean":
            d = os.path.join(data_home(), "games", args.title_id.upper())
            shutil.rmtree(d, ignore_errors=True)
            log(f"cache supprime : {d}")
            return 0
        if args.cmd == "identify":
            tmp = os.path.join(data_home(), "tmp-identify")
            os.makedirs(tmp, exist_ok=True)
            _, x = resolve_source(args.source, os.path.join(data_home(), "sources"))
            info = identify(x, tmp)
            shutil.rmtree(tmp, ignore_errors=True)
            info.pop("image", None)
            recipe, rdir = load_recipe(info["title_id"])
            info["recipe"] = bool(recipe)
            info["match"] = bool(recipe and match_xex(recipe, info))
            print(json.dumps(info, indent=1))
            return 0
        b = Build(args.source, force=getattr(args, "force", False),
                  stages=[s for s in getattr(args, "stages", "").split(",") if s] or None,
                  jobs=getattr(args, "jobs", None), container=getattr(args, "container", False))
        if args.cmd == "build":
            b.run()
            return 0
        b.do_identify()
        if args.cmd == "status":
            print(json.dumps({"cache": b.cache, "built": b.ok(), "key": b.key}, indent=1))
        else:
            print(json.dumps(launch_info(b)))
        return 0
    except RecipeError as e:
        print(f"[recipe] ERREUR : {e}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
