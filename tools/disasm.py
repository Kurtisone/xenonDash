"""Désassemble une plage de l'image décompressée via llvm-mc.
usage: disasm.py <image.pe> <base_hex> <start_hex> <end_hex>"""
import sys, subprocess, struct
img, base, start, end = sys.argv[1], *(int(x,16) for x in sys.argv[2:5])
d = open(img,'rb').read()[start-base:end-base]
words = [d[i:i+4] for i in range(0, len(d) - len(d)%4, 4)]
for i, w in enumerate(words):
    r = subprocess.run(['llvm-mc','--disassemble','-triple=powerpc64-unknown-linux-gnu','-mcpu=pwr7'],
                       input=' '.join(f'0x{b:02x}' for b in w), capture_output=True, text=True)
    asm = ' '.join(l.strip() for l in r.stdout.splitlines() if l.startswith('\t') and not l.strip().startswith('.'))
    a = start + i*4
    # résoudre les cibles de branchement relatives
    ins = struct.unpack('>I', w)[0]; op = ins >> 26
    if op == 18:
        li = ins & 0x03FFFFFC; li = li - 0x04000000 if li & 0x02000000 else li
        asm += f'   -> {a+li:#x}' if not ins & 2 else ''
    elif op == 16:
        bd = ins & 0xFFFC; bd = bd - 0x10000 if bd & 0x8000 else bd
        asm += f'   -> {a+bd:#x}' if not ins & 2 else ''
    print(f'{a:08X}  {ins:08X}  {asm or "<?>"}')
