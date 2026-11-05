import sys, struct, os
d = open(sys.argv[1],'rb').read(); out = sys.argv[2] if len(sys.argv)>2 else None
assert d[:4] in (b'PIRS',b'LIVE',b'CON ')
con = d[:4]==b'CON '
hsize = struct.unpack('>I', d[0x340:0x344])[0]
sep = d[0x37B]
ft_count = struct.unpack('<H', d[0x37C:0x37E])[0]
ft_block = int.from_bytes(d[0x37E:0x381],'little')
shift = 1 if ((hsize+0xFFF)&0xB000)==0xB000 else (0 if sep&1 else 1)
def blocknum(i):
    def b(x):
        return (x << shift) if con else x
    r = b((i+0xAA)//0xAA)+i
    if i>=0xAA:
        r += b((i+0x70E4)//0x70E4)
        if i>=0x70E4: r += b((i+0x4AF768)//0x4AF768)
    return r
def off(i): return ((hsize+0xFFF)&0xF000) + (blocknum(i)<<12)
ents=[]
for k in range(ft_count):
    blk = d[off(ft_block+k):off(ft_block+k)+0x1000]
    for j in range(0,0x1000,0x40):
        e=blk[j:j+0x40]; fl=e[0x28]
        if fl==0: continue
        n=e[:fl&0x3F].decode('latin1')
        ents.append(dict(name=n,flags=fl,start=int.from_bytes(e[0x2F:0x32],'little'),
            path=struct.unpack('>h',e[0x32:0x34])[0],size=struct.unpack('>I',e[0x34:0x38])[0]))
def full(e):
    p=[e['name']]
    while e['path']!=-1: e=ents[e['path']]; p.insert(0,e['name'])
    return '/'.join(p)
for e in ents:
    fp=full(e); isdir=e['flags']&0x80
    print(('D ' if isdir else 'F ')+f"{e['size']:>10} {'C' if e['flags']&0x40 else '-'} {fp}")
    if out and not isdir:
        dst=os.path.join(out,fp); os.makedirs(os.path.dirname(dst),exist_ok=True)
        nb=(e['size']+0xFFF)//0x1000; buf=b''.join(d[off(e['start']+k):off(e['start']+k)+0x1000] for k in range(nb))
        open(dst,'wb').write(buf[:e['size']])
