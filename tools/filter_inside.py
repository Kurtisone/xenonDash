"""Écarte les candidates situées à l'intérieur d'une fonction déjà générée.
usage: filter_inside.py <generated_dir> <candidates.toml>"""
import sys, re, glob, bisect
ext = {}
for f in glob.glob(sys.argv[1] + '/*_recomp.*.cpp'):
    cur = None
    for line in open(f):
        m = re.match(r'(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?sub_([0-9A-F]{8})\)', line)
        if m:
            cur = int(m.group(1), 16); ext.setdefault(cur, cur); continue
        if cur is not None:
            for a in re.findall(r'loc_([0-9A-F]{8})', line):
                ext[cur] = max(ext[cur], int(a, 16))
starts = sorted(ext)
kept = rej = 0
for line in open(sys.argv[2]):
    m = re.match(r'0x([0-9A-F]{8})', line)
    if not m: continue
    x = int(m.group(1), 16)
    i = bisect.bisect_left(starts, x) - 1
    if i >= 0 and ext[starts[i]] >= x:
        rej += 1; print(f'# rejet (dans sub_{starts[i]:08X}) ' + line.rstrip(), file=sys.stderr); continue
    kept += 1; print(line.rstrip())
print(f'# gardées {kept}, rejetées {rej}', file=sys.stderr)
