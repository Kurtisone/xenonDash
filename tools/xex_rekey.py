#!/usr/bin/env python3
"""Rechiffre la clé de session d'un XEX2 pour que tools/xexinfo (clé retail) puisse le lire.

Certains modules système (xam.xex de la 2.0.13604...) ont leur clé de session chiffrée avec la clé « XEX1 » (clé publique
connue du format, comme la clé retail) au lieu de la clé retail. Les données ne changent pas : on déchiffre la clé de
session avec la clé qui convient (vérifiée par le SHA-1 du premier bloc) et on la réécrit chiffrée avec la clé retail.
Le hash d'en-tête n'est pas recalculé (le chargeur de XenonRecomp ne le vérifie pas). Sortie : fichier dérivé, à ne pas versionner.

usage: xex_rekey.py <entrée.xex> <sortie.xex>   (code 0 : sortie écrite, éventuellement identique ; 1 : aucune clé ne convient)
"""
import hashlib
import struct
import sys

from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

KEYS = {
    "retail": bytes.fromhex("20B185A59D28FDC340583FBB0896BF91"),
    "xex1": bytes.fromhex("A26C10F71FD935E98B99922CE9321572"),
    "devkit": bytes(16),
}


def aes(key, data, enc=False):
    c = Cipher(algorithms.AES(key), modes.CBC(bytes(16)))
    o = c.encryptor() if enc else c.decryptor()
    return o.update(data) + o.finalize()


def main():
    x = bytearray(open(sys.argv[1], "rb").read())
    hdr, = struct.unpack(">I", x[8:12])
    sec, = struct.unpack(">I", x[0x10:0x14])
    n, = struct.unpack(">I", x[0x14:0x18])
    opt = dict(struct.unpack(">II", x[0x18 + i * 8:0x20 + i * 8]) for i in range(n))
    v = opt[0x3FF]
    enc = struct.unpack(">H", x[v + 4:v + 6])[0]
    if enc == 0:
        open(sys.argv[2], "wb").write(x)
        print("non chiffré")
        return 0
    bsize, = struct.unpack(">I", x[v + 0xC:v + 0x10])
    bhash = bytes(x[v + 0x10:v + 0x24])
    ek = bytes(x[sec + 0x150:sec + 0x160])
    for name, k in KEYS.items():
        sk = aes(k, ek)
        if hashlib.sha1(aes(sk, bytes(x[hdr:hdr + bsize]))).digest() == bhash:
            x[sec + 0x150:sec + 0x160] = aes(KEYS["retail"], sk, enc=True)
            open(sys.argv[2], "wb").write(x)
            print(f"clé {name} -> rechiffrée avec la clé retail")
            return 0
    print("aucune clé connue ne déchiffre ce XEX", file=sys.stderr)
    return 1


sys.exit(main())
