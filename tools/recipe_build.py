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
import threading
import tomllib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codegen_loop
import d3d_match

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
ENGINE_REV = 2   # a incrementer quand la disposition des produits du cache (bin/, build.ok...) change


class RecipeError(Exception):
    pass


class MissingFunction(RecipeError):
    """Test d'amorcage arrete sur une fonction non declaree au codegen (mode generique) : la boucle de construction la reprend."""


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


def write_if_changed(path, text):
    """Ecrit un fichier seulement si son contenu change : ninja/CMake relancent le codegen sur un simple changement de date."""
    if os.path.isfile(path) and open(path).read() == text:
        return
    with open(path, "w") as f:
        f.write(text)


def sync_tree(src, dst, skip=("__pycache__",)):
    """Copie src dans dst en ne touchant que les fichiers dont le contenu differe ; supprime ceux qui n'existent plus."""
    os.makedirs(dst, exist_ok=True)
    keep = set()
    for d, dirs, files in os.walk(src):
        dirs[:] = [x for x in dirs if x not in skip]
        rel = os.path.relpath(d, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            a, b = os.path.join(d, f), os.path.normpath(os.path.join(dst, rel, f))
            keep.add(b)
            if not os.path.isfile(b) or open(a, "rb").read() != open(b, "rb").read():
                shutil.copy(a, b)
    for d, dirs, files in os.walk(dst):
        for f in files:
            if os.path.normpath(os.path.join(d, f)) not in keep:
                os.remove(os.path.join(d, f))


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
    d = os.path.join(GAMES, title_id if title_id.startswith("_") else title_id.upper())
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
    if os.path.basename(rdir) == "_generic":    # le mode generique depend aussi du matcher, de ses signatures et de la boucle de codegen
        files += [(TOOLS, "d3d_match.py"), (TOOLS, "codegen_loop.py"), (TOOLS, "func_discover.py")] + [(TOOLS, os.path.join("d3d_signatures", f))
                                                                          for f in sorted(os.listdir(d3d_match.SIGDIR))]
    return hashlib.sha256((sha256_tree(files) + str(ENGINE_REV)).encode()).hexdigest()[:12]


def cache_key(info, recipe, rdir):
    sdk_ver, sdk_hash = sdk_id()
    parts = [info["sha256_image"], str(recipe["schema"]), sdk_hash, xenosrecomp_id(), recipe_codegen_rev(rdir, recipe)]
    if recipe.get("generic"):
        parts.append("generic-" + recipe_rev(rdir))
    return hashlib.sha256("|".join(parts).encode()).hexdigest()[:16], {
        "xex_image": info["sha256_image"], "sdk": sdk_ver, "sdk_hash": sdk_hash, "xenosrecomp": xenosrecomp_id(),
        "codegen_rev": parts[-1]}


# --------------------------------------------------------------------------------------------------
# Progression
# --------------------------------------------------------------------------------------------------
class Progress:
    """progress.json : lu par le Dashboard (superposition de construction) ; ecrit de facon atomique."""

    def __init__(self, cache, estimate):
        self.lock = threading.Lock()
        self.tick_fn = None   # appelee chaque seconde par le battement de coeur (progression a l'estime d'une etape sans sortie)
        self.path = os.path.join(cache, "progress.json") if cache else None
        self.state = {"stage": "", "index": 0, "total": len(STAGES), "percent": 0.0, "message": "", "done": False,
                      "error": "", "pid": os.getpid(), "started": time.time(), "estimate_s": estimate, "stages": {}}

    def update(self, **kw):
        self.state.update(kw)
        self.state["elapsed_s"] = round(time.time() - self.state["started"], 1)
        if self.path:
            with self.lock:
                tmp = self.path + ".tmp"
                with open(tmp, "w") as f:
                    json.dump(self.state, f)
                os.replace(tmp, self.path)

    def start_heartbeat(self):
        """Ecrit progress.json chaque seconde (duree ecoulee a jour meme quand une etape ne dit rien)."""
        def beat():
            while not self.state.get("done") and not self.state.get("error"):
                if self.tick_fn:
                    try:
                        self.tick_fn()
                    except Exception:
                        pass
                else:
                    self.update()
                time.sleep(1)
        threading.Thread(target=beat, daemon=True).start()

    def stage(self, name, message=""):
        self.tick_fn = None
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
    """Renvoie (dossier_du_jeu, chemin_default_xex). Un dossier contient default.xex ; ISO, STFS, GOD... sont extraits (cache)
    par tools/xcontainer.py (lot A) ; repli sur tools/extract_iso.py pour une ISO si xcontainer est absent."""
    arg = os.path.abspath(os.path.expanduser(arg))
    if os.path.isdir(arg):
        x = os.path.join(arg, "default.xex")
        if os.path.isfile(x):
            return arg, x
    if arg.lower().endswith(".xex") and os.path.isfile(arg):
        return os.path.dirname(arg), arg
    xc = os.path.join(TOOLS, "xcontainer.py")
    if os.path.exists(arg) and os.path.isfile(xc):
        if os.path.isdir(arg):    # arborescence de paquets (<TitleID>/<type>/<paquet>) : on extrait celui qui contient un XEX
            r = subprocess.run([sys.executable, "-I", xc, "info", arg], stdout=subprocess.PIPE, text=True)
            try:
                pk = [x["name"] for x in json.loads(r.stdout).get("packages", [])]
            except ValueError:
                pk = []
            for name in pk:
                try:
                    return resolve_source(os.path.join(arg, name), cache_parent_for_iso)
                except RecipeError:
                    continue
        out = os.path.join(cache_parent_for_iso, "src-" + hashlib.sha256(arg.encode()).hexdigest()[:10])
        os.makedirs(out, exist_ok=True)
        r = subprocess.run([sys.executable, "-I", xc, "extract", arg, out], stdout=subprocess.PIPE, text=True)
        game = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else ""
        if r.returncode == 0 and game and os.path.isfile(os.path.join(game, "default.xex")):
            return game, os.path.join(game, "default.xex")
        raise RecipeError(f"extraction de {arg} par xcontainer.py echouee")
    if arg.lower().endswith(".iso") and os.path.isfile(arg):
        out = os.path.join(cache_parent_for_iso, "iso-" + hashlib.sha256(arg.encode()).hexdigest()[:10])
        if not os.path.isfile(os.path.join(out, "default.xex")):
            os.makedirs(out, exist_ok=True)
            log(f"extraction de l'ISO {arg} -> {out}")
            r = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "extract_iso.py"), arg, out])
            if r.returncode != 0:
                raise RecipeError("extraction de l'ISO echouee")
        return out, os.path.join(out, "default.xex")
    raise RecipeError(f"source non reconnue : {arg} (dossier avec default.xex, .xex, ISO, STFS, GOD)")


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


# Roles dont l'appel est suivi par la facade apres la fonction d'origine (arguments d'avant, r3 apres) : methodes du peripherique
AFTER_ROLES = ["BeginVertices", "DrawIndexedVertices", "DrawVertices", "EndVertices", "Clear", "BeginTiling", "Resolve"]


def gen_generic_profile(report, dest, missing_dir):
    """Rapport de d3d_match.analyze -> d3d_profile.generated.h + d3d_hooks.generated.inc + d3d_nullgpu_hooks.generated.inc.
    Seules les fonctions retrouvees a haute confiance sont crochetees (les roles facultatifs absents n'ont pas de crochet)."""
    roles = {k: int(v["addr"], 16) for k, v in report["roles"].items()}
    off = report["offsets"]
    lay = report["layout"]
    ok = lambda r: r in report["roles"] and report["roles"][r]["confidence"] >= d3d_match.MIN_CONFIDENCE \
        and r not in report.get("optional_missing", [])
    h = ["// genere par tools/recipe_build.py (mode generique) depuis tools/d3d_match.py : ne pas versionner",
         "#pragma once", "#include <cstdint>", "namespace xd_profile {"]
    for i, r in enumerate(AFTER_ROLES, 1):
        h.append(f"constexpr uint32_t k_{r} = {i};")
    h.append(f"constexpr const char* kMissingShaderDir = \"{missing_dir}\";")
    for k in ("fence_ptr", "fence_value", "drain_ptr_word", "second_word"):
        h.append(f"constexpr uint32_t kNull_{k} = 0x{off[k]:X}u;")
    h.append(f"constexpr uint32_t kNull_vblank_handler = 0x{roles['VblankHandler']:08X}u;")
    # disposition de l'ombre du peripherique lue dans le code du jeu (ngpu_core/src/device_layout.h)
    for k in ("fetch_constants", "vertex_constants", "pixel_constants", "vertex_bool_constants", "pixel_bool_constants",
              "index_buffer", "pixel_shader", "vertex_shader"):
        h.append(f"constexpr uint32_t kLayout_{k} = {lay[k]};")
    h.append("constexpr uint32_t kLayout_register_groups[7] = {" + ", ".join(map(str, lay["register_groups"])) + "};")
    # objet shader de la XDK : decalage du pointeur vers sa partie physique (microcode patche par D3D)
    h.append(f"constexpr uint32_t kShader_physical_ptr = {report.get('shader_physical_ptr', 32)};")
    # aucune liste de shaders d'interface, aucune plage de pitch de scene : echelle de rendu 1
    h += ["constexpr uint64_t kInterfaceVertexShaders[] = {0};", "constexpr uint64_t kCornerVertexShaders[] = {0};",
          "constexpr uint32_t kScenePitchMin = 1;", "constexpr uint32_t kScenePitchMax = 0;", "}  // namespace xd_profile"]
    open(os.path.join(dest, "d3d_profile.generated.h"), "w").write("\n".join(h) + "\n")
    inc = ["// genere par tools/recipe_build.py (mode generique) : un crochet par fonction D3D retrouvee par d3d_match.py"]
    hooked = {}
    for r in AFTER_ROLES:
        if not ok(r):
            inc.append(f"// {r} : non retrouvee a haute confiance, pas de crochet")
            continue
        a = f"{roles[r]:08X}"
        hooked[r] = roles[r]
        inc.append(f"REX_HOOK_RAW(sub_{a}) {{ const PPCContext before = ctx; __imp__sub_{a}(ctx, base);"
                   f" XdFrontOnCall(xd_profile::k_{r}, before, ctx.r3.u32, base); }}  // {r}")
    a = f"{roles['Swap']:08X}"
    hooked["Swap"] = roles["Swap"]
    inc.append(f"REX_HOOK_RAW(sub_{a}) {{ XdFrontOnSwap(base); __imp__sub_{a}(ctx, base); }}  // Swap")
    if ok("GpuBeginShaderConstantF4"):   # (peripherique, pixel, premier registre, nombre) -> adresse des constantes a ecrire
        a = f"{roles['GpuBeginShaderConstantF4']:08X}"
        hooked["GpuBeginShaderConstantF4"] = roles["GpuBeginShaderConstantF4"]
        inc.append(f"REX_HOOK_RAW(sub_{a}) {{ const uint32_t pixel = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;"
                   f" __imp__sub_{a}(ctx, base); XdFrontOnGpuConstants(base, pixel, start, count, ctx.r3.u32); }}  // GpuBeginShaderConstantF4")
    for kind in ("Vertex", "Pixel"):
        inner, wrap = roles.get(f"Create{kind}ShaderInner"), roles[f"Create{kind}Shader"]
        if inner and ok(f"Create{kind}ShaderInner"):   # constructeur sur place (objet = r3, &info = r5, info[0] = conteneur)
            a = f"{inner:08X}"
            hooked[f"Create{kind}ShaderInner"] = inner
            inc.append(f"REX_HOOK_RAW(sub_{a}) {{ const uint32_t object = ctx.r3.u32; const uint32_t container = REX_LOAD_U32(ctx.r5.u32);"
                       f" __imp__sub_{a}(ctx, base); XdFrontOnShaderCreate(base, object, container); }}  // Create{kind}ShaderInner")
        else:                                            # enveloppe publique (pFunction = r3) -> objet en r3
            a = f"{wrap:08X}"
            hooked[f"Create{kind}Shader"] = wrap
            inc.append(f"REX_HOOK_RAW(sub_{a}) {{ const uint32_t container = ctx.r3.u32; __imp__sub_{a}(ctx, base);"
                       f" XdFrontOnShaderCreate(base, ctx.r3.u32, container); }}  // Create{kind}Shader")
    open(os.path.join(dest, "d3d_hooks.generated.inc"), "w").write("\n".join(inc) + "\n")
    ng = ["// genere par tools/recipe_build.py (mode generique) : attentes de la XDK D3D, voir null_gpu.cpp"]
    a = f"{roles['WaitFence']:08X}"
    ng.append(f"REX_HOOK_RAW(sub_{a}) {{ ngpu::NullGpuCompleteNow(base, ctx.r3.u32); __imp__sub_{a}(ctx, base); }}  // WaitFence")
    a = f"{roles['WaitDrain']:08X}"
    ng.append(f"REX_HOOK_RAW(sub_{a}) {{ const uint32_t dev = ctx.r3.u32; ngpu::NullGpuCompleteNow(base, dev);"
              f" const uint32_t idp = ngpu::Be32(base + dev + xd_profile::kNull_fence_ptr);"
              f" if (idp) ngpu::PutBe32(base + idp + xd_profile::kNull_drain_ptr_word, ctx.r4.u32); __imp__sub_{a}(ctx, base); }}  // WaitDrain")
    a = f"{roles['WaitSecond']:08X}"
    ng.append(f"REX_HOOK_RAW(sub_{a}) {{ const uint32_t dev = ctx.r3.u32; ngpu::NullGpuCompleteNow(base, dev);"
              f" const uint32_t idp = ngpu::Be32(base + dev + xd_profile::kNull_fence_ptr);"
              f" if (idp) ngpu::PutBe32(base + idp + xd_profile::kNull_second_word, ctx.r5.u32); __imp__sub_{a}(ctx, base); }}  // WaitSecond")
    open(os.path.join(dest, "d3d_nullgpu_hooks.generated.inc"), "w").write("\n".join(ng) + "\n")
    for r in ("WaitFence", "WaitDrain", "WaitSecond"):
        hooked[r] = roles[r]
    return hooked


def analyze_xex(xex, image_path=None):
    """Analyse D3D d'un XEX (mise en cache par hachage de l'image decodee). Renvoie le rapport de d3d_match.analyze."""
    tmp = os.path.join(data_home(), "tmp-analyze")
    os.makedirs(tmp, exist_ok=True)
    if image_path is None:
        image_path = decode_image(xex, tmp)[0]
    digest = sha256_file(image_path)
    sig_rev = hashlib.sha256(b"".join(open(os.path.join(d3d_match.SIGDIR, f), "rb").read()
                                      for f in sorted(os.listdir(d3d_match.SIGDIR)) if f.endswith(".json"))
                             + open(d3d_match.__file__, "rb").read()).hexdigest()[:12]
    cache = os.path.join(data_home(), "analysis", f"{digest[:16]}-{sig_rev}.json")
    if os.path.isfile(cache):
        return json.load(open(cache))
    img = d3d_match.Image(image_path, xex=xex)
    rep = d3d_match.analyze(img, d3d_match.load_signatures())
    rep.pop("_raw")
    rep["sha256_image"] = digest
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    json.dump(rep, open(cache, "w"), indent=1)
    shutil.rmtree(tmp, ignore_errors=True)
    return rep


def user_functions_path(title_id):
    """Fonctions imposees au codegen generique (decouvertes a l'execution ou declarees a la main) : jamais dans le depot."""
    return os.path.join(data_home(), "games", title_id, "missing_functions.toml")


def read_user_functions(title_id):
    out = {}
    p = user_functions_path(title_id)
    if os.path.isfile(p):
        for line in open(p):
            m = re.match(r'\s*"?0x([0-9A-Fa-f]{8})"?\s*=\s*(\{[^}]*\})', line)
            if m:
                out[int(m.group(1), 16)] = m.group(2)
    return out


def extra_modules(game_dir, xex):
    """XEX secondaires et DLL du jeu (non recompiles par le mode generique)."""
    out = []
    for dp, _, files in os.walk(game_dir):
        for f in files:
            if f.lower().endswith((".xex", ".dll", ".xexp")) and os.path.abspath(os.path.join(dp, f)) != os.path.abspath(xex):
                out.append(os.path.relpath(os.path.join(dp, f), game_dir))
    return sorted(out)[:50]


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
    def __init__(self, source_arg, force=False, stages=None, jobs=None, container=None, generic=False):
        self.force = force
        self.force_generic = generic or bool(os.environ.get("XD_GENERIC"))
        self.mode = "recipe"
        self.generic_reason = ""
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
        tmp = os.path.join(home, "tmp-identify")
        os.makedirs(tmp, exist_ok=True)
        info = identify(self.xex, tmp)
        shutil.rmtree(tmp, ignore_errors=True)
        info["image"] = None
        self.info = info
        locked = match_xex(recipe, info) if recipe else None
        if recipe is None:
            self.generic_reason = f"pas de recette games/{self.info['title_id']}/"
        elif not locked:
            known = ", ".join(x["sha256_image"][:12] for x in recipe.get("xex", []))
            self.generic_reason = (f"XEX {info['version']} (image {info['sha256_image'][:12]}) hors du verrou de la recette "
                                   f"(connus : {known})")
        elif self.force_generic:
            self.generic_reason = "mode generique force (--generic / XD_GENERIC)"
        if self.generic_reason:
            self.mode = "generic"
            recipe, rdir = load_recipe("_generic")
            locked = None
        elif locked.get("version") and locked["version"] != info["version"]:
            raise RecipeError(f"version de l'en-tete {info['version']} differente de la recette {locked['version']}")
        self.recipe, self.rdir = recipe, rdir
        self.locked = locked
        self.key, self.keyparts = cache_key(info, recipe, rdir)
        if self.mode == "generic" and os.environ.get("XD_CACHE_KEY"):
            # developpement du mode generique : garder le meme dossier de cache d'une revision des outils a l'autre (les etapes
            # restent invalidees par leurs propres cachets) ; jamais utilise par le Dashboard
            self.key = os.environ["XD_CACHE_KEY"]
        self.cache = os.path.join(home, "games", info["title_id"], self.key)
        self.name = recipe.get("name", info["title_id"]) if self.mode == "recipe" else f"Jeu {info['title_id']}"
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
            if json.load(open(p)).get("recipe_rev") != recipe_rev(self.rdir):
                return False
            if self.mode == "generic":    # boucle jouer -> reconstruire : de nouveaux microcodes manques depuis la derniere construction
                missing = os.path.join(data_home(), "games", self.info["title_id"], "missing_shaders")
                now = len([f for f in os.listdir(missing) if f.endswith(".bin")]) if os.path.isdir(missing) else 0
                if now != json.load(open(os.path.join(self.cache, "shaders", "info.json"))).get("missing_added", 0):
                    return False
            return True
        except Exception:
            return False

    # -- etape prepare
    def do_prepare(self, prog):
        b = self.recipe.get("build", {})
        need_disk, need_ram = b.get("min_disk_gb", 40), b.get("min_ram_gb", 16)
        need_disk = float(os.environ.get("XD_MIN_DISK_GB", need_disk))   # abaisser pour un essai : la construction de Skate pese ~0,6 Go
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
        for p, hint in ((REXGLUE, "SDK installe dans sdk/ (voir docs/technique.md)"),
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
        sync_tree(os.path.join(self.rdir, "src"), os.path.join(proj, "src"))
        if os.path.isdir(os.path.join(self.rdir, "native")):
            sync_tree(os.path.join(self.rdir, "native"), os.path.join(proj, "native"))
        for n in cg.get("files", []):
            shutil.copy(os.path.join(self.rdir, n), os.path.join(proj, n))
        gen = os.path.join(proj, "gen")
        os.makedirs(gen, exist_ok=True)
        name = cg["project"]
        prof = {}
        if self.mode == "generic":
            self.scaffold_generic(proj, gen, name)
        else:
            prof = gen_d3d_profile(self.rdir, r, gen)
        # manifeste ReXGlue
        files = list(cg.get("files", [])) + (["functions.generated.toml"] if self.mode == "generic" else [])
        includes = ", ".join(f'"{n}"' for n in files)
        manifest = (f'[project]\nname = "{name}"\nsdk_version = "0.10.0"\ngame_root = "{self.game_dir}"\n\n'
                    f'[entrypoint]\nfile_path = "{self.xex}"\nout_directory_path = "{cg["out_directory"]}"\n'
                    f'includes = [{includes}]\n')
        write_if_changed(os.path.join(proj, f"{name}_manifest.toml"), manifest)
        # CMakeLists du jeu depuis le gabarit
        t = open(os.path.join(TEMPLATE, "CMakeLists.txt")).read()
        t = t.replace("@NAME@", name).replace("@XENONDASH_ROOT@", ROOT).replace("@OUT@", cg["out_directory"])
        write_if_changed(os.path.join(proj, "CMakeLists.txt"), t)
        # configuration d'execution : runtime de la recette + game_data_root absolu (le jeu reste ou il est)
        rt = open(os.path.join(self.rdir, r.get("runtime", {}).get("file", "runtime.toml"))).read()
        rt = re.sub(r"(?m)^game_data_root\s*=.*\n", "", rt)
        rt += f"\ngame_data_root = '{self.game_dir}'\n"
        if r.get("render", {}).get("mode") == "native":
            # reglages d'execution propres au rendu natif (d3d_profile.toml [runtime]) : ils remplacent ceux de runtime.toml
            for k, v in prof.get("runtime", {}).items():
                rt = re.sub(rf"(?m)^{k}\s*=.*\n", "", rt)
                rt += f"{k} = {toml_value(v)}\n"
        write_if_changed(os.path.join(proj, f"{name}.runtime.toml"), rt)

    def scaffold_generic(self, proj, gen, name):
        """Mode generique : analyse D3D de l'image (d3d_match), profil et crochets generes ; refus net si non rendable nativement."""
        imgdir = os.path.join(self.cache, "image")
        bins = [f for f in os.listdir(imgdir)] if os.path.isdir(imgdir) else []
        if not any(f.endswith(".bin") for f in bins):
            decode_image(self.xex, self.cache)
            bins = os.listdir(imgdir)
        self.image_path = os.path.join(imgdir, next(f for f in bins if f.endswith(".bin")))
        extra = extra_modules(self.game_dir, self.xex)
        if extra:
            log(f"avertissement : XEX secondaires/DLL du jeu non recompiles par le mode generique : {', '.join(extra[:8])}"
                + (" ..." if len(extra) > 8 else ""))
        rep = analyze_xex(self.xex, self.image_path)
        rep["extra_modules"] = extra
        json.dump(rep, open(os.path.join(self.cache, "analysis.json"), "w"), indent=1)
        found = len(rep["roles"])
        log(f"analyse D3D : XDK {rep['xdk']}, {found} fonctions retrouvees, manquantes : {rep['missing'] or 'aucune'}")
        if not rep["supported"]:
            # XD_ALLOW_UNSUPPORTED=1 : essai de diagnostic (codegen, compilation, demarrage) sans garantie de rendu correct ; ne contourne
            # que le verdict, jamais le besoin des fonctions et des decalages (et ne bascule jamais sur l'emulation)
            if not os.environ.get("XD_ALLOW_UNSUPPORTED") or rep["missing"] or any(
                    k not in rep["offsets"] or rep["offsets"][k] is None for k in ("fence_ptr", "fence_value", "drain_ptr_word", "second_word")):
                raise RecipeError("jeu non rendable nativement par le mode generique (aucun repli sur l'emulation) : " + rep["reason"])
            log("AVERTISSEMENT (XD_ALLOW_UNSUPPORTED) : verdict ignore pour un essai de diagnostic : " + rep["reason"])
        missing_dir = os.path.join(data_home(), "games", self.info["title_id"], "missing_shaders")
        genstage = os.path.join(self.cache, "gen.stage")
        shutil.rmtree(genstage, ignore_errors=True)
        os.makedirs(genstage)
        hooked_roles = gen_generic_profile(rep, genstage, missing_dir)
        json.dump({k: f"0x{a:08X}" for k, a in hooked_roles.items()}, open(os.path.join(self.cache, "hooked.json"), "w"), indent=1)
        open(os.path.join(genstage, "xd_project.h"), "w").write(f'#pragma once\n#include "generated/default/{name}_init.h"\n')
        for f in os.listdir(genstage):
            write_if_changed(os.path.join(gen, f), open(os.path.join(genstage, f)).read())
        shutil.rmtree(genstage, ignore_errors=True)
        # overrides initiaux : fonctions crochetees (declarees comme debuts de fonction) + setjmp/longjmp du CRT par signature
        crt = {k: int(rep["roles"][k]["addr"], 16) for k in ("setjmp", "longjmp")
               if k in rep["roles"] and rep["roles"][k]["confidence"] >= 0.85}
        hooked = {a: a for a in hooked_roles.values()}
        ov = os.path.join(proj, "functions.generated.toml")
        final = os.path.join(self.cache, "functions.final.toml")     # resultat de la boucle de codegen (relance sans refaire la boucle)
        if os.path.isfile(final):
            write_if_changed(ov, open(final).read())
        else:
            codegen_loop.write_overrides(ov + ".tmp", {}, hooked, {}, crt.get("setjmp"), crt.get("longjmp"),
                                         read_user_functions(self.info["title_id"]))
            write_if_changed(ov, open(ov + ".tmp").read())
            os.remove(ov + ".tmp")
        self.crt = crt
        self.report = rep

    @staticmethod
    def crt_roles(rep):
        return {k: int(v["addr"], 16) for k, v in rep["roles"].items()
                if k in ("setjmp", "longjmp") and v["confidence"] >= 0.85}

    @staticmethod
    def rexcrt_roles(rep):
        """Fonctions XAPI du jeu remplacees par celles du SDK ([rexcrt]) : les fibres, seulement si les cinq sont retrouvees."""
        got = {k: int(v["addr"], 16) for k, v in rep["roles"].items()
               if k in d3d_match.REXCRT_ROLES and v["confidence"] >= d3d_match.REXCRT_MIN_CONFIDENCE}
        return got if len(got) == len(d3d_match.REXCRT_ROLES) else {}

    def hooked_seeds(self):
        """Fonctions crochetees : declarees comme debuts de fonction des la 1re passe (le crochet exige __imp__sub_X)."""
        return {int(a, 16): int(a, 16) for a in json.load(open(os.path.join(self.cache, "hooked.json"))).values()}

    # -- etape codegen
    def do_codegen(self, prog):
        proj = os.path.join(self.cache, "project")
        name = self.recipe["codegen"]["project"]
        out = os.path.join(proj, self.recipe["codegen"]["out_directory"])
        rev = self.keyparts["codegen_rev"] + self.keyparts["sdk_hash"]
        if self.mode == "generic":
            # ce qui change le code genere : la boucle, la decouverte, les fonctions crochetees/CRT/XAPI de l'analyse, les fonctions
            # declarees a l'execution ; pas le noyau de rendu ni la facade (une retouche de ngpu_core ne refait pas le codegen)
            rep_ = json.load(open(os.path.join(self.cache, "analysis.json")))
            loop_rev = sha256_tree([(TOOLS, "codegen_loop.py"), (TOOLS, "func_discover.py")])[:12]
            rev = self.keyparts["sdk_hash"] + loop_rev + hashlib.sha256(json.dumps(
                [sorted(self.hooked_seeds()), sorted(self.crt_roles(rep_).items()), sorted(self.rexcrt_roles(rep_).items()),
                 sorted(read_user_functions(self.info["title_id"]).items())]).encode()).hexdigest()[:12]
        if self.stamped("codegen", rev) and os.path.isfile(os.path.join(out, "sources.cmake")) and not self.force:
            log("codegen deja fait (cache)")
            return
        est = self.recipe.get("build", {}).get("estimate_minutes", {}).get("codegen", 10) * 60
        t0 = time.time()
        # progression : le codegen n'a pas de pourcentage ; on estime par le temps
        done = {"v": False}

        prog.tick_fn = lambda: prog.sub(min(0.95, (time.time() - t0) / est), None)
        env = {"LD_LIBRARY_PATH": os.path.join(SDK, "lib")}
        if self.mode == "generic":
            img = d3d_match.Image(os.path.join(self.cache, "image", next(f for f in os.listdir(os.path.join(self.cache, "image"))
                                                                          if f.endswith(".bin"))), xex=self.xex)
            crt = self.crt_roles(rep_)
            ldir = os.path.join(self.cache, "codegen_logs")
            os.makedirs(ldir, exist_ok=True)
            state = os.path.join(self.cache, "codegen_state.json")     # reprise : meme boucle, memes outils (sinon on repart de zero)
            srev = os.path.join(self.cache, "codegen_state.rev")
            if not (os.path.isfile(srev) and open(srev).read() == loop_rev) or self.force:
                if os.path.isfile(state):
                    os.remove(state)
            open(srev, "w").write(loop_rev)
            try:
                stats = codegen_loop.run_loop(REXGLUE, os.path.join(SDK, "lib"), proj, f"{name}_manifest.toml", out, f"{name}_init.cpp",
                                              img, os.path.join(proj, "functions.generated.toml"), ldir, crt.get("setjmp"),
                                              crt.get("longjmp"), log=log,
                                              tick=lambda i, n: prog.update(message=f"codegen, passe {i}"),
                                              extra=read_user_functions(self.info["title_id"]), seeds=self.hooked_seeds(),
                                              rexcrt=self.rexcrt_roles(rep_), state_path=state,
                                              discover=not os.environ.get("XD_NO_DISCOVER"))
            except codegen_loop.CodegenError as e:
                raise RecipeError(f"codegen generique : {e}")
            json.dump(stats, open(os.path.join(self.cache, "codegen_stats.json"), "w"), indent=1)
            shutil.copy(os.path.join(proj, "functions.generated.toml"), os.path.join(self.cache, "functions.final.toml"))
            log(f"codegen generique : {stats['functions']} fonctions en {stats['iterations']} passes "
                f"({stats['unresolved']} cibles, {stats['pointer']} par pointeur dont {stats['static']} par decouverte statique, "
                f"{stats['continuations']} continuations, {stats['static_dropped']} candidats rejetes)")
        else:
            run_logged([REXGLUE, "--force", "codegen", f"{name}_manifest.toml"], os.path.join(self.cache, "codegen.log"),
                       cwd=proj, env=env)
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
        if self.mode == "generic":
            return self.do_shaders_generic(prog, proj, sh)
        shutil.rmtree(sh, ignore_errors=True)
        os.makedirs(sh)
        t0 = time.time()
        est = self.recipe.get("build", {}).get("estimate_minutes", {}).get("shaders", 4) * 60
        prog.tick_fn = lambda: prog.sub(min(0.95, (time.time() - t0) / est), None)   # a l'estime : les outils ne disent rien
        lp = os.path.join(self.cache, "shaders.log")
        py = [sys.executable, "-I"]
        tools = os.path.join(self.rdir, "tools")
        inp = os.path.join(sh, "input")
        os.makedirs(inp)
        # 1. conteneurs des archives du jeu (decompresses, jamais versionnes)
        prog.update(message="recolte des shaders dans les archives du jeu")
        harvested = os.path.join(sh, "harvest")
        run_logged(py + [os.path.join(tools, s["harvest"]), os.path.join(self.game_dir, s["archives"]), harvested], lp)
        for f in os.listdir(harvested):
            if f.endswith(".bin"):
                shutil.copy(os.path.join(harvested, f), inp)
        # 2. shaders compiles dans l'executable (lus dans l'image decodee)
        prog.update(message="shaders de l'executable")
        img = os.path.join(sh, "image")
        run_logged([REXGLUE, "dump-xex", self.xex, img], lp, env={"LD_LIBRARY_PATH": os.path.join(SDK, "lib")})
        run_logged(py + [os.path.join(tools, s["image_shaders"]), img, inp], lp)
        # 3. XenosRecomp unifie
        prog.update(message="XenosRecomp (SPIR-V)")
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

    def do_shaders_generic(self, prog, proj, sh):
        """Recolte generique (balayage des fichiers du jeu et de l'image) + shaders manques d'une partie precedente."""
        missing = os.path.join(data_home(), "games", self.info["title_id"], "missing_shaders")
        nmiss = len([f for f in os.listdir(missing) if f.endswith(".bin")]) if os.path.isdir(missing) else 0
        rev = self.keyparts["xex_image"][:12] + self.keyparts["xenosrecomp"] + self.keyparts["sdk_hash"] + f"m{nmiss}"
        if self.stamped("shaders", rev) and os.path.isfile(os.path.join(sh, "shader_cache.cpp")) and not self.force:
            log("cache de shaders deja fait")
            return
        shutil.rmtree(sh, ignore_errors=True)
        os.makedirs(sh)
        lp = os.path.join(self.cache, "shaders.log")
        inp = os.path.join(sh, "input")
        t0 = time.time()
        est = self.recipe.get("build", {}).get("estimate_minutes", {}).get("shaders", 6) * 60
        prog.tick_fn = lambda: prog.sub(min(0.95, (time.time() - t0) / est), None)
        prog.update(message="balayage des conteneurs de microcode (fichiers du jeu, image, shaders manques)")
        image = os.path.join(self.cache, "image", next(f for f in os.listdir(os.path.join(self.cache, "image")) if f.endswith(".bin")))
        run_logged([sys.executable, "-I", os.path.join(self.rdir, "tools", "scan_shaders.py"), self.game_dir, image, inp, missing], lp)
        prog.update(message="XenosRecomp (SPIR-V)")
        env = {"XENOS_RECOMP_MICROCODE_KEY": "1", "XENOS_RECOMP_NO_ZSTD": "1",
               "LD_LIBRARY_PATH": os.path.join(XENOSRECOMP_DIR, "thirdparty", "dxc-bin", "lib", "x64")}
        run_logged([XENOSRECOMP, inp, os.path.join(sh, "shader_cache.cpp"),
                    os.path.join(XENOSRECOMP_DIR, "XenosRecomp", "shader_common.h")], lp, env=env)
        n = len(re.findall(r"(?m)^\t\{ 0x", open(os.path.join(sh, "shader_cache.cpp")).read()))
        log(f"{n} shaders dans le cache ({nmiss} issus de parties precedentes)")
        json.dump({"shaders": n, "missing_added": nmiss}, open(os.path.join(sh, "info.json"), "w"))
        shutil.rmtree(inp, ignore_errors=True)
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
        build_cmd = ["cmake", "--build", bdir, "-j", str(jobs), "--target", self.recipe["codegen"]["project"]]
        rc = run_logged(build_cmd, lp, on_line=on_line, check=self.mode != "generic")
        if rc != 0:   # mode generique : imports du XEX sans implementation (ordinaux inconnus) -> stubs generes, puis 2e passe
            text = open(lp, errors="replace").read()
            undef = sorted(set(re.findall(r"undefined (?:reference to|symbol:) [`']?(sub_[0-9A-F]{8})", text)))
            if not undef:
                raise RecipeError("compilation en echec :\n" + "".join(text.splitlines(True)[-25:]))
            sp = os.path.join(proj, "gen", "auto_stubs.cpp")
            codegen_loop.write_auto_stubs(sp, undef)
            log(f"{len(undef)} import(s) inconnu(s) -> stubs generes (gen/auto_stubs.cpp)")
            run_logged(["cmake", "-S", proj, "-B", bdir], lp)
            run_logged(build_cmd, lp, on_line=on_line)
        # produits du cache : binaire + bibliotheques dans bin/
        bin_dir = os.path.join(self.cache, "bin")
        shutil.rmtree(bin_dir, ignore_errors=True)
        os.makedirs(bin_dir)
        exe = self.recipe["codegen"]["project"]
        shutil.copy(os.path.join(bdir, exe), bin_dir)
        for d in (bdir, os.path.join(SDK, "lib")):     # librexruntime.so : RUNPATH $ORIGIN, donc a cote du binaire
            for f in os.listdir(d):
                if f.startswith("librexruntime") and (f.endswith(".so") or ".so." in f):
                    shutil.copy(os.path.join(d, f), bin_dir)
            if any(f.startswith("librexruntime") for f in os.listdir(bin_dir)):
                break
        if not any(f.startswith("librexruntime") for f in os.listdir(bin_dir)):
            raise RecipeError("librexruntime.so introuvable (SDK installe dans sdk/ ?)")
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
        env = dict(os.environ, DISPLAY=display)
        env.pop("LD_LIBRARY_PATH", None)   # comme le lanceur du Dashboard : le binaire doit trouver seul ses bibliotheques
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
            m = re.search(r"Call to invalid or unregistered function at guest address 0x([0-9A-Fa-f]{8})", text)
            if m and self.mode == "generic":
                # fonction invoquee par pointeur que l'analyse statique n'a pas vue : declaree dans le cache utilisateur, a la prochaine
                # construction le codegen l'inclut (boucle « jouer -> reconstruire », docs/recompilateur_generique.md)
                a = int(m.group(1), 16)
                up = user_functions_path(self.info["title_id"])
                os.makedirs(os.path.dirname(up), exist_ok=True)
                with open(up, "a") as f:
                    f.write(f"0x{a:08X} = {{}}  # appelee a l'execution sans etre declaree (bootstrap)\n")
                raise MissingFunction(f"test d'amorcage : fonction 0x{a:08X} non declaree au codegen, ajoutee a {up} ; relancer la "
                                      f"construction (boucle jouer -> reconstruire), voir {lp}")
            raise RecipeError("test d'amorcage : erreur fatale, voir " + lp)
        for pat in bt.get("expect", []):
            if pat not in text:
                raise RecipeError(f"test d'amorcage : '{pat}' absent du journal ({lp})")
        if not alive:
            raise RecipeError("test d'amorcage : le jeu s'est arrete avant " + str(seconds) + " s, voir " + lp)

    def do_bootstrap_loop(self, prog):
        """Mode generique : amorcage -> fonction manquante -> codegen (une passe, reprise) -> compilation incrementale -> amorcage.
        Dernier filet apres la decouverte statique ; XD_BOOT_LOOPS borne le nombre de tours (defaut 12, 0 = aucun)."""
        turns = int(os.environ.get("XD_BOOT_LOOPS", "12")) if self.mode == "generic" else 0
        self.boot_turns = 0
        while True:
            try:
                return self.do_bootstrap(prog)
            except MissingFunction as e:
                if self.boot_turns >= turns:
                    raise
                self.boot_turns += 1
                log(f"{e}")
                log(f"tour {self.boot_turns}/{turns} de la boucle d'amorcage : codegen et compilation incrementale")
                t = time.time()
                prog.update(message=f"fonction manquante a l'execution : reconstruction ({self.boot_turns})")
                self.do_codegen(prog)
                self.do_compile(prog)
                self.t.setdefault("boot_loop", []).append(round(time.time() - t, 1))

    # -- orchestration
    def run(self):
        self.do_identify()
        info = self.info
        os.makedirs(self.cache, exist_ok=True)
        est = sum(self.recipe.get("build", {}).get("estimate_minutes", {"x": 30}).values()) * 60
        prog = Progress(self.cache, est)
        prog.update(title_id=info["title_id"], name=self.name, cache=self.cache, source=self.game_dir)
        prog.start_heartbeat()
        if self.ok() and not self.force and self.stages == STAGES:
            log(f"{self.name} : cache a jour ({self.cache})")
            prog.update(done=True, percent=100.0, message="deja construit")
            return self.cache
        t0 = time.time()
        steps = [("identify", None), ("prepare", self.do_prepare), ("extract", self.do_extract),
                 ("scaffold", self.do_scaffold), ("codegen", self.do_codegen), ("shaders", self.do_shaders),
                 ("compile", self.do_compile), ("bootstrap", self.do_bootstrap_loop)]
        msgs = {"identify": f"XEX {info['title_id']} v{info['version']} " + ("reconnu" if self.mode == "recipe" else
                                                                       "sans recette : mode generique (" + self.generic_reason + ")"),
                "prepare": "controles", "extract": "source du jeu",
                "scaffold": "projet de la recette" if self.mode == "recipe" else "analyse D3D (motifs de code) et projet generique",
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
                           "recipe_rev": recipe_rev(self.rdir), "mode": self.mode, "seconds": self.t, "built": time.strftime("%F %T"),
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
            "cwd": os.path.join(b.cache, "bin"), "cache": b.cache, "built": b.ok(), "mode": b.mode}


def identify_report(source, force_generic=False):
    """identify : TitleID, version, hachage, mode (recette/generique), XDK, fonctions D3D retrouvees ou manquantes, verdict."""
    home = data_home()
    game_dir, xex = resolve_source(source, os.path.join(home, "sources"))
    tmp = os.path.join(home, "tmp-identify")
    os.makedirs(tmp, exist_ok=True)
    info = identify(xex, tmp)
    image = info.pop("image", None)
    recipe, rdir = load_recipe(info["title_id"])
    info["recipe"] = bool(recipe)
    info["match"] = bool(recipe and match_xex(recipe, info))
    info["mode"] = "recipe" if info["match"] and not (force_generic or os.environ.get("XD_GENERIC")) else "generic"
    rep = analyze_xex(xex, image)
    shutil.rmtree(tmp, ignore_errors=True)
    info["xdk"] = rep["xdk"]
    info["static_libraries"] = rep["libs"]
    info["d3d"] = {"found": rep["roles"], "missing": rep["missing"], "weak": rep["weak"], "offsets": rep["offsets"],
                   # lot B2 : disposition de l'ombre, roles facultatifs absents, sources des decalages (absents des analyses en cache anciennes)
                   "layout": rep.get("layout"), "optional_missing": rep.get("optional_missing", []),
                   "offset_sources": rep.get("offset_sources", {})}
    if info["mode"] == "recipe":
        info["supported"] = True    # la recette fait foi (rendu natif de la recette), meme si le matcher generique est moins sur
        info["reason"] = ""
        info["generic_supported"] = rep["supported"]
        info["generic_reason"] = rep["reason"]
    else:
        info["supported"] = rep["supported"]
        info["reason"] = rep["reason"]
    info["extra_modules"] = extra_modules(game_dir, xex)
    if info["extra_modules"]:
        info["note"] = "XEX secondaires/DLL presents : non recompiles par le mode generique"
    return info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("identify")
    a.add_argument("source")
    a.add_argument("--generic", action="store_true", help="ignore la recette games/<TitleID>/")
    for n in ("build", "status", "launch-info"):
        s = sub.add_parser(n)
        s.add_argument("source")
        s.add_argument("--generic", action="store_true", help="ignore la recette games/<TitleID>/ (mode generique)")
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
            print(json.dumps(identify_report(args.source, args.generic), indent=1))
            return 0
        b = Build(args.source, force=getattr(args, "force", False),
                  stages=[s for s in getattr(args, "stages", "").split(",") if s] or None,
                  jobs=getattr(args, "jobs", None), container=getattr(args, "container", False), generic=args.generic)
        if args.cmd == "build":
            b.run()
            return 0
        b.do_identify()
        if args.cmd == "status":
            print(json.dumps({"cache": b.cache, "built": b.ok(), "key": b.key, "mode": b.mode}, indent=1))
        else:
            print(json.dumps(launch_info(b)))
        return 0
    except RecipeError as e:
        print(f"[recipe] ERREUR : {e}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    sys.exit(main())
