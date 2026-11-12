#!/usr/bin/env python3
"""Chaîne de portage automatisée d'un Dashboard Xbox 360 (phase P2 du PLAN).

À partir d'une mise à jour système (dossier $SystemUpdate, paquet STFS) ou directement d'un dash.xex,
produit un projet ReXGlue compilé et testé dans dashboards/<version>/ :

  extract   extraction STFS (tools/stfs.py), repérage de dash.xex, modules et polices flash
  assets    assets/dash.xex, assets/flash/ (modules, polices, shrdres.xzp converti)
  pe        déchiffrement/décompression de dash.xex (tools/xexinfo) -> image PE + sections
  scaffold  projet depuis le gabarit tools/templates/dashboard (n'écrase jamais l'existant)
  codegen   `rexglue codegen` + boucle de corrections -> overrides.toml (généré)
  build     cmake (preset linux-amd64-relwithdebinfo) + compilation (+ stubs d'imports manquants)
  smoke     test de fumée sous Xvfb + lavapipe : capture PNG, aucune ligne FATAL
  compare   (option --compare-with) comparaison avec un projet de référence

Code de retour non nul si une étape échoue. Voir README, « Porter une nouvelle version ».
"""
import argparse
import glob
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
TEMPLATE = os.path.join(TOOLS, "templates", "dashboard")
SDK = os.path.join(ROOT, "sdk")            # préfixe d'installation du SDK (--sdk-prefix pour en essayer un autre)
REXGLUE = os.path.join(SDK, "bin", "rexglue")
BUILD_DIR_NAME = PRESET = "linux-amd64-relwithdebinfo"
XEXINFO = os.path.join(TOOLS, "xexinfo", "build", "xexinfo")
STAGES = ["extract", "assets", "pe", "scaffold", "codegen", "build", "smoke", "compare"]

# Polices : nom canonique dans la flash console (XeBuilds les fournit en minuscules)
FONT_NAMES = {"xenonclatin.xtt": "XenonCLatin.xtt", "xenonjklatin.xtt": "XenonJKLatin.xtt"}
# Ressources de xam.xex à fournir aussi comme media:\<nom>.xzp (convertie comme shrdres.xzp)
XAM_XZP_RESOURCES = ["gamercrd"]
# Gabarit : fichiers copiés s'ils n'existent pas encore (relatif au gabarit -> relatif au projet)
TEMPLATE_FILES = ["CMakeLists.txt", "CMakePresets.json", "dash_manifest.toml", "dash_overrides.toml",
                  "src/app.h", "src/main.cpp", "run_headless.sh", "input_test.sh"]


class StageError(Exception):
    pass


def log(msg):
    print(f"[port] {msg}", flush=True)


def run(cmd, cwd=None, log_path=None, env=None, check=True):
    """Exécute une commande, journalise sa sortie dans log_path (et retourne code, texte)."""
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace")
    if log_path:
        with open(log_path, "w") as f:
            f.write(p.stdout)
    if check and p.returncode != 0:
        tail = "\n".join(p.stdout.splitlines()[-25:])
        raise StageError(f"commande en échec ({p.returncode}) : {' '.join(map(str, cmd))}\n{tail}")
    return p.returncode, p.stdout


# --------------------------------------------------------------------------------------------------
# 1. Extraction
# --------------------------------------------------------------------------------------------------
def is_stfs(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in (b"PIRS", b"LIVE", b"CON ")
    except OSError:
        return False


def stage_extract(a, ctx):
    ex = os.path.join(ctx["work"], "extract")
    shutil.rmtree(ex, ignore_errors=True)
    os.makedirs(ex)
    src = a.source
    if not os.path.exists(src):
        raise StageError(f"source introuvable : {src}")
    files = []  # fichiers « plats » (déjà extraits ou issus d'un paquet STFS)
    if os.path.isfile(src) and not is_stfs(src):
        # dash.xex seul
        shutil.copy(src, os.path.join(ex, "$flash_dash.xex"))
    else:
        paths = [src] if os.path.isfile(src) else [os.path.join(dp, f) for dp, _, fs in os.walk(src) for f in fs]
        n = 0
        for p in sorted(paths):
            if is_stfs(p):
                sub = os.path.join(ex, os.path.basename(p))
                log(f"extraction STFS : {p}")
                run([sys.executable, os.path.join(TOOLS, "stfs.py"), p, sub])
                for dp, _, fs in os.walk(sub):
                    for f in fs:
                        files.append(os.path.join(dp, f))
                n += 1
            elif p.lower().endswith((".xex", ".xtt")):
                files.append(p)
        if not files:
            raise StageError("aucun paquet STFS ni module trouvé dans la source")
        # le dossier $SystemUpdate d'une mise à jour récente contient aussi des .xex déjà en clair
    ctx["files"] = files or [os.path.join(ex, f) for f in os.listdir(ex)]
    dash = None
    for want in ("$flash_dash.xex", "dash.xex"):
        for f in ctx["files"]:
            if os.path.basename(f).lower() == want:
                dash = f
                break
        if dash:
            break
    if not dash:
        have = sorted(os.path.basename(f) for f in ctx["files"] if "dash" in f.lower())
        raise StageError(f"dash.xex introuvable dans la source (candidats : {have}). Une mise à jour dont le "
                         "dash n'est qu'un delta (.xexp) ne suffit pas : fournir le dash.xex complet.")
    ctx["dash"] = dash
    log(f"dash.xex : {dash} ({os.path.getsize(dash)} octets)")


def load_ctx_extract(ctx):
    """Reprend le contexte d'extraction si l'étape a été sautée."""
    ex = os.path.join(ctx["work"], "extract")
    if "dash" in ctx:
        return
    if not os.path.isdir(ex):
        raise StageError("étape extract jamais exécutée (work/port/<version>/extract absent)")
    files = [os.path.join(dp, f) for dp, _, fs in os.walk(ex) for f in fs]
    ctx["files"] = files
    for want in ("$flash_dash.xex", "dash.xex"):
        for f in files:
            if os.path.basename(f).lower() == want:
                ctx["dash"] = f
                return
    raise StageError("dash.xex absent de l'extraction")


# --------------------------------------------------------------------------------------------------
# 2. Ressources (assets/)
# --------------------------------------------------------------------------------------------------
def flash_name(basename):
    """Nom dans la flash console pour un fichier de la mise à jour (« $flash_X.xex », « $install_X.xex »)."""
    for pre in ("$flash_", "$install_"):
        if basename.startswith(pre):
            return basename[len(pre):]
    return basename


def stage_assets(a, ctx):
    load_ctx_extract(ctx)
    assets = os.path.join(ctx["dest"], "assets")
    flash = os.path.join(assets, "flash")
    os.makedirs(flash, exist_ok=True)
    shutil.copy(ctx["dash"], os.path.join(assets, "dash.xex"))
    n = 0
    if a.flash_dir:  # archive XeBuilds : modules complets + polices de la version
        for f in sorted(os.listdir(a.flash_dir)):
            low = f.lower()
            if low == "xam.xex":
                continue  # non déchiffrable ici / inutilisé (XAM émulé)
            if low.endswith(".xex"):
                shutil.copy(os.path.join(a.flash_dir, f), os.path.join(flash, f))
                n += 1
            elif low in FONT_NAMES:
                shutil.copy(os.path.join(a.flash_dir, f), os.path.join(flash, FONT_NAMES[low]))
                n += 1
    for f in sorted(ctx["files"]):  # modules complets de la mise à jour (prioritaires)
        b = os.path.basename(f)
        if b.startswith(("$flash_", "$install_")) and b.lower().endswith((".xex", ".xtt")):
            nm = flash_name(b)
            if nm.lower() == "xam.xex":
                continue
            shutil.copy(f, os.path.join(flash, FONT_NAMES.get(nm.lower(), nm)))
            n += 1
    if not a.flash_dir:
        log("avertissement : pas de --flash-dir (archive XeBuilds) : seuls les modules complets de la mise "
            "à jour sont copiés ; les .xexp (deltas) ne sont pas appliqués")
    # Polices absentes (la mise à jour ne fournit que des correctifs .xttp) : substitution par celles de 6770
    for fn in FONT_NAMES.values():
        cands = [os.path.join(ROOT, "dashboards", "2.0.6770.0", "assets", "flash", fn)]
        if getattr(a, "fonts_dir", None):  # --fonts-dir : dossier contenant XenonCLatin.xtt / XenonJKLatin.xtt (casse libre)
            cands = [os.path.join(a.fonts_dir, f) for f in (os.listdir(a.fonts_dir) if os.path.isdir(a.fonts_dir) else [])
                     if f.lower() == fn.lower()] + cands
        sub = next((c for c in cands if os.path.exists(c)), None)
        if not os.path.exists(os.path.join(flash, fn)) and sub:
            shutil.copy(sub, os.path.join(flash, fn))
            log(f"police {fn} absente : substituée par {sub}")
    log(f"flash : {len(os.listdir(flash))} fichiers dans {flash}")
    # shrdres.xzp : le xam.xex complet de la mise à jour elle-même (même version) est prioritaire
    own_xam = [f for f in ctx["files"] if os.path.basename(f).lower() == "$flash_xam.xex"]
    if own_xam:
        a.shrdres_xam = own_xam[0]
        log(f"xam.xex de la mise à jour utilisé pour shrdres/gamercrd : {own_xam[0]}")
    out = os.path.join(flash, "shrdres.xzp")
    if a.shrdres:
        shutil.copy(a.shrdres, out)
        log(f"shrdres.xzp copié depuis {a.shrdres}")
    elif a.shrdres_xam and os.path.exists(a.shrdres_xam):
        pe = os.path.join(ctx["work"], "shrdres_xam.pe")
        # xam.xex dont la clé de session est chiffrée avec la clé XEX1 (2.0.13604, Blades) : rechiffrée avec la clé retail
        rekey = os.path.join(ctx["work"], "shrdres_xam_rekey.xex")
        run([sys.executable, os.path.join(TOOLS, "xex_rekey.py"), a.shrdres_xam, rekey])
        a.shrdres_xam = rekey
        raw = os.path.join(ctx["work"], "shrdres_raw.xzp")
        run([XEXINFO, a.shrdres_xam, pe], log_path=os.path.join(ctx["work"], "shrdres_xexinfo.txt"))
        run([sys.executable, os.path.join(TOOLS, "xex_resource.py"), a.shrdres_xam, pe, "shrdres", raw])
        run([sys.executable, os.path.join(TOOLS, "xzp_alias.py"), raw, out])
        log(f"shrdres.xzp converti depuis {a.shrdres_xam}")
        # autres ressources de XAM ouvertes par le Dashboard via media:\<nom>.xzp (gamercrd : carte de joueur, 5759)
        for res in XAM_XZP_RESOURCES:
            raw = os.path.join(ctx["work"], f"{res}_raw.xzp")
            rc, _ = run([sys.executable, os.path.join(TOOLS, "xex_resource.py"), a.shrdres_xam, pe, res, raw],
                        check=False)
            if rc == 0 and os.path.exists(raw):
                run([sys.executable, os.path.join(TOOLS, "xzp_alias.py"), raw, os.path.join(flash, res + ".xzp")])
                log(f"{res}.xzp converti depuis {a.shrdres_xam}")
    else:
        log("avertissement : pas de shrdres.xzp (--shrdres ou --shrdres-xam) : icônes/chaînes partagées absentes")
    extract_dash_resources(ctx, assets, flash)


def extract_dash_resources(ctx, assets, flash):
    """Ressources XZP embarquées dans dash.xex (dashcomm, consoles, network, slots, hubui... pour NXE/Metro) : le
    Dashboard les demande par « langpack://L..<nom>.xzp » (XamPackageManagerGetFilePathW -> media:\\<nom>.xzp). Elles sont
    déjà au format natif (XUIZ) : copiées telles quelles dans la flash (sans écraser un fichier existant). Le
    Dashboard 17559 ouvre aussi media:\\en-US\\shrdres.xzp : on y place un lien vers shrdres.xzp."""
    import struct
    xex = os.path.join(assets, "dash.xex")
    pe = os.path.join(ctx["work"], "dash_res.pe")
    run([XEXINFO, xex, pe], log_path=os.path.join(ctx["work"], "dash_res_xexinfo.txt"))
    data, img = open(xex, "rb").read(), open(pe, "rb").read()
    n, = struct.unpack(">I", data[0x14:0x18])
    opt = dict(struct.unpack(">II", data[0x18 + i * 8:0x20 + i * 8]) for i in range(n))
    if 0x2FF not in opt or 0x10201 not in opt:
        return
    base, v = opt[0x10201], opt[0x2FF]
    size, = struct.unpack(">I", data[v:v + 4])
    count = 0
    for j in range((size - 4) // 16):
        e = data[v + 4 + j * 16:v + 20 + j * 16]
        name = e[:8].rstrip(b"\0").decode("latin1")
        addr, length = struct.unpack(">II", e[8:16])
        blob = img[addr - base:addr - base + length]
        out = os.path.join(flash, name + ".xzp")
        if blob[:4] == b"XUIZ" and not os.path.exists(out):
            open(out, "wb").write(blob)
            count += 1
    if count:
        log(f"{count} ressources XZP de dash.xex copiées dans la flash (media:\\<nom>.xzp)")
        loc = os.path.join(flash, "en-US")
        os.makedirs(loc, exist_ok=True)
        link = os.path.join(loc, "shrdres.xzp")
        if os.path.exists(os.path.join(flash, "shrdres.xzp")) and not os.path.lexists(link):
            os.symlink(os.path.join("..", "shrdres.xzp"), link)


# --------------------------------------------------------------------------------------------------
# 3. Image PE
# --------------------------------------------------------------------------------------------------
def stage_pe(a, ctx):
    xex = os.path.join(ctx["dest"], "assets", "dash.xex")
    if not os.path.exists(xex):
        raise StageError("assets/dash.xex absent (étape assets)")
    pe = os.path.join(ctx["work"], "dash.pe")
    info = os.path.join(ctx["work"], "dash_info.txt")
    run([XEXINFO, xex, pe], log_path=info)
    ctx["pe"] = pe
    txt = open(info, errors="replace").read()
    m = re.search(r"BASE (0x[0-9A-Fa-f]+) SIZE (0x[0-9A-Fa-f]+) ENTRY (0x[0-9A-Fa-f]+)", txt)
    if not m:
        raise StageError(f"xexinfo n'a pas produit d'en-tête exploitable ({info})")
    ctx["base"] = int(m.group(1), 16)
    secs = {}
    for mm in re.finditer(r"^SECT (\S+)\s+(0x[0-9A-Fa-f]+) (0x[0-9A-Fa-f]+) flags=(\d+)", txt, re.M):
        secs.setdefault(mm.group(1), (int(mm.group(2), 16), int(mm.group(3), 16), int(mm.group(4))))
    ctx["secs"] = secs
    if ".text" not in secs:
        raise StageError("section .text introuvable")
    log(f"PE : base 0x{ctx['base']:08X}, entrée {m.group(3)}, {len(secs)} sections")


def load_ctx_pe(ctx):
    if "pe" in ctx:
        return
    stage_pe(None, ctx)  # reprise après l'étape pe : on la refait (quelques secondes)


# --------------------------------------------------------------------------------------------------
# 4. Gabarit
# --------------------------------------------------------------------------------------------------
def stage_scaffold(a, ctx):
    dest = ctx["dest"]
    os.makedirs(os.path.join(dest, "src"), exist_ok=True)
    for rel in TEMPLATE_FILES:
        src = os.path.join(TEMPLATE, rel)
        dst = os.path.join(dest, rel)
        if os.path.exists(dst) and not a.refresh_template:
            continue
        txt = open(src).read().replace("@VERSION@", a.version)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "w") as f:
            f.write(txt)
        if os.access(src, os.X_OK):
            os.chmod(dst, 0o755)
        log(f"gabarit -> {os.path.relpath(dst, ROOT)}")
    # profil de test (GPD fictifs, non Microsoft) repris du projet 6770 pour les parcours à l'écran
    prof = os.path.join(dest, "assets", "profiles")
    ref = os.path.join(ROOT, "dashboards", "2.0.6770.0", "assets", "profiles")
    if not os.path.exists(prof) and os.path.isdir(ref) and os.path.abspath(ref) != os.path.abspath(prof):
        shutil.copytree(ref, prof)
        log("profil de test copié depuis dashboards/2.0.6770.0/assets/profiles")
    ov = os.path.join(dest, "overrides.toml")
    if not os.path.exists(ov):
        open(ov, "w").write("")


# --------------------------------------------------------------------------------------------------
# 5. codegen + corrections automatiques
# --------------------------------------------------------------------------------------------------
ADDR = r"0x([0-9A-Fa-f]{8})"


def manual_keys(dest):
    """Adresses déjà déclarées dans les corrections manuelles (à ne pas répéter dans overrides.toml)."""
    p = os.path.join(dest, "dash_overrides.toml")
    keys = set()
    if os.path.exists(p):
        for line in open(p):
            m = re.match(r"\s*0x([0-9A-Fa-f]{8})\s*=", line)
            if m:
                keys.add(int(m.group(1), 16))
    return keys


def write_overrides(dest, unresolved, ptr, cont=None):
    lines = [
        "# Corrections AUTOMATIQUES de l'analyse de dash.xex, écrites par tools/port_dashboard.sh.",
        "# Ne pas éditer : régénéré à chaque exécution (les corrections manuelles vont dans dash_overrides.toml).",
        "[functions]",
        "# Cibles de branchements que l'analyseur n'a rattachées à aucune fonction (erreurs « UnresolvedCall »).",
    ]
    for addr, src in sorted(unresolved.items()):
        lines.append(f"0x{addr:08X} = {{}}  # branche depuis 0x{src:08X}")
    lines.append("")
    lines.append("# Fonctions atteintes seulement par pointeur (vtables/callbacks) ou dont l'adresse est construite")
    lines.append("# par lis/addi : tools/find_ptr_funcs.py + tools/filter_inside.py (les continuations de catch et les")
    lines.append("# cibles des tables de portée SEH, situées dans une fonction, sont écartées).")
    for addr, ref in sorted(ptr.items()):
        lines.append(f"0x{addr:08X} = {{}}  # ref 0x{ref:08X}")
    if cont:
        lines.append("")
        lines.append("# Continuations de funclets (finally/catch) : le funclet se termine par `b` vers un point situé dans sa")
        lines.append("# fonction parente. On déclare ce point comme fonction (jusqu'au premier blr/b), le funclet l'appelle puis")
        lines.append("# retourne au parent, qui exécute lui-même la suite (le code en double est sans effet).")
        for addr, (end, src) in sorted(cont.items()):
            lines.append(f"0x{addr:08X} = {{ end = 0x{end:08X} }}  # continuation depuis 0x{src:08X}")
    with open(os.path.join(dest, "overrides.toml"), "w") as f:
        f.write("\n".join(lines) + "\n")


def block_end(ctx, start):
    """Fin (exclue) du bloc commençant à `start` : première instruction terminale (blr, bctr, b sans lien)."""
    img = ctx["pe_data"]
    a = start
    for _ in range(512):
        off = a - ctx["base"]
        w = struct.unpack(">I", img[off:off + 4])[0]
        a += 4
        if w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and not (w & 1)):
            return a
    return a


def scan_ptr_funcs(ctx, gen_dir, manual, known_extra):
    """Candidats par pointeur, hors fonctions déjà générées. Retourne {adresse: ref}."""
    secs = ctx["secs"]
    ts, tsz, _ = secs[".text"]
    ranges = []
    for nm in (".rdata", ".data"):
        if nm in secs:
            s, sz, _ = secs[nm]
            ranges.append(f"{s:X}:{s + sz:X}")
    if not ranges:
        raise StageError("ni .rdata ni .data dans l'image")
    cand = os.path.join(ctx["work"], "ptr_candidates.toml")
    _, out = run([sys.executable, os.path.join(TOOLS, "find_ptr_funcs.py"), ctx["pe"], f"{ctx['base']:X}",
                  os.path.join(gen_dir, "dash_init.cpp"), f"{ts:X}", f"{ts + tsz:X}"] + ranges)
    open(cand, "w").write(out)
    _, kept = run([sys.executable, os.path.join(TOOLS, "filter_inside.py"), gen_dir, cand])
    res = {}
    for line in kept.splitlines():
        m = re.match(r"0x([0-9A-F]{8}) = \{\}\s+# ref 0x([0-9A-F]{8})", line)
        if m:
            a_, r_ = int(m.group(1), 16), int(m.group(2), 16)
            if a_ not in manual and a_ not in known_extra:
                res[a_] = r_
    return res


def stage_codegen(a, ctx):
    load_ctx_pe(ctx)
    dest = ctx["dest"]
    if not os.path.exists(os.path.join(dest, "dash_manifest.toml")):
        raise StageError("projet absent (étape scaffold)")
    manual = manual_keys(dest)
    unresolved, ptr, cont = {}, {}, {}
    ctx["pe_data"] = open(ctx["pe"], "rb").read()
    gen_dir = os.path.join(dest, "generated", "dash")
    write_overrides(dest, unresolved, ptr, cont)
    final_warn = ""
    for it in range(1, a.max_iter + 1):
        lp = os.path.join(ctx["work"], f"codegen{it}.log")
        t0 = time.time()
        rc, out = run([REXGLUE, "codegen", "dash_manifest.toml"], cwd=dest, log_path=lp, check=False)
        log(f"codegen #{it} : code {rc} ({time.time() - t0:.0f} s) -> {os.path.relpath(lp, ROOT)}")
        if rc != 0:
            new = {}
            for m in re.finditer(rf"^\s+{ADDR} from {ADDR}: b ", out, re.M):
                t, s = int(m.group(1), 16), int(m.group(2), 16)
                if t not in unresolved and t not in manual:
                    new.setdefault(t, s)
            if not new:
                raise StageError("codegen en échec sans erreur « UnresolvedCall » exploitable :\n"
                                 + "\n".join(out.splitlines()[-20:]))
            unresolved.update(new)
            log(f"  {len(new)} cible(s) non résolue(s) ajoutée(s)")
            write_overrides(dest, unresolved, ptr, cont)
            continue
        final_warn = "\n".join(l for l in out.splitlines() if l.startswith("Unresolved"))
        new = scan_ptr_funcs(ctx, gen_dir, manual, set(unresolved) | set(ptr))
        log(f"  fonctions par pointeur : {len(new)} nouvelles")
        # continuations de funclets : « Unresolved b target T from S » avec T dans une fonction déjà générée
        newc = {}
        for m in re.finditer(rf"^Unresolved b target {ADDR} from {ADDR}", final_warn, re.M):
            t, s_ = int(m.group(1), 16), int(m.group(2), 16)
            if t not in manual and t not in unresolved and t not in ptr and t not in cont:
                newc[t] = (block_end(ctx, t), s_)
        if newc:
            log(f"  {len(newc)} continuation(s) de funclet déclarée(s) comme fonction")
            cont.update(newc)
            write_overrides(dest, unresolved, ptr, cont)
            ptr.update(new)
            write_overrides(dest, unresolved, ptr, cont)
            continue
        if not new:
            ctx["codegen_warnings"] = final_warn
            n_fn = len(re.findall(r"\{ 0x[0-9A-F]{8}, sub_", open(os.path.join(gen_dir, "dash_init.cpp")).read()))
            log(f"codegen terminé : {n_fn} fonctions, {len(unresolved)} cibles non résolues, "
                f"{len(ptr)} fonctions par pointeur")
            if final_warn:
                log("  avertissements résiduels (à étudier, ex. retour de funclet catch) :\n    "
                    + final_warn.replace("\n", "\n    "))
            return
        ptr.update(new)
        write_overrides(dest, unresolved, ptr, cont)
    raise StageError(f"codegen : pas de convergence en {a.max_iter} itérations")


# --------------------------------------------------------------------------------------------------
# 6. Compilation
# --------------------------------------------------------------------------------------------------
def stage_build(a, ctx):
    dest = ctx["dest"]
    bdir = os.path.join(dest, "out", "build", BUILD_DIR_NAME)
    if not os.path.exists(os.path.join(bdir, "build.ninja")):
        if BUILD_DIR_NAME == PRESET:
            run(["cmake", "--preset", PRESET, f"-DCMAKE_PREFIX_PATH={SDK}"], cwd=dest,
                log_path=os.path.join(ctx["work"], "cmake_configure.log"))
        else:  # SDK alternatif : dossier de compilation à part, mêmes variables que le préréglage
            run(["cmake", "-S", dest, "-B", bdir, "-G", "Ninja", "-DCMAKE_C_COMPILER=clang",
                 "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_PREFIX_PATH={SDK}"],
                cwd=dest, log_path=os.path.join(ctx["work"], "cmake_configure.log"))
    for attempt in (1, 2):
        lp = os.path.join(ctx["work"], f"build{attempt}.log")
        t0 = time.time()
        rc, out = run(["cmake", "--build", bdir, "-j", str(a.jobs)], cwd=dest, log_path=lp,
                      check=False)
        log(f"compilation #{attempt} : code {rc} ({time.time() - t0:.0f} s) -> {os.path.relpath(lp, ROOT)}")
        if rc == 0:
            break
        # Imports XAM/noyau sans implémentation : le code généré référence sub_XXXXXXXX non défini
        undef = sorted(set(re.findall(r"undefined (?:reference to|symbol:) [`']?(sub_[0-9A-F]{8})", out)))
        if attempt == 1 and undef:
            sp = os.path.join(dest, "src", "auto_stubs.cpp")
            with open(sp, "w") as f:
                f.write("// Généré par tools/port_dashboard.sh : imports du XEX sans implémentation dans le SDK\n"
                        "// (thunks « sub_XXXXXXXX » = ordinaux inconnus, voir « Cannot resolve ordinal » dans le journal\n"
                        "// de codegen). Stubs retournant 0 ; à remplacer par une vraie implémentation si utilisés.\n"
                        "#include <rex/hook.h>\n#include <rex/types.h>\n\n")
                for s in undef:
                    f.write(f"REX_STUB_RETURN({s}, 0)\n")
            log(f"  {len(undef)} symbole(s) non défini(s) -> {os.path.relpath(sp, ROOT)}")
            continue
        raise StageError("compilation en échec :\n" + "\n".join(out.splitlines()[-30:]))
    link = os.path.join(bdir, "librexgpu-xenos.so")
    if not os.path.lexists(link):
        os.symlink(os.path.join(SDK, "lib", "librexgpu-xenos.so"), link)
    if not os.path.exists(os.path.join(bdir, "dash")):
        raise StageError("l'exécutable dash n'a pas été produit")


# --------------------------------------------------------------------------------------------------
# 7. Test de fumée
# --------------------------------------------------------------------------------------------------
def free_display():
    for n in range(80, 140):
        if not os.path.exists(f"/tmp/.X11-unix/X{n}") and not os.path.exists(f"/tmp/.X{n}-lock"):
            return n
    raise StageError("aucun affichage X libre")


def read_png(path):
    """Décodeur minimal pour les PNG écrits par tools/xwd2png.py (RGB 8 bits, filtre 0). -> (w, h, octets RGB)."""
    d = open(path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(d):
        ln, = struct.unpack(">I", d[pos:pos + 4])
        t = d[pos + 4:pos + 8]
        c = d[pos + 8:pos + 8 + ln]
        if t == b"IHDR":
            w, h = struct.unpack(">II", c[:8])
        elif t == b"IDAT":
            idat += c
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * 3 + 1
    return w, h, b"".join(raw[y * stride + 1:(y + 1) * stride] for y in range(h))


def png_stats(path):
    w, h, px = read_png(path)
    colors = len({px[i:i + 3] for i in range(0, len(px), 3 * 7)})  # échantillon
    return w, h, colors


def png_diff(p1, p2):
    """(part de pixels différents, écart moyen par canal)."""
    w1, h1, a_ = read_png(p1)
    w2, h2, b_ = read_png(p2)
    if (w1, h1) != (w2, h2):
        return 1.0, 255.0
    nd = sum(1 for i in range(0, len(a_), 3) if a_[i:i + 3] != b_[i:i + 3])
    tot = sum(abs(x - y) for x, y in zip(a_, b_))
    return nd / (w1 * h1), tot / len(a_)


def stage_smoke(a, ctx):
    dest = ctx["dest"]
    exe = os.path.join(dest, "out", "build", BUILD_DIR_NAME, "dash")
    if not os.path.exists(exe):
        raise StageError("exécutable absent (étape build)")
    runs = os.path.join(dest, "runs")
    name = a.smoke_name
    fb = os.path.join(runs, name + ".fb")
    shutil.rmtree(fb, ignore_errors=True)
    os.makedirs(fb)
    logdir = os.path.join(dest, "out", "build", BUILD_DIR_NAME, "logs")
    before = set(glob.glob(os.path.join(logdir, "*.log")))
    disp = free_display()
    xvfb = subprocess.Popen(["Xvfb", f":{disp}", "-screen", "0", "1280x720x24", "-fbdir", fb],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    dash = None
    out_path = os.path.join(runs, name + ".out")
    png = os.path.join(runs, name + ".png")
    try:
        time.sleep(1.5)
        if xvfb.poll() is not None:
            raise StageError("Xvfb n'a pas démarré")
        env = dict(os.environ, DISPLAY=f":{disp}", DASH_AUDIO="nop", MESA_VK_WSI_PRESENT_MODE="immediate",
                   VK_ICD_FILENAMES=os.environ.get("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json"),
                   LD_LIBRARY_PATH=os.path.join(SDK, "lib"))
        with open(out_path, "w") as fo:
            dash = subprocess.Popen([exe, f"--game_data_root={os.path.join(dest, 'assets')}", "--gpu_plugin=xenos",
                                     "--log_flush_interval=1"], env=env, stdout=fo, stderr=subprocess.STDOUT,
                                    cwd=dest, start_new_session=True)
        t0 = time.time()
        while time.time() - t0 < a.smoke_seconds and dash.poll() is None:
            time.sleep(0.5)
        alive = dash.poll() is None
        code = dash.returncode
        subprocess.run([sys.executable, os.path.join(TOOLS, "xwd2png.py"), os.path.join(fb, "Xvfb_screen0"), png],
                       check=True)
    finally:
        for p in (dash, xvfb):
            if p is not None and p.poll() is None:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                p.wait()
    errs = []
    if not alive:
        errs.append(f"dash s'est arrêté avant {a.smoke_seconds} s (code {code})")
    logs = sorted(set(glob.glob(os.path.join(logdir, "*.log"))) - before, key=os.path.getmtime)
    fatal = []
    for lg in logs + [out_path]:
        for line in open(lg, errors="replace"):
            if re.search(r"\bFATAL\b|\[critical\]", line):
                fatal.append(line.rstrip())
    if fatal:
        errs.append(f"{len(fatal)} ligne(s) FATAL, p. ex. : {fatal[0]}")
    w, h, colors = png_stats(png)
    log(f"capture : {os.path.relpath(png, ROOT)} ({w}x{h}, ~{colors} couleurs échantillonnées)")
    if colors < 64:
        errs.append(f"capture quasi uniforme ({colors} couleurs) : rien n'est affiché")
    if a.reference_png:
        frac, mean = png_diff(png, a.reference_png)
        log(f"écart avec {a.reference_png} : {frac * 100:.2f} % de pixels différents, écart moyen {mean:.2f}/255")
        ctx["png_diff"] = (frac, mean)
        if mean > a.max_diff:  # le fond animé fait varier presque chaque pixel : on juge l'écart moyen
            errs.append(f"capture différente de la référence (écart moyen {mean:.2f} > {a.max_diff:.2f})")
    if errs:
        raise StageError("test de fumée en échec :\n  " + "\n  ".join(errs))
    log(f"test de fumée OK (dash vivant après {a.smoke_seconds} s, aucun FATAL)")


# --------------------------------------------------------------------------------------------------
# 8. Comparaison avec un projet de référence
# --------------------------------------------------------------------------------------------------
def function_table(proj):
    p = os.path.join(proj, "generated", "dash", "dash_init.cpp")
    return set(re.findall(r"\{ (0x[0-9A-F]{8}), sub_", open(p).read()))


def override_keys(proj):
    keys = set()
    for f in ("dash_overrides.toml", "overrides.toml"):
        p = os.path.join(proj, f)
        if os.path.exists(p):
            keys |= {"0x" + m.upper() for m in re.findall(r"(?m)^\s*0x([0-9A-Fa-f]{8})\s*=", open(p).read())}
    return keys


def stage_compare(a, ctx):
    ref = os.path.abspath(a.compare_with)
    dest = ctx["dest"]
    fa, fb = function_table(ref), function_table(dest)
    oa, ob = override_keys(ref), override_keys(dest)
    log(f"fonctions : référence {len(fa)}, régénéré {len(fb)}, différences {len(fa ^ fb)}")
    log(f"adresses d'overrides : référence {len(oa)}, régénéré {len(ob)}")
    for tag, s in (("seulement dans la référence", oa - ob), ("seulement dans le régénéré", ob - oa)):
        if s:
            log(f"  {tag} ({len(s)}) : {' '.join(sorted(s)[:12])}{' ...' if len(s) > 12 else ''}")
    if fa != fb:
        raise StageError("les tables de fonctions diffèrent")
    log("tables de fonctions identiques")


# --------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="dossier $SystemUpdate / mise à jour extraite, paquet STFS, ou dash.xex")
    ap.add_argument("version", help="nom de version, ex. 2.0.6770.0")
    ap.add_argument("--dest", help="dossier du projet (défaut : dashboards/<version>)")
    ap.add_argument("--work", help="dossier de travail (défaut : work/port/<version>)")
    ap.add_argument("--flash-dir", help="modules/polices complets de la version (archive XeBuilds, ex. firmware/xebuild-6770)")
    ap.add_argument("--fonts-dir", help="dossier de polices XenonCLatin.xtt/XenonJKLatin.xtt (substitution quand la mise à "
                    "jour n'en fournit que des correctifs .xttp, ex. NXE 13604 : celles de 6770, firmware/xebuild-6770)")
    ap.add_argument("--shrdres", help="shrdres.xzp déjà converti pour cette version")
    ap.add_argument("--shrdres-xam", default=None,
                    help="xam.xex d'une mise à jour complète d'où tirer shrdres (défaut : firmware/17559/$flash_xam.xex)")
    ap.add_argument("--stages", help=f"liste d'étapes séparées par des virgules ({','.join(STAGES)}) ; défaut : toutes")
    ap.add_argument("--from-stage", choices=STAGES, help="reprendre à cette étape")
    ap.add_argument("--no-smoke", action="store_true", help="ne pas faire le test de fumée")
    ap.add_argument("--refresh-template", action="store_true", help="réécrire les fichiers du gabarit déjà présents")
    ap.add_argument("--compare-with", help="projet de référence à comparer (tables de fonctions, overrides)")
    ap.add_argument("--reference-png", help="capture de référence à comparer au test de fumée")
    ap.add_argument("--max-diff", type=float, default=4.0, help="écart moyen toléré avec --reference-png, sur 255 (défaut 4)")
    ap.add_argument("--sdk-prefix", help="préfixe d'installation d'un autre SDK (défaut : sdk/) ; la compilation se fait alors dans "
                                         "out/build/linux-amd64-relwithdebinfo-<nom du préfixe>, sans toucher à la compilation habituelle")
    ap.add_argument("--smoke-seconds", type=int, default=40)
    ap.add_argument("--smoke-name", default="smoke")
    ap.add_argument("--max-iter", type=int, default=12, help="itérations maximales de la boucle codegen")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    if a.sdk_prefix:
        global SDK, REXGLUE, BUILD_DIR_NAME
        SDK = os.path.abspath(a.sdk_prefix)
        REXGLUE = os.path.join(SDK, "bin", "rexglue")
        if os.path.realpath(SDK) != os.path.realpath(os.path.join(ROOT, "sdk")):
            BUILD_DIR_NAME = f"{PRESET}-{os.path.basename(SDK.rstrip('/'))}"

    for tool, hint in ((REXGLUE, "SDK installé (cmake --build ... --target install)"),
                       (XEXINFO, "tools/xexinfo (voir README)")):
        if not os.path.exists(tool):
            sys.exit(f"outil manquant : {tool} -- {hint}")
    if a.shrdres_xam is None:
        d = os.path.join(ROOT, "firmware", "17559", "$flash_xam.xex")
        a.shrdres_xam = d if os.path.exists(d) else None
    dest = os.path.abspath(a.dest) if a.dest else os.path.join(ROOT, "dashboards", a.version)
    work = os.path.abspath(a.work) if a.work else os.path.join(ROOT, "work", "port", a.version)
    os.makedirs(work, exist_ok=True)
    if a.stages:
        todo = a.stages.split(",")
        bad = [s for s in todo if s not in STAGES]
        if bad:
            sys.exit(f"étape inconnue : {bad}")
    else:
        todo = [s for s in STAGES if s != "compare"]
        if a.compare_with:
            todo.append("compare")
        if a.from_stage:
            todo = [s for s in todo if STAGES.index(s) >= STAGES.index(a.from_stage)]
    if a.no_smoke and "smoke" in todo:
        todo.remove("smoke")
    ctx = {"dest": dest, "work": work}
    fn = {"extract": stage_extract, "assets": stage_assets, "pe": stage_pe, "scaffold": stage_scaffold,
          "codegen": stage_codegen, "build": stage_build, "smoke": stage_smoke, "compare": stage_compare}
    log(f"version {a.version} -> {os.path.relpath(dest, ROOT)} (travail : {os.path.relpath(work, ROOT)})")
    log("étapes : " + ", ".join(todo))
    for s in todo:
        log(f"=== {s} ===")
        try:
            fn[s](a, ctx)
        except StageError as e:
            print(f"[port] ÉCHEC à l'étape {s} : {e}", file=sys.stderr)
            sys.exit(2)
    log("terminé")


if __name__ == "__main__":
    main()
