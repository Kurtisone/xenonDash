"""Trouve les fonctions atteintes seulement par pointeur (vtables, callbacks) que l'analyse a ratées.
usage: find_ptr_funcs.py <image.pe> <base_hex> <dash_init.cpp> <text_start> <text_end> <data ranges start:end ...>"""
import sys, struct, re
d = open(sys.argv[1],'rb').read(); base = int(sys.argv[2],16)
known = set(int(m,16) for m in re.findall(r'\{ 0x([0-9A-Fa-f]{8}), \w+ \}', open(sys.argv[3]).read()))
ts, te = int(sys.argv[4],16), int(sys.argv[5],16)
def w(a): return struct.unpack('>I', d[a-base:a-base+4])[0]
def is_term(i):
    op = i >> 26
    return i in (0x4E800020, 0x4E800420, 0) or (op == 18 and not i & 1)  # blr, bctr, padding, b (sans lien)
cands = {}
for rng in sys.argv[6:]:
    s, e = (int(x,16) for x in rng.split(':'))
    for a in range(s, e, 4):
        v = w(a)
        if ts <= v < te and v % 4 == 0 and v not in known and is_term(w(v-4)) and w(v) != 0:
            cands.setdefault(v, a)
# Adresses de fonctions construites dans le code : lis rX,hi ; ... ; addi/ori rY,rX,lo
for a in range(ts, te, 4):
    i = w(a); op = i >> 26
    if op not in (14, 24): continue
    if op == 14: rd, ra, imm = (i >> 21) & 31, (i >> 16) & 31, i & 0xFFFF; imm = imm - 0x10000 if imm & 0x8000 else imm
    else: ra, rd, imm = (i >> 21) & 31, (i >> 16) & 31, i & 0xFFFF  # ori rA,rS,UIMM
    if ra == 0: continue
    for back in range(1, 64):
        j = w(a - 4*back)
        if j >> 26 == 15 and (j >> 16) & 31 == 0 and (j >> 21) & 31 == ra:
            hi = j & 0xFFFF
            v = ((hi << 16) + imm) & 0xFFFFFFFF
            if ts <= v < te and v % 4 == 0 and v not in known and is_term(w(v-4)) and w(v) != 0:
                cands.setdefault(v, a)
            break
        # registre réécrit entre-temps : on abandonne
        if (j >> 21) & 31 == ra and (j >> 26) in (14, 15, 24, 31, 32): break
print(f'# fonctions connues: {len(known)}, candidates: {len(cands)}', file=sys.stderr)
for v in sorted(cands): print(f'0x{v:08X} = {{}}  # ref 0x{cands[v]:08X}')
