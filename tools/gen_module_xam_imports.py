#!/usr/bin/env python3
"""Produit le fichier des imports XAM d'un module recompilé (palier G, docs/xam_recompile.md).

usage: gen_module_xam_imports.py <generated/module> <sortie.cpp>

Lit le code généré du module (`__imp__<Nom>`), garde les noms exportés par `xam.xex` (table d'exports du SDK, ordinaux
de la console) et écrit, pour chacun, un `__imp__<Nom>` à visibilité masquée qui appelle
dash::xamredir::CallModuleImport (common/src/dash_xam_redirect.cpp) : l'appel atteint l'export de même ordinal de XAM
recompilé au lieu du HLE du SDK. Les imports `xboxkrnl` restent au noyau du SDK."""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gen, out = sys.argv[1:3]


def table(lib):
    d = {}
    path = os.path.join(ROOT, f"third_party/rexglue-sdk/src/kernel/{lib}/export_table.inc")
    for m in re.finditer(r"XE_EXPORT\(\s*(\w+),\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\)", open(path).read()):
        d[m.group(3)] = (int(m.group(2), 16), m.group(4))
    return d


xam = table("xam")
names = set()
for f in sorted(os.listdir(gen)):
    if f.endswith((".cpp", ".h")):
        names.update(re.findall(r"__imp__(\w+)", open(os.path.join(gen, f), errors="replace").read()))
sel = sorted(n for n in names if n in xam and xam[n][1] == "kFunction")
HEAD = r"""// Imports `xam.xex` de @MOD@.xex recompilé : routés vers XAM recompilé (palier G).
// FICHIER PRODUIT par tools/gen_module_xam_imports.py à partir du code généré (@N@ imports) : ne pas éditer.
//
// Chaque `__imp__<Nom>` est masqué : dans la DLL du module, l'appel se lie à cette définition (pas à celle de
// l'exécutable ni du SDK) et passe par dash::xamredir::CallModuleImport, qui exécute l'export de XAM recompilé de même
// ordinal.

#include <cstdint>

#include <rex/ppc/context.h>

#include "dash_xam_redirect.h"

#define MODULE_XAM_IMPORT(name, ordinal)                                                       \
  extern "C" __attribute__((visibility("hidden"))) void __imp__##name(PPCContext& ctx, uint8_t* base) { \
    static dash::xamredir::ModuleImport imp{#name, ordinal};                                    \
    dash::xamredir::CallModuleImport(imp, ctx, base);                                          \
  }

"""
with open(out, "w") as f:
    f.write(HEAD.replace("@MOD@", os.path.basename(os.path.normpath(gen))).replace("@N@", str(len(sel))))
    for n in sel:
        f.write(f"MODULE_XAM_IMPORT({n}, {xam[n][0]})\n")
print(f"{len(sel)} imports xam.xex, {len(names) - len(sel)} autres ({out})")
