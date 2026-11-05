"""Compare les imports d'un XEX à ce que le runtime ReXGlue implémente."""
import sys, re, os, collections
kdir, imports = sys.argv[1], sys.argv[2]
names = {}
for lib, f in (('xboxkrnl.exe','xboxkrnl/export_table.inc'),('xam.xex','xam/export_table.inc')):
    for m in re.finditer(r'XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+)', open(os.path.join(kdir,f)).read()):
        names[(lib,int(m.group(1),16))] = m.group(2)
impl, stub = set(), set()
for root,_,files in os.walk(kdir):
    for fn in files:
        if not fn.endswith('.cpp'): continue
        t = open(os.path.join(root,fn), errors='ignore').read()
        for m in re.finditer(r'REX_EXPORT(_STUB\w*)?\(\s*__imp__(\w+)', t):
            (stub if m.group(1) else impl).add(m.group(2))
stats = collections.Counter(); rows = []
for line in open(imports):
    lib, o = line.split('\t'); o = int(o)
    n = names.get((lib,o), f'ord_{o:#x}')
    st = 'IMPL' if n in impl else 'STUB' if n in stub else 'MISSING'
    stats[(lib,st)] += 1; rows.append((lib,st,n))
for r in sorted(rows): print(*r, sep='\t')
print('#', dict(stats), file=sys.stderr)
