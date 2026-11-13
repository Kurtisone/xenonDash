#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boucle automatique de codegen d'un XEX quelconque (reprise de la logique de tools/port_dashboard.py, P2).

`rexglue codegen` echoue (ou avertit) quand l'analyse rate des fonctions. Cette boucle les declare une a une dans un fichier
d'overrides genere jusqu'a un codegen propre :
  1. cibles de branchements non resolues (« 0x... from 0x...: b ») -> fonctions ;
  2. fonctions atteintes seulement par pointeur (vtables, callbacks, adresses construites par lis/addi) : tools/find_ptr_funcs.py,
     puis tools/filter_inside.py ecarte celles situees dans une fonction deja generee (continuations de catch, tables SEH) ;
  3. continuations de funclets (« Unresolved b target T from S » avec T dans une fonction generee) -> fonction jusqu'au 1er blr/b ;
  4. setjmp/longjmp du CRT : adresses donnees par le matcher de signatures (d3d_match.py), ecrites en tete du fichier.
Avant la 1re passe, tools/func_discover.py declare les fonctions feuilles atteintes seulement par pointeur (donnees, lis/addi) dont le
debut est plausible ; si le codegen echoue sans cible exploitable, ces candidats sont retires par paliers (data-in, code, puis tous) et
la passe est refaite : un faux positif ne peut pas bloquer la boucle. L'etat de la boucle est memorise (state_path) : une relance
apres l'ajout d'une fonction decouverte a l'execution ne coute qu'une passe (rexglue ne reecrit que les fichiers qui changent).
Les imports sans thunk et les ordinaux inconnus sont traites par le SDK (patch unified) et par les stubs generes a la compilation
(`write_auto_stubs`). Aucune dependance a port_dashboard.py (ses fonctions sont couplees a son contexte) : ce module est le
pendant generique, a factoriser dans l'autre sens plus tard.
"""
import json
import os
import re
import struct
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ADDR = r"0x([0-9A-Fa-f]{8})"


class CodegenError(Exception):
    pass


def write_overrides(path, unresolved, ptr, cont, setjmp=None, longjmp=None, extra=None, rexcrt=None):
    lines = ["# Corrections AUTOMATIQUES de l'analyse (tools/codegen_loop.py), regenerees a chaque construction.", ""]
    if setjmp:
        lines.append(f"setjmp_address = 0x{setjmp:08X}")
    if longjmp:
        lines.append(f"longjmp_address = 0x{longjmp:08X}")
    if rexcrt:   # fonctions du jeu remplacees par celles du SDK (fibres XAPI... : detectees par d3d_match.py)
        lines += ["", "[rexcrt]"] + [f"{k} = 0x{a:08X}" for k, a in sorted(rexcrt.items(), key=lambda kv: kv[1])]
    # une adresse ne figure qu'une fois (TOML) : continuation > cible non resolue > pointeur
    extra = extra or {}
    cont = {a: v for a, v in cont.items() if a not in extra}
    unresolved = {a: v for a, v in unresolved.items() if a not in cont and a not in extra}
    ptr = {a: v for a, v in ptr.items() if a not in extra}
    ptr = {a: v for a, v in ptr.items() if a not in cont and a not in unresolved}
    lines += ["", "[functions]", "# cibles de branchements que l'analyse n'a rattachees a aucune fonction"]
    for a, src in sorted(unresolved.items()):
        lines.append(f"0x{a:08X} = {{}}  # branche depuis 0x{src:08X}")
    lines.append("# fonctions atteintes par pointeur ou dont l'adresse est construite par lis/addi")
    for a, ref in sorted(ptr.items()):
        lines.append(f"0x{a:08X} = {{}}  # ref 0x{ref:08X}")
    lines.append("# fonctions declarees par l'utilisateur ou decouvertes a l'execution (missing_functions.toml, bootstrap)")
    for a, raw in sorted(extra.items()):
        lines.append(f"0x{a:08X} = {raw}")
    lines.append("# continuations de funclets (finally/catch)")
    for a, (end, src) in sorted(cont.items()):
        lines.append(f"0x{a:08X} = {{ end = 0x{end:08X} }}  # continuation depuis 0x{src:08X}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def block_end(data, base, start):
    a = start
    for _ in range(512):
        off = a - base
        w = struct.unpack(">I", data[off:off + 4])[0]
        a += 4
        if w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and not (w & 1)):
            return a
    return a


def run_loop(rexglue, sdk_lib, proj, manifest, gen_dir, init_name, image, overrides, log_dir, setjmp=None, longjmp=None,
             max_iter=14, log=print, tick=None, extra=None, seeds=None, rexcrt=None, state_path=None, discover=True):
    """image : tools/d3d_match.Image. Renvoie un dict de statistiques ; leve CodegenError si pas de convergence."""
    import func_discover
    env = dict(os.environ, LD_LIBRARY_PATH=sdk_lib)
    extra = dict(extra or {})
    rexcrt = dict(rexcrt or {})
    extra = {a: v for a, v in extra.items() if a not in rexcrt.values()}
    unresolved, ptr, cont = {}, dict(seeds or {}), {}     # seeds : fonctions imposees (crochets) declarees des la 1re passe
    static, dropped = {}, []
    resumed = False
    if state_path and os.path.isfile(state_path):         # reprise : etat d'une boucle precedente sur la meme image
        try:
            st = json.load(open(state_path))
            unresolved = {int(k): v for k, v in st["unresolved"].items()}
            ptr.update({int(k): v for k, v in st["ptr"].items()})
            cont = {int(k): tuple(v) for k, v in st["cont"].items()}
            static = {int(k): v for k, v in st["static"].items()}
            dropped = st.get("dropped", [])
            resumed = True
            log(f"  reprise de la boucle : {len(unresolved)} cibles, {len(ptr)} par pointeur, {len(cont)} continuations")
        except (ValueError, KeyError):
            pass
    if discover and not resumed:
        taken = set(extra) | set(rexcrt.values()) | {a for a in (setjmp, longjmp) if a}
        kinds = set((os.environ.get("XD_DISCOVER_KINDS") or "data,code,data-in").split(","))     # essais : restreindre les natures
        found = {a: v for a, v in func_discover.discover(image).items() if a not in ptr and a not in taken and v[0] in kinds}
        static = {a: k for a, (k, _) in found.items()}
        ptr.update({a: r for a, (_, r) in found.items()})
        log(f"  decouverte statique : {len(found)} fonctions hors .pdata declarees ("
            + ", ".join(f"{k} {sum(1 for x in static.values() if x == k)}" for k in sorted(set(static.values()))) + ")")

    def save_state():
        if state_path:
            json.dump({"unresolved": unresolved, "ptr": ptr, "cont": cont, "static": static, "dropped": dropped},
                      open(state_path, "w"))

    def write():
        write_overrides(overrides, unresolved, ptr, cont, setjmp, longjmp, extra, rexcrt)
    write()
    secs = image.sections
    ts, tsz = secs[".text"]
    ts += image.base
    ranges = []
    for nm in (".rdata", ".data"):
        if nm in secs:
            s, sz = secs[nm]
            ranges.append(f"{s + image.base:X}:{s + image.base + sz:X}")
    final_warn = ""
    for it in range(1, max_iter + 1):
        lp = os.path.join(log_dir, f"codegen{it}.log")
        t0 = time.time()
        p = subprocess.run([rexglue, "--force", "codegen", manifest], cwd=proj, env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, errors="replace")
        out = p.stdout
        open(lp, "w").write(out)
        log(f"codegen #{it} : code {p.returncode} ({time.time() - t0:.0f} s)")
        if tick:
            tick(it, max_iter)
        if p.returncode != 0:
            new = {}
            for m in re.finditer(rf"^\s+{ADDR} from {ADDR}: b ", out, re.M):
                t, s = int(m.group(1), 16), int(m.group(2), 16)
                if t not in unresolved and t not in cont:
                    new.setdefault(t, s)
            if not new:
                # rejet automatique des candidats statiques : d'abord ceux que l'erreur nomme, puis par paliers de confiance
                named = {int(x, 16) for x in re.findall(ADDR, out)}
                drop = [a for a in static if a in named]
                if not drop:
                    for kind in ("data-in", "code", "data"):
                        drop = [a for a, k in static.items() if k == kind]
                        if drop:
                            break
                if not drop:
                    raise CodegenError("codegen en echec sans cible non resolue exploitable :\n" + "\n".join(out.splitlines()[-20:]))
                for a in drop:
                    static.pop(a)
                    ptr.pop(a, None)
                dropped += drop
                log(f"  codegen en echec : {len(drop)} candidat(s) statique(s) retire(s), nouvelle passe")
                write()
                continue
            unresolved.update(new)
            log(f"  {len(new)} cible(s) non resolue(s) ajoutee(s)")
            write()
            continue
        final_warn = "\n".join(l for l in out.splitlines() if l.startswith("Unresolved"))
        init = os.path.join(gen_dir, init_name)
        cand = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "find_ptr_funcs.py"), image.path, f"{image.base:X}", init,
                               f"{ts:X}", f"{ts + tsz:X}"] + ranges, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout
        cpath = os.path.join(log_dir, "ptr_candidates.toml")
        open(cpath, "w").write(cand)
        kept = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, "filter_inside.py"), gen_dir, cpath],
                              stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True).stdout
        new = {}
        for line in kept.splitlines():
            m = re.match(r"0x([0-9A-F]{8}) = \{\}\s+# ref 0x([0-9A-F]{8})", line)
            if m and int(m.group(1), 16) not in unresolved and int(m.group(1), 16) not in ptr \
                    and int(m.group(1), 16) not in cont:
                new[int(m.group(1), 16)] = int(m.group(2), 16)
        # candidats dont la reference est un POINTEUR DE DONNEES (vtable, table de rappels) : fonctions meme situes dans une fonction
        # generee (blocs partages, queues communes) ; mesure sur Skate : 26 vrais pour 2 faux. Les refs lis/addi du code restent filtrees
        # (1045 faux pour 70 vrais).
        text_lo, text_hi = ts, ts + tsz
        for line in cand.splitlines():
            m = re.match(r"0x([0-9A-F]{8}) = \{\}\s+# ref 0x([0-9A-F]{8})", line)
            if m:
                a_, r_ = int(m.group(1), 16), int(m.group(2), 16)
                if not (text_lo <= r_ < text_hi) and a_ not in unresolved and a_ not in ptr and a_ not in cont and a_ not in extra:
                    new.setdefault(a_, r_)
        newc = {}
        for m in re.finditer(rf"^Unresolved b target {ADDR} from {ADDR}", final_warn, re.M):
            t, s = int(m.group(1), 16), int(m.group(2), 16)
            if t not in unresolved and t not in ptr and t not in cont:
                newc[t] = (block_end(image.data, image.base, t), s)
        log(f"  fonctions par pointeur : {len(new)} nouvelles ; continuations : {len(newc)}")
        if not new and not newc:
            n_fn = len(re.findall(r"\{ 0x[0-9A-F]{8}, sub_", open(init).read()))
            save_state()
            return {"iterations": it, "functions": n_fn, "unresolved": len(unresolved), "pointer": len(ptr),
                    "continuations": len(cont), "static": len(static), "static_dropped": len(dropped),
                    "resumed": resumed, "residual_warnings": final_warn.splitlines()[:20]}
        ptr.update(new)
        cont.update(newc)
        write()
    raise CodegenError(f"codegen : pas de convergence en {max_iter} iterations")


def write_auto_stubs(path, symbols):
    """Stubs retournant 0 pour les imports du XEX sans implementation (thunks « sub_XXXXXXXX » = ordinaux inconnus)."""
    with open(path, "w") as f:
        f.write("// Genere par tools/codegen_loop.py : imports du XEX sans implementation dans le SDK (ordinaux inconnus).\n"
                "// Stubs retournant 0 ; a remplacer par une vraie implementation si le jeu les utilise.\n"
                "#include <rex/hook.h>\n#include <rex/types.h>\n\n")
        for s in symbols:
            f.write(f"REX_STUB_RETURN({s}, 0)\n")
