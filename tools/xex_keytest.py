"""Teste quelle clé (retail/devkit) déchiffre le premier bloc d'un XEX2 compressé.
usage: xex_keytest.py <file.xex> <retail_key_hex>"""
import sys, struct, hashlib
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
x = open(sys.argv[1], 'rb').read(); retail = bytes.fromhex(sys.argv[2])
hdr = struct.unpack('>I', x[8:12])[0]; sec = struct.unpack('>I', x[0x10:0x14])[0]
n = struct.unpack('>I', x[0x14:0x18])[0]
opt = dict(struct.unpack('>II', x[0x18+i*8:0x20+i*8]) for i in range(n))
v = opt[0x3ff]; enc, comp = struct.unpack('>HH', x[v+4:v+8])
bsize = struct.unpack('>I', x[v+0xC:v+0x10])[0]; bhash = x[v+0x10:v+0x24]
def dec(k, d): c = Cipher(algorithms.AES(k), modes.CBC(b'\0'*16)).decryptor(); return c.update(d) + c.finalize()
print(f'enc={enc} comp={comp} block0={bsize:#x}')
print('plain', hashlib.sha1(x[hdr:hdr+bsize]).digest() == bhash)
for name, k in (('retail', retail), ('devkit', b'\0'*16)):
    sk = dec(k, x[sec+0x150:sec+0x160])
    print(name, hashlib.sha1(dec(sk, x[hdr:hdr+bsize])).digest() == bhash)
