#!/usr/bin/env python3
"""Boucle de codegen pour un module seul (xam.xex...) : équivalent de l'étape « codegen » de port_dashboard.py,
sans le gabarit Dashboard (P4).
usage: codegen_module.py <projet> <manifeste.toml> <nom> <image.pe> <base_hex> <.text début_hex> <.text taille_hex> <plages data début:fin ...>
Écrit <projet>/<nom>_auto_overrides.toml (cibles non résolues + fonctions par pointeur)."""
import os, re, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
proj, manifest, name, pe, base = sys.argv[1:6]
ts, tsz = int(sys.argv[6], 16), int(sys.argv[7], 16)
ranges = sys.argv[8:]
ov = os.path.join(proj, f"{name}_auto_overrides.toml")
gen = os.path.join(proj, "generated", name)
A = r"0x([0-9A-Fa-f]{8})"
unres, ptr = {}, {}
def write():
    with open(ov, "w") as f:
        f.write(f"# Corrections AUTOMATIQUES ({name}), écrites par tools/codegen_module.py.\n[functions]\n")
        for a, s in sorted(unres.items()): f.write(f"0x{a:08X} = {{}}  # branche depuis 0x{s:08X}\n")
        for a, s in sorted(ptr.items()): f.write(f"0x{a:08X} = {{}}  # ref 0x{s:08X}\n")
write()
for it in range(1, 40):
    p = subprocess.run([os.path.join(ROOT, "sdk/bin/rexglue"), "codegen", manifest], cwd=proj,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
    out = p.stdout
    print(f"codegen #{it}: rc={p.returncode}", flush=True)
    if p.returncode:
        new = {}
        for m in re.finditer(rf"^\s+{A} from {A}: b ", out, re.M):
            t, s = int(m.group(1), 16), int(m.group(2), 16)
            if t not in unres: new.setdefault(t, s)
        if not new: print(out[-2000:]); sys.exit(1)
        unres.update(new); write(); continue
    cand = os.path.join(proj, "ptr_candidates.toml")
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools/find_ptr_funcs.py"), pe, base,
                        os.path.join(gen, f"{name}_init.cpp"), f"{ts:X}", f"{ts+tsz:X}"] + ranges,
                       stdout=subprocess.PIPE, text=True, check=True)
    open(cand, "w").write(r.stdout)
    k = subprocess.run([sys.executable, os.path.join(ROOT, "tools/filter_inside.py"), gen, cand],
                       stdout=subprocess.PIPE, text=True, check=True).stdout
    new = {}
    for line in k.splitlines():
        m = re.match(r"0x([0-9A-F]{8}) = \{\}\s+# ref 0x([0-9A-F]{8})", line)
        if m:
            a_ = int(m.group(1), 16)
            if a_ not in unres and a_ not in ptr: new[a_] = int(m.group(2), 16)
    print(f"  par pointeur: {len(new)} nouvelles", flush=True)
    resid = [l for l in out.splitlines() if l.startswith("Unresolved")]
    if not new:
        print(f"terminé: {len(unres)} non résolues, {len(ptr)} par pointeur; résiduels: {len(resid)}")
        print("\n".join(resid)); sys.exit(0)
    ptr.update(new); write()
sys.exit(2)
