#!/usr/bin/env python3
"""Régénère le code de tous les modules de dashboards/2.0.13604.0-xam (XAM, hud, modules XUI du HUD), palier N3
(docs/xam_recompile.md).

Pourquoi pas un seul `rexglue codegen --ignore-stamp` : le codegen charge TOUS les XEX du manifeste en mémoire avant
d'analyser, et signin.xex, createprofile.xex et gamerprofile.xex ont la même base fixe 0x90100000 : le dernier chargé écrase
les autres (code généré faux). Le codegen du SDK (fork) refuse donc un recouvrement dès qu'un des deux modules est à générer.
Les modules sont générés par groupes sans base commune (`--target`) : {xam, hud, vk, deviceselector, ximecore, signin},
puis createprofile, puis gamerprofile ; une exécution complète finale (tous les modules à jour, donc ignorés) écrit les
fichiers de projet des huit modules.
Ensuite : correctif des instructions de XAM (tools/xam_unimpl_patch.py) et fichiers d'imports `xam.xex` des modules
(tools/gen_module_xam_imports.py).

usage : tools/regen_xam_modules.py      (SDK installé dans sdk/ ; ~2 min)"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
P = os.path.join(ROOT, "dashboards/2.0.13604.0-xam")
RG = os.path.join(ROOT, "sdk/bin/rexglue")
XUI = ["vk", "signin", "createprofile", "gamerprofile", "deviceselector", "ximecore"]
GROUPS = [["xam", "hud", "vk", "deviceselector", "ximecore", "signin"], ["createprofile"], ["gamerprofile"]]
GEN = os.path.join(P, "generated/dash")
# Chemin ABSOLU du manifeste, comme la construction CMake : l'empreinte des modules en dépend (sinon la construction les juge périmés).
MANIFEST = os.path.join(P, "dash_manifest.toml")


def run(*cmd):
    r = subprocess.run(cmd, cwd=P, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    if r.returncode:
        sys.exit(f"échec : {' '.join(cmd)}\n{r.stdout[-3000:]}")


for group in GROUPS:
    run(RG, "codegen", "--ignore-stamp", MANIFEST, "--target", *group)
    print("groupe", group, "ok", flush=True)
# Exécution complète : tous les modules sont à jour (ignorés), elle réécrit module_registry.cpp et dll_targets.cmake pour
# les huit modules. C'est aussi ce que fait la construction CMake (étape « Generating recompiled code »).
run(RG, "codegen", MANIFEST)
print("exécution complète ok", flush=True)

subprocess.run([sys.executable, os.path.join(ROOT, "tools/xam_unimpl_patch.py"), os.path.join(P, "generated/xam")], check=True)
for m in ["hud"] + XUI:
    out = os.path.join(P, "src_hud/hud_xam_imports.cpp" if m == "hud" else f"src_xui/{m}_xam_imports.cpp")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    subprocess.run([sys.executable, os.path.join(ROOT, "tools/gen_module_xam_imports.py"), os.path.join(GEN, m), out], check=True)
