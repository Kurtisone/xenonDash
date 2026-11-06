"""Analyse statique des fonctions D3D/Vd liées dans dash.pe (étude de faisabilité rendu natif).
usage: analyse_d3d.py <dash.pe> <generated_dir> <cmd> [args]
  strings <regex>        : chaînes de l'image avec adresse et fonctions référentes
  vdcallers              : imports Vd*/Mm* et leurs appelants
  shaders [liste]        : conteneurs de shaders Xenos (0x102A1100/1101) : nombre, VS/PS, tailles
  shadernames            : noms de constantes/échantillonneurs des tables de constantes
  familles               : classement vidéo (décodeur WMV) / UI et liste des shaders UI
  cf                     : flot de contrôle (opcodes CF), VS à export mémoire probable
  pm4 | funcs lo hi | who a.. | api : fonctions D3D, appelants, points d'entrée de la bibliothèque D3D
"""
import sys, re, struct, glob, collections
img, gen, cmd = sys.argv[1:4]
BASE = 0x92000000
D = open(img, 'rb').read()
TEXT = (0x9210C000, 0x9210C000 + 0x630184)

def funcs():
    """début de fonctions d'après .pdata (RUNTIME_FUNCTION: début, longueur/flags)"""
    pd = D[0x920E8000-BASE:0x920E8000-BASE+0x23CB0]
    out = []
    for i in range(0, len(pd), 8):
        a, w = struct.unpack('>II', pd[i:i+8])
        if a: out.append((a, (w >> 8 & 0x3FFFFF) * 4))
    return sorted(out)

def owner(fl, addr):
    import bisect
    starts = [f[0] for f in fl]
    i = bisect.bisect_right(starts, addr) - 1
    return fl[i][0] if i >= 0 else None

def xrefs():
    """lis rX,hi ; addi/ori rX,rX,lo  (fenêtre de 8 insns) -> adresse -> [sites]"""
    t = D[TEXT[0]-BASE:TEXT[1]-BASE]
    ref = collections.defaultdict(list)
    n = len(t)//4
    w = struct.unpack('>%dI' % n, t[:n*4])
    for i, ins in enumerate(w):
        if ins >> 26 == 15 and (ins >> 16 & 0x1F) == 0:  # lis (addis r,0)
            rt, hi = ins >> 21 & 31, ins & 0xFFFF
            for j in range(i+1, min(i+8, n)):
                x = w[j]
                if x >> 26 in (14, 24) and (x >> 16 & 31) == rt:
                    lo = x & 0xFFFF
                    if x >> 26 == 14 and lo & 0x8000: lo -= 0x10000
                    ref[((hi << 16) + lo) & 0xFFFFFFFF].append(TEXT[0] + i*4)
    return ref

if cmd == 'strings':
    rx = re.compile(sys.argv[4].encode(), re.I)
    fl = funcs(); ref = xrefs()
    for m in re.finditer(rb'[\x20-\x7e\n\t]{6,}', D):
        s = m.group()
        if rx.search(s):
            a = BASE + m.start()
            fs = sorted({owner(fl, r) for r in ref.get(a, [])})
            print(f'{a:08X} {s[:90]!r} <- {[hex(f) for f in fs]}')
elif cmd == 'vdcallers':
    callers = collections.defaultdict(set)
    for f in glob.glob(gen + '/*_recomp.*.cpp'):
        cur = None
        for l in open(f):
            m = re.match(r'DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)', l)
            if m: cur = m.group(1); continue
            for c in re.findall(r'\b(__imp__(?:Vd|Mm)\w+)\(ctx, base\)', l): callers[c].add(cur)
    for k, v in sorted(callers.items()): print(k, len(v), sorted(v))

def containers(data, base=0):
    """Conteneurs de shaders Xenos : flags 0x102A1100 (VS) / 0x102A1101 (PS), entête décrit par XenosRecomp."""
    out = []
    for m in re.finditer(rb'\x10\x2a\x11[\x00\x01]', data):
        o = m.start()
        if o + 0x24 > len(data): continue
        fl, vs, ps, fc, ct, dt, so, f1c, f20 = struct.unpack('>9I', data[o:o+0x24])
        if not (0x40 <= vs <= 0x4000 and ps <= 0x10000 and ps % 4 == 0 and ct < vs and dt <= vs and so < vs and o + vs + ps <= len(data)): continue
        ph, sz = struct.unpack('>II', data[o+so:o+so+8])
        # version token vs_3_0 / ps_3_0 dans la table de constantes
        ver = re.search(rb'(vs|ps)_\d_\d', data[o:o+vs])
        out.append(dict(off=o+base, kind='VS' if fl & 1 else 'PS', vsz=vs, psz=ps, ucode_off=ph, ucode_size=sz,
                        ver=ver.group().decode() if ver else '?', data=data[o:o+vs+ps]))
    return out

if cmd == 'shaders':
    import hashlib
    cs = containers(D, BASE)
    # exclure les doublons d'imbrication (conteneur contenu dans un autre)
    print('conteneurs plausibles:', len(cs))
    h = collections.Counter(hashlib.md5(c['data']).hexdigest() for c in cs)
    print('distincts (octets identiques):', len(h))
    print('VS:', sum(c['kind']=='VS' for c in cs), 'PS:', sum(c['kind']=='PS' for c in cs))
    print('versions:', collections.Counter(c['ver'] for c in cs))
    print('taille ucode totale (octets):', sum(c['ucode_size'] for c in cs), ' conteneurs totaux:', sum(len(c['data']) for c in cs))
    sec = collections.Counter()
    for c in cs:
        a = c['off']
        sec['.rdata' if a < 0x920E8000 else '.data' if 0x92797400 <= a < 0x928E9000 else 'embsec' if 0x9273C200<=a<0x92797000 else 'autre'] += 1
    print('par section:', dict(sec))
    if len(sys.argv) > 4:
        for c in cs: print(f"{c['off']:08X} {c['kind']} {c['ver']} total={len(c['data']):#x} ucode={c['ucode_size']:#x}")

if cmd == 'shadernames':
    cs = containers(D, BASE)
    names = collections.Counter()
    for c in cs:
        for n in set(re.findall(rb'[A-Za-z_][A-Za-z0-9_]{2,40}(?=\x00)', c['data'][:c['vsz']])):
            if not re.match(rb'(vs|ps)_', n) and n != b'Microsoft': names[n.decode()] += 1
    for n, k in names.most_common(80): print(k, n)

def cf_scan(c):
    """Décode le flot de contrôle Xenos : (nb exec, nb alloc mémoire, nb boucles, nb instructions ALU/fetch)."""
    u = c['data'][c['vsz'] + c['ucode_off']: c['vsz'] + c['ucode_off'] + c['ucode_size']]
    w = struct.unpack('>%dI' % (len(u)//4), u[:len(u)//4*4])
    cfs, i = [], 0
    min_exec = len(w)//3
    while 3*i < len(w) and 2*i < min_exec*3 + 2:
        a, b, d = w[3*i:3*i+3] if 3*i+3 <= len(w) else (0, 0, 0)
        # mot 48 bits : dword0 = bits 0-31, dword1 bas = bits 32-47 (little-endian logique du matériel après swap)
        f0 = (a & 0xFFFFFFFF) | ((b & 0xFFFF) << 32)
        f1 = (b >> 16) | ((d & 0xFFFFFFFF) << 16)
        for f in (f0, f1):
            op = f >> 44 & 0xF
            cfs.append((op, f))
            if op in (1, 2, 3, 4, 5, 6, 13, 14, 15):
                min_exec = min(min_exec, (f & 0xFFF))
        i += 1
        if len(cfs) >= 2*min_exec//3 + 2 and len(cfs) > 2*1: 
            if 3*i >= min_exec*3: break
    return cfs

if cmd == 'cf':
    # Heuristique : un VS qui contient plus d'une instruction « alloc » de type 2 exporte probablement vers la mémoire
    # (memexport) en plus de ses interpolateurs ; recoupé avec les constantes « out* » de la table de constantes.
    cs = containers(D, BASE)
    ops = collections.Counter(); multi = []
    for c in cs:
        cfs = cf_scan(c)
        for op, f in cfs: ops[op] += 1
        n2 = sum(1 for op, f in cfs if op == 12 and (f >> 40 & 3) == 2)
        if c['kind'] == 'VS' and n2 > 1: multi.append(c)
    print('opcodes CF:', dict(sorted(ops.items())))
    named = sum(1 for c in multi if re.search(rb'out(Tiles|Buffer|Image)', c['data'][:c['vsz']]))
    print('VS à alloc multiples (memexport probable):', len(multi), 'dont constantes out* :', named)

if cmd == 'familles':
    cs = containers(D, BASE)
    wmv = re.compile(rb'updb|cDim|cYUV|outTiles|inResiduals|inTiles|inImage|outImage|cRndCtrl|cPad|cBicubic|cPQuant|cShiftTable|cRounding|g_Image|g_Field|g_Overlay|g_Luma|g_Chroma|g_Sharpen|g_Threshold|g_Sample|YSampler|g_YUV|g_RGB|_yuv|cInterlaced|cPhase|cOffset128|inFlags|outBuffer|img_info|InputFormatData|g_NoiseFilter')
    fam = collections.defaultdict(list)
    for c in cs:
        t = c['data']
        k = 'video' if wmv.search(t[:c['vsz']] + t[c['vsz']+c['ucode_off']+c['ucode_size']:]) else 'ui/autre'
        fam[(k, c['kind'])].append(c)
    for k, v in sorted(fam.items()):
        print(k, len(v), 'ucode moyen', sum(x['ucode_size'] for x in v)//len(v), 'max', max(x['ucode_size'] for x in v))
    for c in fam[('ui/autre', 'VS')] + fam[('ui/autre', 'PS')]:
        n = sorted({x.decode() for x in re.findall(rb'[A-Za-z_][A-Za-z0-9_]{2,30}(?=\x00)', c['data'][:c['vsz']]) if not re.match(rb'(vs|ps)_|Microsoft', x)})
        print(f"{c['off']:08X} {c['kind']} ucode={c['ucode_size']:#x}", n[:6])

def callgraph(fl):
    """bl directs : appelants (nb de sites, nb de fonctions distinctes) par cible."""
    t = D[TEXT[0]-BASE:TEXT[1]-BASE]
    n = len(t)//4
    w = struct.unpack('>%dI' % n, t[:n*4])
    sites = collections.defaultdict(list)
    for i, ins in enumerate(w):
        if ins >> 26 == 18 and ins & 1 and not ins & 2:
            li = ins & 0x03FFFFFC; li = li - 0x04000000 if li & 0x02000000 else li
            a = TEXT[0] + i*4
            sites[a + li].append(a)
    return sites, w

PM4 = {0x10:'NOP',0x21:'REG_RMW',0x22:'DRAW_INDX',0x23:'VIZ_QUERY',0x27:'IM_LOAD',0x2B:'IM_LOAD_IMMEDIATE',0x2D:'SET_CONSTANT',
       0x2E:'LOAD_ALU_CONSTANT',0x2F:'LOAD_ALU_CONSTANT?',0x36:'DRAW_INDX_2',0x3B:'INVALIDATE_STATE',0x3C:'WAIT_REG_MEM',0x3D:'MEM_WRITE',
       0x3E:'REG_TO_MEM',0x3F:'INDIRECT_BUFFER',0x42:'COND_EXEC',0x45:'EVENT_WRITE_SHD',0x46:'EVENT_WRITE',0x47:'EVENT_WRITE_EXT',
       0x48:'ME_INIT',0x4B:'SET_DRAW_INIT_FLAGS',0x4C:'SET_BIN_SELECT_LO',0x4D:'SET_BIN_SELECT_HI',0x4E:'SET_BIN_MASK_LO',0x4F:'SET_BIN_MASK_HI',
       0x55:'SET_CONSTANT2',0x56:'SET_SHADER_CONSTANTS',0x58:'INDIRECT_BUFFER_PFD',0x30:'WAIT_FOR_IDLE?',0x31:'WAIT_FOR_IDLE'}

if cmd == 'pm4':
    fl = funcs(); sites, w = callgraph(fl)
    per = collections.defaultdict(collections.Counter)
    for i, ins in enumerate(w):
        if ins >> 26 == 15 and (ins >> 16 & 31) == 0 and 0xC000 <= (ins & 0xFFFF) <= 0xC0FF:
            rt = ins >> 21 & 31
            for j in range(i+1, min(i+4, len(w))):
                x = w[j]
                if x >> 26 in (14, 24) and (x >> 16 & 31) == rt:
                    op = (x & 0xFFFF) >> 8 & 0xFF
                    f = owner(fl, TEXT[0] + i*4)
                    per[f][PM4.get(op, f'op{op:02X}')] += 1
                    break
    for f, c in sorted(per.items()):
        ncall = len(sites.get(f, []))
        print(f'{f:08X} callers={ncall:3d}', dict(c))

if cmd == 'funcs':
    lo, hi = int(sys.argv[4], 16), int(sys.argv[5], 16)
    fl = funcs(); sites, w = callgraph(fl)
    callers_f = collections.defaultdict(set)
    for tgt, ss in sites.items():
        for s in ss: callers_f[tgt].add(owner(fl, s))
    for a, sz in fl:
        if lo <= a < hi:
            print(f'{a:08X} size={sz:5d} sites={len(sites.get(a,[])):3d} callers={len(callers_f.get(a,())):3d}')

if cmd == 'who':
    fl = funcs(); sites, w = callgraph(fl)
    for t in sys.argv[4:]:
        t = int(t, 16)
        cs = sorted({owner(fl, s) for s in sites.get(t, [])})
        print(f'{t:08X} <- {len(sites.get(t,[]))} sites,', [f'{c:08X}' for c in cs][:24])

if cmd == 'api':
    lo, hi = 0x92120000, 0x92140000
    wmv = (0x92540000, 0x92600000)
    fl = funcs(); sites, w = callgraph(fl)
    inrange = [(a, s) for a, s in fl if lo <= a < hi]
    print('fonctions dans la bibliothèque D3D [%X,%X):' % (lo, hi), len(inrange), 'octets:', sum(s for _, s in inrange))
    rows = []
    for a, s in inrange:
        ext = [owner(fl, x) for x in sites.get(a, []) if not (lo <= x < hi)]
        if ext:
            nw = sum(1 for e in ext if wmv[0] <= e < wmv[1])
            rows.append((a, s, len(ext), len(set(ext)), nw))
    print('points d\'entrée appelés depuis l\'extérieur:', len(rows))
    print('dont appelés par XUI/dash (hors décodeur WMV):', sum(1 for r in rows if r[2] - r[4] > 0))
    for r in sorted(rows, key=lambda r: -r[2]):
        print(f'{r[0]:08X} size={r[1]:5d} sites_ext={r[2]:3d} fonctions_ext={r[3]:3d} dont_WMV={r[4]:3d}')
