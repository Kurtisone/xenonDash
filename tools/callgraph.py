"""Imports (__imp__*) atteignables depuis une fonction du code généré, en largeur.
usage: callgraph.py <generated_dir> <sub_XXXXXXXX> [profondeur]"""
import glob, re, sys, collections
calls = collections.defaultdict(set)
for f in glob.glob(sys.argv[1] + '/*_recomp.*.cpp'):
    cur = None
    for line in open(f):
        m = re.match(r'DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)', line)
        if m: cur = m.group(1); continue
        if cur:
            for c in re.findall(r'\b((?:__imp__)?\w+)\(ctx, base\)', line):
                calls[cur].add(c.replace('__imp__sub_', 'sub_') if c.startswith('__imp__sub_') else c)
start, depth = sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 4
seen, frontier = {start: 0}, [start]
for d in range(depth):
    nxt = []
    for fn in frontier:
        for c in calls.get(fn, ()):
            if c not in seen:
                seen[c] = d + 1; nxt.append(c)
    frontier = nxt
for name, d in sorted(seen.items(), key=lambda x: (x[1], x[0])):
    if name.startswith('__imp__'): print(d, name)
