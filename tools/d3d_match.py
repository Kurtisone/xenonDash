#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Retrouve par motif de code les fonctions D3D (XDK liee statiquement) d'un XEX Xbox 360 (P8, recompilateur generique).

Le rendu natif (ngpu_core) remplace des fonctions de la XDK D3D par des crochets. Pour un jeu sans recette il faut leurs adresses
et les decalages du peripherique D3D : ce module les retrouve dans l'image decodee du XEX, sans rien coder en dur.

Principe
  - Chaque instruction PowerPC est reduite a un jeton « lache » (classe d'opcode : registres, immediats, deplacements de branche
    masques) et a un jeton « serre » (classe + registres ; immediats toujours masques, sauf les parametres de rlwinm).
  - Une signature (tools/d3d_signatures/*.json) est la suite de jetons d'une fonction de reference : un masque, jamais le code.
    Elle porte aussi les « champs » a lire (decalage du peripherique charge par telle instruction), des constantes caracteristiques
    et des relations (appelle l'import VdSwap, suit telle autre fonction...).
  - Candidats : index inverse de 5-grammes de jetons sur les fonctions de .pdata, plus recherche exacte du debut de la signature
    (fonctions sans entree .pdata). Verification par alignement (difflib) des jetons ; score = moyenne lache/serre ; champs lus par
    l'alignement ; controles croises (graphe d'appels, constantes, imports du noyau) -> confiance.
  - Le peripherique D3DDevice n'est PAS une adresse generique : le facade le capture a l'execution (r3 du premier appel).

Usage
  d3d_match.py match   <default.xex|image.bin> [--sigs DIR] [--exclude FAMILLE ...]    un role par ligne (adresse, score, champs)
  d3d_match.py analyze <default.xex|image.bin> [...]   rapport JSON : XDK, fonctions, decalages lus, supported, reason
  d3d_match.py compare <default.xex|image.bin> <d3d_profile.toml>   tableau attendu/trouve contre le profil d'une recette
  d3d_match.py learn   <image.bin> --family NOM [--refs references.toml] [--out F]   ecrit tools/d3d_signatures/NOM.json
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "d3d_signatures")
SDK = os.environ.get("XD_SDK", os.path.join(ROOT, "sdk"))
REXGLUE = os.path.join(SDK, "bin", "rexglue")

# Ordinaux xboxkrnl.exe (stables d'une XDK a l'autre) des imports graphiques servant d'ancres.
KERNEL_ORDINALS = {
    "VdEnableRingBufferRPtrWriteBack": 0x1B6, "VdInitializeRingBuffer": 0x1C3, "VdSetSystemCommandBufferGpuIdentifierAddress": 0x1D9,
    "VdPersistDisplay": 0x1C7, "VdSwap": 0x25B, "VdGetSystemCommandBuffer": 0x1BD, "VdSetGraphicsInterruptCallback": 0x1D5,
    "VdInitializeEngines": 0x1C2, "VdRetrainEDRAM": 0x269, "VdQueryVideoMode": 0x1CA, "MmGetPhysicalAddress": 0xBE,
    "VdGetCurrentDisplayGamma": 0x1B9, "VdCallGraphicsNotificationRoutines": 0x1B1,
}
ORDINAL_NAMES = {v: k for k, v in KERNEL_ORDINALS.items()}


# --------------------------------------------------------------------------------------------------
# Image decodee + en-tete XEX
# --------------------------------------------------------------------------------------------------
def xex_options(xex_bytes):
    n = struct.unpack(">I", xex_bytes[0x14:0x18])[0]
    return {k: v for k, v in (struct.unpack(">II", xex_bytes[0x18 + 8 * i:0x20 + 8 * i]) for i in range(n))}


def static_libraries(xex_bytes):
    """[(nom, 'a.b.c.d')] : bibliotheques liees statiquement (D3D9, XGRAPHC, D3DX9...) ; en-tete 0x200FF."""
    opt = xex_options(xex_bytes)
    out = []
    if 0x200FF in opt:
        o = opt[0x200FF]
        size, = struct.unpack(">I", xex_bytes[o:o + 4])
        for j in range((size - 4) // 16):
            e = xex_bytes[o + 4 + 16 * j:o + 20 + 16 * j]
            major, minor, build, qfe = struct.unpack(">HHHH", e[8:])
            out.append((e[:8].decode("latin1").strip("\0 "), f"{major}.{minor}.{build}.{qfe & 0xFF}"))
    return out


def estimate_xdk(libs):
    """Version de la XDK : numero de build de D3D9 (sinon XGRAPHC, sinon XBOXKRNL). Renvoie (build:int|None, texte)."""
    for want in ("D3D9", "XGRAPHC", "XBOXKRNL", "XAPILIB"):
        for name, ver in libs:
            if name == want:
                return int(ver.split(".")[2]), f"{ver} ({want})"
    return None, "inconnue"


def decode_xex(xex, outdir):
    """rexglue dump-xex -> image PE decodee ; renvoie le chemin du .bin."""
    shutil.rmtree(outdir, ignore_errors=True)
    r = subprocess.run([REXGLUE, "dump-xex", xex, outdir], env=dict(os.environ, LD_LIBRARY_PATH=os.path.join(SDK, "lib")),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    bins = [f for f in os.listdir(outdir) if f.endswith(".bin")] if os.path.isdir(outdir) else []
    if r.returncode != 0 or not bins:
        raise RuntimeError("rexglue dump-xex a echoue : " + r.stdout[-500:])
    return os.path.join(outdir, bins[0])


class Image:
    """Image PE decodee (adresses invitees) : sections, fonctions (.pdata), jetons, imports du noyau."""

    def __init__(self, path, xex=None):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path} n'est pas une image PE")
        pe = struct.unpack("<I", d[0x3C:0x40])[0]
        nsec = struct.unpack("<H", d[pe + 6:pe + 8])[0]
        optsz = struct.unpack("<H", d[pe + 20:pe + 22])[0]
        self.base = struct.unpack("<I", d[pe + 24 + 28:pe + 24 + 32])[0]
        self.sections = {}
        for i in range(nsec):
            o = pe + 24 + optsz + 40 * i
            name = d[o:o + 8].rstrip(b"\0").decode("latin1")
            vs, va = struct.unpack("<II", d[o + 8:o + 16])
            self.sections.setdefault(name, (va, vs))
        self.text_lo, tsz = self.sections[".text"]
        self.text_hi = self.text_lo + tsz
        self.xex_bytes = None
        self.libs = []
        self.thunks = {}      # adresse du thunk -> (bibliotheque, ordinal)
        if xex:
            self.xex_bytes = open(xex, "rb").read(0x80000)
            self.libs = static_libraries(self.xex_bytes)
            self._read_imports()
        self._pdata()
        self._tokens()

    # -- imports du noyau (thunks)
    def _read_imports(self):
        x = self.xex_bytes
        opt = xex_options(x)
        if 0x103FF not in opt:
            return
        off = opt[0x103FF]
        _, strsz, nlib = struct.unpack(">III", x[off:off + 12])
        names = [n.decode() for n in x[off + 12:off + 12 + strsz].split(b"\0") if n]
        p = off + 12 + strsz
        for _ in range(nlib):
            size, = struct.unpack(">I", x[p:p + 4])
            nameidx, nimp = struct.unpack(">HH", x[p + 0x24:p + 0x28])
            for j in range(nimp):
                a, = struct.unpack(">I", x[p + 0x28 + j * 4:p + 0x2C + j * 4])
                if not (self.base <= a < self.base + len(self.data)):
                    continue
                rec, = struct.unpack(">I", self.data[a - self.base:a - self.base + 4])
                if rec >> 24 == 1:
                    self.thunks[a] = (names[nameidx], rec & 0xFFFF)
            p += size

    def thunk_name(self, addr):
        t = self.thunks.get(addr)
        if not t:
            return None
        return ORDINAL_NAMES.get(t[1]) if t[0].startswith("xboxkrnl") else None

    # -- fonctions
    def _pdata(self):
        va, vs = self.sections.get(".pdata", (0, 0))
        starts, lens = [], {}
        for i in range(0, vs, 8):
            a, w = struct.unpack(">II", self.data[va + i:va + i + 8])
            if a and self.text_lo + self.base <= a < self.text_hi + self.base:
                starts.append(a)
                lens[a] = (w >> 8 & 0x3FFFFF) * 4
        starts.sort()
        self.fstarts, self.flen = starts, lens

    def func_of(self, addr):
        """(debut, longueur en octets) de la fonction .pdata contenant addr, ou None."""
        i = bisect.bisect_right(self.fstarts, addr) - 1
        if i >= 0:
            s = self.fstarts[i]
            if addr < s + self.flen[s]:
                return s, self.flen[s]
        return None

    def word(self, addr):
        o = addr - self.base
        return struct.unpack(">I", self.data[o:o + 4])[0]

    def words(self, start, count):
        o = start - self.base
        return struct.unpack(f">{count}I", self.data[o:o + 4 * count])

    # -- jetons
    def _tokens(self):
        lo, hi = self.text_lo, self.text_hi
        n = (hi - lo) // 4
        w = struct.unpack(f">{n}I", self.data[lo:lo + 4 * n])
        self.text_words = w
        self.loose = [loose_token(x) for x in w]
        self.tight = [tight_token(x) for x in w]
        self.loose_bytes = b"".join(struct.pack(">H", t) for t in self.loose)

    def idx(self, addr):
        return (addr - self.base - self.text_lo) // 4

    def addr_of(self, idx):
        return self.base + self.text_lo + 4 * idx

    def extent(self, start, ref_len=None, terminator=False):
        """Nombre d'instructions de la fonction commencant a `start` : .pdata, sinon jusqu'a la 1re instruction terminale."""
        if start in self.flen and not terminator:
            return max(1, self.flen[start] // 4)
        i = self.idx(start)
        for k in range(i, min(i + 4096, len(self.text_words))):
            x = self.text_words[k]
            if x in (0x4E800020, 0x4E800420) or (x >> 26 == 18 and not x & 1):
                return k - i + 1
        return ref_len or 1


# --------------------------------------------------------------------------------------------------
# Jetons PowerPC
# --------------------------------------------------------------------------------------------------
def loose_token(w):
    """Classe d'instruction sur 16 bits : opcode primaire (6 bits) + 10 bits (opcode etendu ou indicateurs)."""
    op = w >> 26
    if op in (19, 31):
        return op << 10 | (w >> 1) & 0x3FF
    if op == 63 or op == 59:
        return op << 10 | (w >> 1) & 0x3FF
    if op == 30:
        return op << 10 | (w >> 2) & 7
    if op == 18:
        return op << 10 | w & 1                                  # b / bl
    if op == 16:
        return op << 10 | (w >> 21 & 0x1F) << 1 | w & 1           # BO, LK
    if op in (14, 15):
        return op << 10 | (1 if (w >> 16) & 31 == 0 else 0)       # li/lis contre addi/addis
    if 32 <= op <= 55 or op in (58, 62):
        return op << 10 | (1 if (w >> 16) & 31 == 1 else 0)       # acces memoire : base = pile (r1) ou non
    return op << 10


RLW = (20, 21, 23)    # rlwimi, rlwinm, rlwnm : SH/MB/ME gardes


def tight_token(w):
    """Jeton serre sur 32 bits : classe + champs de registres ; immediats et deplacements masques."""
    op = w >> 26
    c = loose_token(w)
    if op in (19, 31, 63, 59, 30):
        rest = w & 0x03FFF800                                      # RT/RS, RA, RB
    elif op == 18:
        rest = 0
    elif op == 16:
        rest = w & 0x001F0000                                      # BI
    elif op in RLW:
        rest = w & 0x03FFFFFE                                      # registres + SH/MB/ME
    else:
        rest = w & 0x03FF0000                                      # RT/RS + RA
    return (c << 16 ^ rest * 2654435761) & 0xFFFFFFFF


def disp_of(w):
    """Deplacement signe d'un acces memoire en forme D (lwz, stw, lbz, ...) ou None."""
    op = w >> 26
    if 32 <= op <= 55:
        v = w & 0xFFFF
        return v - 0x10000 if v & 0x8000 else v
    return None


def immediates(w):
    """Immediats 'caracteristiques' d'une instruction (li/ori/cmpwi/lis...) pour les controles par constantes."""
    op = w >> 26
    if op in (7, 14, 24, 25, 15, 10, 11, 12, 13, 28, 29):
        v = w & 0xFFFF
        if op in (7, 14, 11, 12, 13, 10) and v & 0x8000:
            v -= 0x10000
        return v
    return None


def branch_target(addr, w):
    op = w >> 26
    if op == 18 and not w & 2:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return addr + li
    if op == 18:
        return w & 0x03FFFFFC
    return None


# --------------------------------------------------------------------------------------------------
# Signatures
# --------------------------------------------------------------------------------------------------
def load_signatures(sigdir=SIGDIR, exclude=()):
    sigs = []
    if os.path.isdir(sigdir):
        for f in sorted(os.listdir(sigdir)):
            if f.endswith(".json"):
                s = json.load(open(os.path.join(sigdir, f)))
                if s["family"] not in exclude:
                    sigs.append(s)
    return sigs


def build_signature(img, start, role, fields=None, extra=None, terminator=False):
    """Signature d'une fonction de reference : jetons masques + metadonnees (champs a lire, appels, constantes)."""
    n = img.extent(start, terminator=terminator)
    i = img.idx(start)
    words = img.text_words[i:i + n]
    loose = img.loose[i:i + n]
    tight = img.tight[i:i + n]
    consts = sorted({v & 0xFFFF for v in (immediates(w) for w in words) if v is not None and abs(v) >= 0x100
                     and (v & 0xFFFF) not in (0x8000,)})
    imports = []
    calls = []
    for k, w in enumerate(words):
        a = start + 4 * k
        if w >> 26 == 18 and w & 1:
            t = branch_target(a, w)
            nm = img.thunk_name(t)
            if nm:
                imports.append(nm)
            else:
                calls.append(t)
    sig = {"role": role, "n": n, "loose": loose, "tight": [t & 0xFFFFFFFF for t in tight],
           "consts": consts, "imports": sorted(set(imports)), "ref_calls": calls}
    if fields:
        sig["fields"] = {}
        for name, (value, kind) in fields.items():
            # index de la 1re instruction dont le deplacement (ou l'immediat) vaut `value`
            for k, w in enumerate(words):
                v = disp_of(w) if kind == "disp" else immediates(w)
                if v == value:
                    sig["fields"][name] = {"index": k, "kind": kind, "ref": value}
                    break
    if extra:
        sig.update(extra)
    return sig


class Matcher:
    NGRAM = 5

    def __init__(self, img, sigs):
        self.img = img
        self.sigs = sigs
        self._index()

    def _index(self):
        img = self.img
        lo = img.loose
        self.inv = collections.defaultdict(list)     # n-gramme -> [indice de fonction]
        self.fn = []                                 # (debut, nb instructions)
        for s in img.fstarts:
            i = img.idx(s)
            n = img.flen[s] // 4
            if n < 1 or i < 0 or i + n > len(lo):
                continue
            fid = len(self.fn)
            self.fn.append((s, n))
            seen = set()
            for k in range(i, i + n - self.NGRAM + 1):
                g = hash(tuple(lo[k:k + self.NGRAM]))
                if g not in seen:
                    seen.add(g)
                    self.inv[g].append(fid)

    # -- candidats d'une signature
    def candidates(self, sig, limit=12):
        img = self.img
        ref = sig["loose"]
        n = len(ref)
        cands = {}
        if n >= self.NGRAM + 2:
            score = collections.Counter()
            grams = {hash(tuple(ref[k:k + self.NGRAM])) for k in range(n - self.NGRAM + 1)}
            for g in grams:
                lst = self.inv.get(g)
                if not lst or len(lst) > 400:
                    continue
                w = 1.0 / len(lst)
                for fid in lst:
                    score[fid] += w
            for fid, _ in score.most_common(limit):
                s, m = self.fn[fid]
                cands[s] = m
        # debut exact (fonctions sans entree .pdata, fonctions minuscules) : recherche des k premiers jetons
        k = min(n, 14 if n > 14 else n)
        pat = b"".join(struct.pack(">H", t) for t in ref[:k])
        pos = 0
        found = 0
        while found < 400:
            p = img.loose_bytes.find(pat, pos)
            if p < 0:
                break
            pos = p + 1
            if p % 2:
                continue
            idx = p // 2
            a = img.addr_of(idx)
            found += 1
            cands.setdefault(a, img.extent(a, n) if a in img.flen else n)
        return cands

    def verify(self, sig, start, count):
        img = self.img
        i = img.idx(start)
        count = max(1, min(count, int(len(sig["loose"]) * 2.5) + 8))
        cl = img.loose[i:i + count]
        ct = img.tight[i:i + count]
        sm = difflib.SequenceMatcher(None, sig["loose"], cl, autojunk=False)
        blocks = sm.get_matching_blocks()
        s_loose = sm.ratio()
        st = difflib.SequenceMatcher(None, sig["tight"], ct, autojunk=False)
        s_tight = st.ratio()
        mapping = {}
        for b in blocks:
            for d in range(b.size):
                mapping[b.a + d] = b.b + d
        return s_loose, s_tight, mapping

    def best(self, sig, topn=3):
        out = []
        for start, count in self.candidates(sig).items():
            if start < self.img.base + self.img.text_lo:
                continue
            l, t, mapping = self.verify(sig, start, count)
            score = 0.5 * l + 0.5 * t
            out.append({"addr": start, "score": round(score, 4), "loose": round(l, 4), "tight": round(t, 4),
                        "count": count, "mapping": mapping})
        out.sort(key=lambda r: -r["score"])
        return out[:topn]


# --------------------------------------------------------------------------------------------------
# Controles croises
# --------------------------------------------------------------------------------------------------
def callees(img, start, count):
    out = []
    i = img.idx(start)
    for k in range(count):
        w = img.text_words[i + k]
        if w >> 26 == 18 and w & 1:
            out.append(branch_target(start + 4 * k, w))
    return out


def check_candidate(img, sig, cand):
    """Verifie imports du noyau appeles (directement ou un niveau plus bas) et constantes caracteristiques. -> (ok, total, details)"""
    ok, total, notes = 0, 0, []
    start, count = cand["addr"], cand["count"]
    direct = callees(img, start, count)
    names = {img.thunk_name(t) for t in direct}
    deeper = set()
    for t in direct:
        if t and img.thunk_name(t) is None and img.base + img.text_lo <= t < img.base + img.text_hi:
            f = img.extent(t)
            deeper |= {img.thunk_name(x) for x in callees(img, t, min(f, 600))}
    for imp in sig.get("imports", []):
        total += 1
        if imp in names or imp in deeper:
            ok += 1
        else:
            notes.append(f"import {imp} absent")
    if sig.get("consts"):
        i = img.idx(start)
        have = {v & 0xFFFF for v in (immediates(w) for w in img.text_words[i:i + count]) if v is not None}
        # constantes caracteristiques : proportion retrouvee (version-dependantes en partie : on en exige la moitie)
        inter = len(have & set(sig["consts"]))
        total += 1
        if inter * 2 >= len(sig["consts"]):
            ok += 1
        else:
            notes.append(f"constantes {inter}/{len(sig['consts'])}")
    return ok, total, notes


def read_fields(img, sig, cand):
    """Lit les decalages declares par la signature a travers l'alignement : {nom: valeur|None}."""
    out = {}
    for name, f in sig.get("fields", {}).items():
        j = cand["mapping"].get(f["index"])
        v = None
        if j is not None:
            w = img.text_words[img.idx(cand["addr"]) + j]
            v = disp_of(w) if f["kind"] == "disp" else immediates(w)
        out[name] = v
    return out


# --------------------------------------------------------------------------------------------------
# Appariement complet
# --------------------------------------------------------------------------------------------------
def confidence(score, margin, ok, total):
    c = score
    if total:
        c *= 0.6 + 0.4 * ok / total
    if margin is not None and margin < 0.02:
        c *= 0.8
    return round(c, 3)


def match_image(img, sigs):
    """Pour chaque role, meilleure fonction parmi toutes les familles de signatures. Renvoie {role: resultat}.

    Deux passes : meilleurs candidats par role, puis resolution des relations (« commence apres la fin de X », « apres X »)
    et de l'unicite (une fonction ne sert qu'un role)."""
    mt = Matcher(img, sigs)
    roles = collections.OrderedDict()
    for s in sigs:
        for role, sig in s["signatures"].items():
            roles.setdefault(role, []).append((s["family"], sig))
    pools = {}
    for role, lst in roles.items():
        pool = {}
        for fam, sig in lst:
            for r in mt.best(sig, topn=4):
                ok, total, notes = check_candidate(img, sig, r)
                entry = dict(r, family=fam, sig=sig, ok=ok, total=total, notes=notes)
                if r["addr"] not in pool or entry["score"] > pool[r["addr"]]["score"]:
                    pool[r["addr"]] = entry
        pools[role] = sorted(pool.values(), key=lambda e: -e["score"])
    # passe 1 : meilleur de chaque role
    chosen = {r: p[0] for r, p in pools.items() if p}
    # passe 2 : relations ; on ajoute le candidat « juste apres l'ancre » et on bonifie ceux qui respectent la relation
    for _ in range(2):
        for role, pool in pools.items():
            rel = next((sg["relation"] for _, sg in roles[role] if "relation" in sg), None)
            if not rel or rel["role"] not in chosen:
                continue
            anchor = chosen[rel["role"]]
            if rel["kind"] == "after_end":
                want = anchor["addr"] + 4 * anchor["count"]
                if not any(e["addr"] == want for e in pool):
                    sig = roles[role][0][1]
                    l, t, mapping = mt.verify(sig, want, len(sig["loose"]))
                    pool.append({"addr": want, "score": round(0.5 * l + 0.5 * t, 4), "loose": round(l, 4), "tight": round(t, 4),
                                 "count": len(sig["loose"]), "mapping": mapping, "family": roles[role][0][0], "sig": sig,
                                 "ok": 0, "total": 0, "notes": []})
            if rel["kind"] == "callee_of":
                sig = roles[role][0][1]
                for t in set(callees(img, anchor["addr"], anchor["count"])):
                    if img.base + img.text_lo <= t < img.base + img.text_hi and not any(e["addr"] == t for e in pool):
                        n = img.extent(t, len(sig["loose"]))
                        l, tt, mapping = mt.verify(sig, t, n)
                        pool.append({"addr": t, "score": round(0.5 * l + 0.5 * tt, 4), "loose": round(l, 4), "tight": round(tt, 4),
                                     "count": n, "mapping": mapping, "family": roles[role][0][0], "sig": sig,
                                     "ok": 0, "total": 0, "notes": []})
            for e in pool:
                if rel["kind"] == "callee_of":
                    good = e["addr"] in set(callees(img, anchor["addr"], anchor["count"]))
                elif rel["kind"] == "after_end":
                    good = e["addr"] == anchor["addr"] + 4 * anchor["count"]
                else:
                    good = e["addr"] > anchor["addr"]
                e["rel_ok"] = good
                e["rank"] = e["score"] + (0.5 if good and rel["kind"] in ("after_end", "callee_of") else 0.05 if good else 0.0)
            pool.sort(key=lambda e: -e.get("rank", e["score"]))
            chosen[role] = pool[0]
    # unicite : par score decroissant, une fonction ne sert qu'un role
    used = {}
    for role in sorted(chosen, key=lambda r: -chosen[r]["score"]):
        for e in pools[role]:
            if e["addr"] not in used:
                used[e["addr"]] = role
                chosen[role] = e
                break
        else:
            del chosen[role]
    result = {}
    for role, top in chosen.items():
        # une fonction deja retenue par un autre role n'est pas une ambiguite (jumeaux Resolve/ResolveInner de la famille dash-13604)
        others = [e for e in pools[role] if e["addr"] != top["addr"] and used.get(e["addr"], role) == role]
        margin = None if not others else round(top["score"] - others[0]["score"], 4)
        rel_ok = top.get("rel_ok")
        fields = read_fields(img, top["sig"], top)
        conf = confidence(top["score"], margin, top["ok"], top["total"])
        if rel_ok:
            conf = round(min(1.0, conf + 0.05), 3)
        result[role] = {"addr": top["addr"], "score": top["score"], "loose": top["loose"], "tight": top["tight"],
                        "family": top["family"], "margin": margin, "checks": f"{top['ok']}/{top['total']}",
                        "notes": top["notes"], "fields": fields, "relation": rel_ok, "count": top["count"],
                        "confidence": conf, "alternatives": [(hex(e["addr"]), e["score"]) for e in others[:2]]}
    return result


# Roles indispensables au rendu natif generique (crochets de la facade) et seuil de confiance
REQUIRED_ROLES = ["DrawVertices", "DrawIndexedVertices", "Clear", "Resolve", "Swap", "CreateVertexShader", "CreatePixelShader",
                  "VblankHandler", "WaitFence", "WaitDrain", "WaitSecond"]
# Roles crochetes seulement s'ils sont retrouves avec confiance : un jeu peut ne pas les contenir (BeginTiling : rendu sans tuiles
# predicatees ; Begin/EndVertices fondus dans le code du jeu par l'editeur de liens ; GpuBeginShaderConstantF4 : jeux qui
# ecrivent leurs constantes directement dans le tampon de commandes). Leur absence est signalee (`optional_missing`).
OPTIONAL_ROLES = ["BeginVertices", "EndVertices", "BeginTiling", "GpuBeginShaderConstantF4"]
MIN_CONFIDENCE = 0.85
FIELD_MIN_CONFIDENCE = 0.75    # lecture d'un decalage : un jumeau proche (Set{Vertex,Pixel}ShaderConstantF) baisse la confiance sans fausser le champ
# Fonctions XAPI du jeu que le SDK remplace par les siennes (section [rexcrt] du codegen) : les fibres. Sans cela SwitchToFiber
# recompile change la pile invitee sous une pile hote inchangee. Appliquees seulement si les cinq sont retrouvees.
REXCRT_ROLES = ["ConvertThreadToFiber", "ConvertFiberToThread", "CreateFiber", "DeleteFiber", "SwitchToFiber"]
REXCRT_MIN_CONFIDENCE = 0.9
# Disposition canonique de l'ombre du peripherique (ngpu_core/src/xenos_decode.h, XDK 2.0.5632). Depuis le lot B2 le noyau recoit un
# instantane canonique que la facade tire de la disposition lue ici (ngpu_core/src/device_layout.h) : une valeur differente n'est
# plus un refus, une valeur non lue est supposee canonique et signalee (`layout.assumed`).
CANONICAL_LAYOUT = {"vertex_shader": 12688, "pixel_shader": 12684, "index_buffer": 12428, "fetch_base_slot": 48,
                    "fetch_stride": 24, "vertex_constants_slot": 120, "pixel_constants_slot": 376,
                    "reg_surface_info": 10368, "reg_copy_dest_base": 10780}
CANONICAL_GROUPS = [10368, 10444, 10528, 10548, 10596, 10680, 10832]     # groupes de registres 0x2000 ... 0x2380
REQUIRED_OFFSETS = ["vertex_shader", "pixel_shader", "index_buffer"]    # doivent etre lus dans le code du jeu
FREE_OFFSETS = ["fence_ptr", "fence_value", "drain_ptr_word", "second_word"]


def plausible_offset(name, v):
    if v is None:
        return False
    if name in ("fetch_base_slot", "vertex_constants_slot", "pixel_constants_slot"):
        return 0 < v < 1024
    if name == "fetch_stride":
        return v == 24
    if name in ("drain_ptr_word", "second_word"):
        return 0 <= v < 256 and not v & 3
    return 0 < v < 0x8000 and not v & 3


def device_layout(offsets):
    """Disposition de l'ombre (decalages en octets dans le D3DDevice) d'apres les champs lus ; `assumed` = valeurs non lues."""
    assumed = []

    def get(name):
        v = offsets.get(name)
        if v is None:
            assumed.append(name)
            return CANONICAL_LAYOUT[name]
        return v
    stride = get("fetch_stride")
    lay = {"fetch_constants": get("fetch_base_slot") * stride}
    for k in ("vertex_constants", "pixel_constants"):      # en octets (lus dans BeginVertices) ou en fentes de 16 octets (Set*ConstantF)
        lay[k] = offsets[k] if offsets.get(k) is not None else get(k + "_slot") * 16
    lay["vertex_bool_constants"] = lay["pixel_constants"] + 0x1000
    lay["pixel_bool_constants"] = lay["pixel_constants"] + 0x1010
    d_low = get("reg_surface_info") - CANONICAL_LAYOUT["reg_surface_info"]        # groupes 0x2000 a 0x2280
    d_high = get("reg_copy_dest_base") - CANONICAL_LAYOUT["reg_copy_dest_base"]    # groupes 0x2300 et 0x2380
    lay["register_groups"] = [g + (d_low if i < 5 else d_high) for i, g in enumerate(CANONICAL_GROUPS)]
    for k in ("index_buffer", "pixel_shader", "vertex_shader"):
        lay[k] = get(k)
    lay["assumed"] = assumed
    lay["canonical"] = all(lay[k] == v for k, v in (("fetch_constants", 0x480), ("vertex_constants", 0x780), ("pixel_constants", 0x1780),
                                                    ("register_groups", CANONICAL_GROUPS), ("index_buffer", 12428),
                                                    ("pixel_shader", 12684), ("vertex_shader", 12688)))
    return lay


def analyze(img, sigs):
    """Rapport complet : fonctions, decalages lus, disposition de l'ombre, verdict `supported` et raison."""
    res = match_image(img, sigs)
    # un champ peut etre lu par plusieurs roles : la valeur plausible du role le plus sur l'emporte
    offsets = {}
    for role, r in res.items():
        for k, v in r["fields"].items():
            if plausible_offset(k, v) and r["confidence"] >= FIELD_MIN_CONFIDENCE and (k not in offsets or r["confidence"] > offsets[k][2]):
                offsets[k] = (v, role, r["confidence"])
    roles_out = {}
    for role, r in res.items():
        roles_out[role] = {"addr": f"0x{r['addr']:08X}", "score": r["score"], "confidence": r["confidence"], "family": r["family"],
                           "checks": r["checks"], "margin": r["margin"]}
    sure = lambda r: r in res and res[r]["confidence"] >= MIN_CONFIDENCE
    missing = [r for r in REQUIRED_ROLES if r not in res]
    weak = [r for r in REQUIRED_ROLES if r in res and not sure(r)]
    optional_missing = [r for r in OPTIONAL_ROLES if not sure(r)]
    if sure("EndVertices") and not (sure("BeginVertices") and res["EndVertices"].get("relation")):
        optional_missing.append("EndVertices")     # fonction minuscule : n'a de sens qu'a la suite de BeginVertices
    reasons = []
    if missing:
        reasons.append("fonctions D3D introuvables : " + ", ".join(missing))
    if weak:
        reasons.append("fonctions D3D a faible confiance (< %.2f) : " % MIN_CONFIDENCE +
                       ", ".join(f"{r} ({res[r]['confidence']:.2f})" for r in weak))
    # famille expérimentale (rendu natif non validé) : le verdict la refuse, comme un rôle manquant
    # (seulement pour la XDK de la famille : une fonction de Skate peut aussi correspondre à une signature de Trials Fusion)
    xdk_build, xdk_text = estimate_xdk(img.libs)
    experimental = {s["family"] for s in sigs if s.get("experimental") and s.get("xdk_build") == xdk_build}
    used_exp = sorted({r["family"] for r in res.values()} & experimental)
    if used_exp:
        reasons.append(f"XDK {xdk_text} : signatures expérimentales ({', '.join(used_exp)}), rendu natif non validé")
    flat = {k: v[0] for k, v in offsets.items()}
    unread = [k for k in REQUIRED_OFFSETS + FREE_OFFSETS if k not in flat]
    if unread:
        reasons.append("decalages du peripherique non lus dans le code du jeu : " + ", ".join(unread))
    layout = device_layout(flat)
    return {"xdk": estimate_xdk(img.libs)[1], "libs": [f"{n} {v}" for n, v in img.libs if n in ("D3D9", "XGRAPHC", "D3DX9", "XBOXKRNL")],
            "roles": roles_out, "offsets": flat, "offset_sources": {k: v[1] for k, v in offsets.items()}, "layout": layout,
            "missing": missing, "weak": weak, "optional_missing": sorted(set(optional_missing)),
            "supported": not reasons, "reason": " ; ".join(reasons), "_raw": res}


PROFILE_ROLE = {"BeginVertices": "BeginVertices", "DrawIndexedVertices": "DrawIndexedVertices", "DrawVertices": "DrawVertices",
                "EndVertices": "EndVertices", "ClearF": "Clear", "BeginTiling": "BeginTiling", "Resolve": "Resolve", "Swap": "Swap",
                "CreateVertexShader": "CreateVertexShaderInner", "CreatePixelShader": "CreatePixelShaderInner",
                "SetPixelShader": "SetPixelShader", "SetVertexShader": "SetVertexShader", "vblank_handler": "VblankHandler",
                "wait_fence": "WaitFence", "wait_drain": "WaitDrain", "wait_second": "WaitSecond"}


def compare_profile(report, profile_path):
    """Tableau attendu/trouve contre un d3d_profile.toml (recette) : [(nom, attendu, trouve, ok)]."""
    prof = tomllib.load(open(profile_path, "rb"))
    rows = []
    want = dict(prof.get("functions", {}))
    for k in ("vblank_handler", "wait_fence", "wait_drain", "wait_second"):
        want[k] = prof["null_gpu"][k]
    for key, a in want.items():
        role = PROFILE_ROLE.get(key)
        got = report["roles"].get(role, {}).get("addr") if role else None
        rows.append((f"{key} -> {role}", a.upper().replace("0X", "0x"), got, bool(got) and int(got, 16) == int(a, 16)))
    for key in ("fence_ptr", "fence_value", "drain_ptr_word", "second_word"):
        rows.append((f"null_gpu.{key}", prof["null_gpu"][key], report["offsets"].get(key), prof["null_gpu"][key] == report["offsets"].get(key)))
    for k, v in CANONICAL_LAYOUT.items():
        rows.append((f"ombre.{k}", v, report["offsets"].get(k), v == report["offsets"].get(k)))
    return rows


def learn(img, family, refs_path, section=None):
    """Construit le fichier de signatures d'une famille a partir de references.toml (adresses connues) et d'une image."""
    refs = tomllib.load(open(refs_path, "rb"))[section or family]
    sigs = {}
    fields_by_role = collections.defaultdict(dict)
    for name, (role, value, kind) in refs.get("fields", {}).items():
        fields_by_role[role][name] = (value, kind)
    term = set(refs.get("terminator", []))
    for role, addr in refs["roles"].items():
        extra = {"ref_addr": addr}
        if role in refs.get("relations", {}):
            kind, other = refs["relations"][role][:2]
            extra["relation"] = {"kind": kind, "role": other}
        sigs[role] = build_signature(img, addr, role, fields_by_role.get(role), extra, terminator=role in term)
    nd, ver = estimate_xdk(img.libs)
    return {"family": family, "description": refs.get("description", ""), "xdk": ver, "xdk_build": nd, "signatures": sigs,
            "experimental": bool(refs.get("experimental", False))}


def load_image(source, work=None):
    """Chemin d'un .xex (decode par rexglue) ou d'une image .bin/.pe deja decodee -> Image."""
    if source.lower().endswith((".bin", ".pe")):
        xex = None
        cand = os.path.join(os.path.dirname(source), "default.xex")
        return Image(source, xex=cand if os.path.isfile(cand) else None)
    work = work or tempfile.mkdtemp(prefix="d3dmatch-")
    path = decode_xex(source, os.path.join(work, "image"))
    return Image(path, xex=source)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("match")
    m.add_argument("source")
    m.add_argument("--sigs", default=SIGDIR)
    m.add_argument("--exclude", action="append", default=[])
    m.add_argument("--json", action="store_true")
    a_ = sub.add_parser("analyze")
    a_.add_argument("source")
    a_.add_argument("--sigs", default=SIGDIR)
    a_.add_argument("--exclude", action="append", default=[])
    c_ = sub.add_parser("compare")
    c_.add_argument("source")
    c_.add_argument("profile")
    c_.add_argument("--sigs", default=SIGDIR)
    c_.add_argument("--exclude", action="append", default=[])
    l = sub.add_parser("learn")
    l.add_argument("source")
    l.add_argument("--family", required=True)
    l.add_argument("--refs", default=os.path.join(SIGDIR, "references.toml"))
    l.add_argument("--out", default=None)
    args = ap.parse_args()
    if args.cmd == "learn":
        img = load_image(args.source)
        out = learn(img, args.family, args.refs)
        path = args.out or os.path.join(SIGDIR, args.family + ".json")
        with open(path, "w") as f:
            json.dump(out, f, separators=(",", ":"))
        print(f"{len(out['signatures'])} signatures -> {path} ({os.path.getsize(path)} octets)")
        return 0
    if args.cmd == "compare":
        img = load_image(args.source)
        rep = analyze(img, load_signatures(args.sigs, args.exclude))
        rows = compare_profile(rep, args.profile)
        for name, want, got, ok in rows:
            print(f"{'OK ' if ok else 'ECART'} {name:42s} attendu {want!s:12s} trouve {got}")
        print(f"{sum(r[3] for r in rows)}/{len(rows)} identiques")
        return 0 if all(r[3] for r in rows) else 1
    if args.cmd == "analyze":
        img = load_image(args.source)
        rep = analyze(img, load_signatures(args.sigs, args.exclude))
        rep.pop("_raw")
        print(json.dumps(rep, indent=1))
        return 0
    if args.cmd == "match":
        img = load_image(args.source)
        res = match_image(img, load_signatures(args.sigs, args.exclude))
        for role, r in res.items():
            print(f"{role:28s} {r['addr']:08X} score {r['score']:.3f} (lache {r['loose']:.3f} serre {r['tight']:.3f}) "
                  f"conf {r['confidence']:.3f} marge {r['margin']} controles {r['checks']} champs {r['fields']}")


if __name__ == "__main__":
    sys.exit(main())
