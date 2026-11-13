#!/usr/bin/env python3
"""Remplace les REX_UNIMPLEMENTED du code généré de XAM par leur équivalent C++.

Le recompilateur du SDK (fork third_party/rexglue-sdk, version de la révision courante) n'émet pas trois
instructions présentes dans XAM 13604 : `crmove` et `crnot` (manipulations de bits du registre de condition,
dash_recomp.2.cpp) et `vavguw` (moyenne non signée de mots, dash_recomp.54.cpp). Le code généré lève alors
`std::runtime_error`. Ce script réécrit ces lignes en C++ direct, à partir de la ligne de commentaire qui précède
(le mnémonique et les opérandes émis par le SDK). Il ne touche à aucune autre ligne et peut être relancé.

Usage : tools/xam_unimpl_patch.py <dossier generated/xam>
Sortie : nombre de remplacements, puis les REX_UNIMPLEMENTED laissés tels quels (inconnus).
"""

import re
import sys
from pathlib import Path

BIT_NAMES = ["lt", "gt", "eq", "so"]  # bits 0..3 d'un champ CR
UNIMPL = re.compile(r'^(\s*)REX_UNIMPLEMENTED\((0x[0-9A-Fa-f]+), "([a-z0-9_]+)"\);\s*$')
CR_BIT = re.compile(r"^4\*cr(\d)\+(lt|gt|eq|so)$")
NAMED_BIT = re.compile(r"^(lt|gt|eq|so)$")
VEC = re.compile(r"^v(\d+)$")


def translate(name, operands):
    """Retourne le C++ équivalent, ou None si la forme n'est pas gérée."""
    ops = [o.strip() for o in operands.split(",")] if operands else []
    if name == "crmove" and len(ops) == 2:
        dst, src = CR_BIT.match(ops[0]), CR_BIT.match(ops[1])
        if dst and src:
            return f"ctx.cr{dst.group(1)}.{dst.group(2)} = ctx.cr{src.group(1)}.{src.group(2)};"
    if name == "crnot" and len(ops) == 2 and ops[0] == ops[1]:
        m = NAMED_BIT.match(ops[0])
        if m:  # bit sans champ explicite : champ CR0
            return f"ctx.cr0.{m.group(1)} = !ctx.cr0.{m.group(1)};"
        m = CR_BIT.match(ops[0])
        if m:
            return f"ctx.cr{m.group(1)}.{m.group(2)} = !ctx.cr{m.group(1)}.{m.group(2)};"
    if name == "vavguw" and len(ops) == 3:
        d, a, b = (VEC.match(o) for o in ops)
        if d and a and b:
            D, A, B = d.group(1), a.group(1), b.group(1)
            return (
                f"for (int i = 0; i < 4; ++i) ctx.v{D}.u32[i] = "
                f"uint32_t((uint64_t(ctx.v{A}.u32[i]) + ctx.v{B}.u32[i] + 1) >> 1);"
            )
    return None


def patch_file(path):
    lines = path.read_text(encoding="utf-8").split("\n")
    done, left = 0, []
    for i, line in enumerate(lines):
        m = UNIMPL.match(line)
        if not m or i == 0:
            continue
        indent, _addr, name = m.group(1), m.group(2), m.group(3)
        # Commentaire du SDK juste au-dessus : « // <mnémonique> <opérandes> » (une ligne « // UNIMPLEMENTED: »
        # peut s'intercaler entre les deux).
        c = None
        for j in (i - 1, i - 2):
            if j >= 0:
                c = re.match(rf"^\s*//\s*{name}\s+(.*)$", lines[j]) or c
        if not c:
            left.append((path.name, name, "commentaire absent"))
            continue
        operands = c.group(1).strip()
        code = translate(name, operands)
        if code is None:
            left.append((path.name, name, operands))
            continue
        lines[i] = f"{indent}{code}  // {name} {operands} (remplace REX_UNIMPLEMENTED)"
        done += 1
    if done:
        path.write_text("\n".join(lines), encoding="utf-8")
    return done, left


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = Path(sys.argv[1])
    total, leftover = 0, []
    for path in sorted(root.glob("*.cpp")):
        if "REX_UNIMPLEMENTED" not in path.read_text(encoding="utf-8", errors="replace"):
            continue
        done, left = patch_file(path)
        total += done
        leftover += left
    print(f"remplacements : {total}")
    for fname, name, operands in leftover:
        print(f"non traité : {fname} {name} {operands}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
