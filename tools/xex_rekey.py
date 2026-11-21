#!/usr/bin/env python3
"""Rechiffre la clé de session d'un XEX2 pour que tools/xexinfo (clé retail) puisse le lire.

Certains modules système (xam.xex de la 2.0.13604...) ont leur clé de session chiffrée avec la clé « XEX1 » (clé publique
connue du format, comme la clé retail) au lieu de la clé retail. Les données ne changent pas : on déchiffre la clé de
session avec la clé qui convient (vérifiée par le SHA-1 du premier bloc) et on la réécrit chiffrée avec la clé retail.
Le hash d'en-tête n'est pas recalculé (le chargeur de XenonRecomp ne le vérifie pas). Sortie : fichier dérivé, à ne pas versionner.

AES-128-CBC (IV nul) sans dépendance obligatoire : le module `cryptography` s'il existe (conteneur de build), sinon
libcrypto (OpenSSL, présent sur presque tout Linux) par ctypes, sinon une implémentation pur Python (lente mais
universelle — l'hôte SteamOS n'a pas `cryptography`, l'import AppImage plantait là, mesure du 2026-11-21).
XEX_REKEY_AES=cryptography|libcrypto|pure pour forcer un fournisseur (tests).

usage: xex_rekey.py <entrée.xex> <sortie.xex>   (code 0 : sortie écrite, éventuellement identique ; 1 : aucune clé ne convient)
"""
import ctypes
import ctypes.util
import hashlib
import os
import struct
import sys

KEYS = {
    "retail": bytes.fromhex("20B185A59D28FDC340583FBB0896BF91"),
    "xex1": bytes.fromhex("A26C10F71FD935E98B99922CE9321572"),
    "devkit": bytes(16),
}

# ---------------------------------------------------------------- fournisseurs AES

def _try_cryptography():
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        return None
    def run(key, data, enc):
        c = Cipher(algorithms.AES(key), modes.CBC(bytes(16)))
        o = c.encryptor() if enc else c.decryptor()
        return o.update(data) + o.finalize()
    return run

def _try_libcrypto():
    noms = [ctypes.util.find_library("crypto"), "libcrypto.so.3", "libcrypto.so.1.1",
            "libcrypto.so.1.0.0", "libcrypto.so"]
    lib = None
    for n in noms:
        if not n:
            continue
        try:
            cand = ctypes.CDLL(n)
            cand.AES_set_encrypt_key  # AttributeError si les symboles legacy sont absents
            lib = cand
            break
        except (OSError, AttributeError):
            continue
    if lib is None:
        return None
    lib.AES_set_encrypt_key.argtypes = (ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    lib.AES_set_decrypt_key.argtypes = (ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p)
    lib.AES_encrypt.argtypes = (ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
    lib.AES_decrypt.argtypes = (ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p)
    AESKEY = ctypes.c_uint32 * 61  # rd_key[60] + rounds (244 octets)
    out = ctypes.create_string_buffer(16)
    def run(key, data, enc):  # CBC à IV nul, fait ici : les primitives OpenSSL sont des ECB
        if len(data) % 16:
            raise ValueError("AES : données non multiples de 16")
        k = AESKEY()
        (lib.AES_set_encrypt_key if enc else lib.AES_set_decrypt_key)(key, 128, k)
        oneway = lib.AES_encrypt if enc else lib.AES_decrypt
        res = bytearray()
        prev = bytes(16)
        for i in range(0, len(data), 16):
            if enc:
                oneway(bytes(a ^ b for a, b in zip(data[i:i + 16], prev)), out, k)
                prev = out.raw
                res += prev
            else:
                oneway(data[i:i + 16], out, k)
                res += bytes(a ^ b for a, b in zip(out.raw, prev))
                prev = data[i:i + 16]
        return bytes(res)
    return run

_SBOX = _INVBOX = None
_X2 = _X3 = _M9 = _M11 = _M13 = _M14 = None

def _gf_mul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B
        b >>= 1
    return p

def _build_tables():
    global _SBOX, _INVBOX, _X2, _X3, _M9, _M11, _M13, _M14
    _SBOX = bytes.fromhex(  # S-box AES (FIPS-197)
        "637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
        "b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
        "09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
        "d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
        "cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
        "e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
        "ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
        "e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")
    inv = bytearray(256)
    for i, v in enumerate(_SBOX):
        inv[v] = i
    _INVBOX = bytes(inv)
    _X2 = tuple(_gf_mul(x, 2) for x in range(256))
    _X3 = tuple(_gf_mul(x, 3) for x in range(256))
    _M9 = tuple(_gf_mul(x, 9) for x in range(256))
    _M11 = tuple(_gf_mul(x, 11) for x in range(256))
    _M13 = tuple(_gf_mul(x, 13) for x in range(256))
    _M14 = tuple(_gf_mul(x, 14) for x in range(256))

_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)

def _expand(key):
    # clés de tour à plat : rk[16*rnd + 4c + r] = mot w[4*rnd+c][r] (AddRoundKey direct)
    w = [list(key[4 * i:4 * i + 4]) for i in range(4)]
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[x] for x in t]
            t[0] ^= _RCON[i // 4 - 1]
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    rk = []
    for rnd in range(11):
        for c in range(4):
            rk += w[4 * rnd + c]
    return rk

def _enc_block(b, rk):
    s = [b[i] ^ rk[i] for i in range(16)]  # AddRoundKey(0)
    for rnd in range(1, 11):
        t = [0] * 16
        for r in range(4):  # SubBytes + ShiftRows : out(r,c) = S(in(r,(c+r)%4))
            for c in range(4):
                t[4 * c + r] = _SBOX[s[4 * ((c + r) & 3) + r]]
        if rnd < 10:  # MixColumns (colonne c = indices 4c..4c+3)
            for c in range(4):
                a = t[4 * c:4 * c + 4]
                t[4 * c + 0] = _X2[a[0]] ^ _X3[a[1]] ^ a[2] ^ a[3]
                t[4 * c + 1] = a[0] ^ _X2[a[1]] ^ _X3[a[2]] ^ a[3]
                t[4 * c + 2] = a[0] ^ a[1] ^ _X2[a[2]] ^ _X3[a[3]]
                t[4 * c + 3] = _X3[a[0]] ^ a[1] ^ a[2] ^ _X2[a[3]]
        s = [t[i] ^ rk[16 * rnd + i] for i in range(16)]
    return s

def _dec_block(b, rk):
    s = [b[i] ^ rk[160 + i] for i in range(16)]  # AddRoundKey(10)
    for rnd in range(9, -1, -1):
        t = [0] * 16
        for r in range(4):  # InvShiftRows + InvSubBytes : out(r,c) = S⁻¹(in(r,(c-r)%4))
            for c in range(4):
                t[4 * c + r] = _INVBOX[s[4 * ((c - r) & 3) + r]]
        for i in range(16):  # AddRoundKey(rnd) AVANT InvMixColumns (ordre standard)
            t[i] ^= rk[16 * rnd + i]
        if rnd > 0:
            for c in range(4):
                a = t[4 * c:4 * c + 4]
                t[4 * c + 0] = _M14[a[0]] ^ _M11[a[1]] ^ _M13[a[2]] ^ _M9[a[3]]
                t[4 * c + 1] = _M9[a[0]] ^ _M14[a[1]] ^ _M11[a[2]] ^ _M13[a[3]]
                t[4 * c + 2] = _M13[a[0]] ^ _M9[a[1]] ^ _M14[a[2]] ^ _M11[a[3]]
                t[4 * c + 3] = _M11[a[0]] ^ _M13[a[1]] ^ _M9[a[2]] ^ _M14[a[3]]
        s = t
    return s

def _run_pure(key, data, enc):
    if len(data) % 16:
        raise ValueError("AES : données non multiples de 16")
    rk = _expand(key)
    out = bytearray()
    prev = bytes(16)  # IV nul
    if enc:
        for i in range(0, len(data), 16):
            blk = bytes(_enc_block([data[i + j] ^ prev[j] for j in range(16)], rk))
            out += blk
            prev = blk
    else:
        for i in range(0, len(data), 16):
            blk = bytes(_dec_block(data[i:i + 16], rk))
            out += bytes(a ^ b for a, b in zip(blk, prev))
            prev = data[i:i + 16]
    return bytes(out)

_BACKEND = None

def _backend():
    global _BACKEND
    if _BACKEND is None:
        want = os.environ.get("XEX_REKEY_AES", "auto")
        order = {"cryptography": ["cryptography"], "libcrypto": ["libcrypto"],
                 "pure": ["pure"],
                 "auto": ["cryptography", "libcrypto", "pure"]}.get(want)
        if order is None:
            raise RuntimeError(f"XEX_REKEY_AES inconnu : {want}")
        impl = None
        for name in order:
            if name == "cryptography":
                impl = _try_cryptography()
            elif name == "libcrypto":
                impl = _try_libcrypto()
            else:
                _build_tables()
                impl = _run_pure
            if impl is not None:
                break
        if impl is None:
            raise RuntimeError("aucun fournisseur AES disponible")
        _BACKEND = (name, impl)
        print(f"AES : {name}", file=sys.stderr)
    return _BACKEND[1]

def aes(key, data, enc=False):
    return _backend()(key, data, enc)

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
