#!/usr/bin/env python3
"""Essai XenosRecomp : traduit (conteneur -> HLSL) puis compile (HLSL -> SPIR-V via dxc) chaque .bin d'un répertoire.
usage: xenos_batch.py <répertoire de .bin (récursif)> <répertoire de sortie> [racine XenosRecomp]
Un sous-processus par shader (XenosRecomp plante ou s'arrête sur assert/entrée inattendue) ; résultats dans
<sortie>/résultats.tsv : nom, type, étape en échec (ok|recomp|dxc), cause (première ligne d'erreur).
Aucune sortie n'est destinée à être versionnée (dérivés de microcode Microsoft)."""
import sys, os, subprocess, glob, collections
src, out = sys.argv[1:3]
root = sys.argv[3] if len(sys.argv) > 3 else 'work/XenosRecomp'
X = f'{root}/build/XenosRecomp/XenosRecomp'
DXC = f'{root}/thirdparty/dxc-bin/bin/x64/dxc-linux'
INC = f'{root}/XenosRecomp/shader_common.h'
env = dict(os.environ, LD_LIBRARY_PATH=f'{root}/thirdparty/dxc-bin/lib/x64')
os.makedirs(out, exist_ok=True)
rows = []
for f in sorted(glob.glob(src + '/**/*.bin', recursive=True)):
    fam = os.path.basename(os.path.dirname(f))
    name = os.path.basename(f)[:-4]
    kind = name.split('_')[1]
    h, s = f'{out}/{fam}_{name}.hlsl', f'{out}/{fam}_{name}.spv'
    for p in (h, s):
        if os.path.exists(p): os.remove(p)
    stage, cause = 'ok', ''
    try:
        r = subprocess.run([X, f, h, INC], capture_output=True, text=True, timeout=60)
        if r.returncode != 0 or not os.path.exists(h):
            stage, cause = 'recomp', (r.stderr.strip().splitlines() or [f'code {r.returncode}'])[-1][:200]
    except subprocess.TimeoutExpired:
        stage, cause = 'recomp', 'timeout'
    if stage == 'ok':
        t = 'ps_6_0' if kind == 'PS' else 'vs_6_0'
        args = [DXC, '-T', t, '-HV', '2021', '-all-resources-bound', '-spirv', '-fvk-use-dx-layout', '-Qstrip_debug']
        if kind == 'VS': args.append('-fvk-invert-y')
        r = subprocess.run(args + [h, '-Fo', s], capture_output=True, text=True, env=env, timeout=300)
        if r.returncode != 0 or not os.path.exists(s):
            errs = [l for l in r.stderr.splitlines() if 'error' in l]
            stage, cause = 'dxc', (errs[0] if errs else r.stderr.strip()[:200])[:200]
    rows.append((fam, name, kind, stage, cause))
with open(f'{out}/résultats.tsv', 'w') as o:
    for r in rows: o.write('\t'.join(r) + '\n')
c = collections.Counter((r[0], r[2], r[3]) for r in rows)
for k, v in sorted(c.items()): print(k, v)
